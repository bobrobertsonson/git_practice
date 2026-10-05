// Tests of the Record + Match pieces of the plugin (docs/specs/phase6a_record_match_plugin.md): the DI take
// recorder (ring, writer thread, WAV, sidecar), the match / export job runner driven against a fake child
// process, the match plan, and audition / A-B / apply through the processor's loader. Audio and the fake
// tools are synthesised into temp dirs at test time; nothing is committed.
#include <signal.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>

#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cmath>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <thread>

#include "JobRunner.h"
#include "MatchGlue.h"
#include "PresetAudition.h"
#include "TakeRecorder.h"
#include "fake_tools.h"
#include "processor_harness.h"
#include "onnx_synth.h"
#include "sawblade/sha256.h"
#include "sawblade/wav_io.h"

namespace {

using namespace std::chrono_literals;

constexpr double kFs = 48000.0;

// Anything that falls back to the default data folder (settings, takes, jobs) stays inside a temp dir.
[[maybe_unused]] const bool kDataDirSet = [] {
  const fs::path d = fs::temp_directory_path() / ("sawblade_plugin_tests_data_" + std::to_string(::getpid()));
  ::setenv("SAWBLADE_DATA_DIR", d.c_str(), 1);
  return true;
}();

// A deterministic, non-trivial signal that is exactly reproducible.
std::vector<float> signal(std::size_t n, unsigned seed = 1) {
  std::mt19937 g(seed);
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) x[i] = 0.4f * std::sin(0.0173f * static_cast<float>(i)) + 0.05f * (static_cast<float>(g() % 2001) / 1000.0f - 1.0f);
  return x;
}

std::vector<float> readMono(const fs::path& wav, double* rate = nullptr) {
  const sawblade::AudioFile f = sawblade::readWav(wav);
  REQUIRE(f.channels == 1);
  if (rate) *rate = f.sampleRate;
  return f.interleaved;
}

json readJson(const fs::path& p) {
  std::ifstream f(p);
  return json::parse(f, nullptr, false);
}

bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 15000ms) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return pred();
}

struct FakeHead : juce::AudioPlayHead {
  bool playing = false;
  std::int64_t pos = 0;
  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo i;
    i.setIsPlaying(playing);
    i.setTimeInSamples(pos);
    return i;
  }
};

// Runs `blocks` blocks of `n` samples of the signal `x` (starting at *at) through the host; the head advances.
void runBlocks(Host& h, const std::vector<float>& x, std::size_t& at, int blocks, int n, FakeHead* head = nullptr) {
  std::vector<float> out(static_cast<std::size_t>(n)), out2(static_cast<std::size_t>(n));
  for (int b = 0; b < blocks; ++b) {
    h.process(x.data() + at, out.data(), n, nullptr, out2.data());
    at += static_cast<std::size_t>(n);
    if (head && head->playing) head->pos += n;
  }
}

fs::path writeSong(const fs::path& root, const std::string& name, std::size_t samples, bool guitar = false) {
  const fs::path d = root / name;
  fs::create_directories(d);
  std::vector<float> l(samples), r(samples);
  for (std::size_t i = 0; i < samples; ++i) {
    l[i] = static_cast<float>(i + 1) * 1e-7f;
    r[i] = -0.5f * l[i];
  }
  sawblade::writeWavFloat32Stereo(d / "drums.wav", kFs, l, r);
  sawblade::writeWavFloat32Stereo(d / "other.wav", kFs, l, r);
  if (guitar) sawblade::writeWavFloat32Stereo(d / "guitar.wav", kFs, l, r);
  return d;
}

// Finishes the take in progress: stop(), one more block (the audio thread ends the take at a block boundary),
// then waits for the writer.
void finishTake(Host& h) {
  h.p.recorder().stop();
  std::vector<float> in(64, 0.0f), out(64);
  h.process(in.data(), out.data(), 64);
  REQUIRE(h.p.recorder().waitIdle());
}

// ---- the fake child (tests/fake_tools.h) ------------------------------------------------------------------------------------
struct TmpHolder {
  TempDir tmp;  // declared in a base so it exists before the toolbox is built in it
};
struct FakeTools : TmpHolder, fake_tools::Toolbox {
  FakeTools() : TmpHolder(), fake_tools::Toolbox(tmp.dir) {}
};

bool processGone(std::int64_t pid) {
  if (pid <= 1) return false;
  if (::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH) return true;
  // Killed but not yet reaped (nobody waits for an orphan in a container): a zombie is gone too.
  std::ifstream st("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (std::getline(st, line)) {
    const auto close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z';
  }
  return false;
}

std::int64_t grandchildPid(const fs::path& outDir) {
  std::ifstream f(outDir / "grandchild.pid");
  std::int64_t p = 0;
  f >> p;
  return p;
}

std::vector<std::string> argvOf(const fs::path& outDir) {
  const json j = readJson(outDir / "argv.json");
  std::vector<std::string> v;
  if (j.is_object() && j.contains("argv")) v = j["argv"].get<std::vector<std::string>>();
  return v;
}
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
std::string after(const std::vector<std::string>& v, const std::string& s) {
  const auto it = std::find(v.begin(), v.end(), s);
  return it != v.end() && it + 1 != v.end() ? *(it + 1) : std::string();
}

using fake_tools::release;

}  // namespace

// ---- recorder ---------------------------------------------------------------------------------------------------
TEST_CASE("record: the WAV is bit-exact for mixed block sizes and the sidecar describes it", "[record]") {
  TempDir tmp;
  Host h(kFs, 512);
  auto& rec = h.p.recorder();
  rec.setTakesDir(tmp.dir / "takes");
  REQUIRE(rec.ringCapacity() >= 2 * 48000);  // at least 2 s of audio

  const auto x = signal(48000 * 3 + 123);
  REQUIRE(rec.start(""));
  CHECK(rec.state() == TakeRecorder::State::Armed);
  // The test feeds audio far faster than real time, so give the writer a moment after every ~0.4 s (a real host
  // delivers 0.4 s of audio in 0.4 s; the ring holds 2.7 s).
  {
    const std::vector<int> sizes{1, 17, 64, 480, 4097, 5000, 333, 128};
    std::size_t at = 0, k = 0, sinceSleep = 0;
    std::vector<float> out(8192);
    while (at < x.size()) {
      const auto n = std::min<std::size_t>(static_cast<std::size_t>(sizes[k++ % sizes.size()]), x.size() - at);
      h.process(x.data() + at, out.data(), static_cast<int>(n));
      at += n;
      sinceSleep += n;
      if (sinceSleep > 20000) {
        sinceSleep = 0;
        std::this_thread::sleep_for(30ms);
      }
    }
  }
  CHECK(rec.state() == TakeRecorder::State::Recording);
  CHECK(rec.recordedSamples() == x.size());
  CHECK(h.allocs == 0);  // nothing allocated on the audio thread while recording
  CHECK(h.locks == 0);
  CHECK(h.nonFinite == false);
  finishTake(h);
  CHECK(rec.state() == TakeRecorder::State::Idle);
  CHECK(rec.overruns() == 0);

  const auto takes = rec.listTakes();
  REQUIRE(takes.size() == 1);
  const TakeInfo& t = takes[0];
  CHECK(t.sampleRate == kFs);
  CHECK(t.channels == 1);
  CHECK(t.lengthSamples == static_cast<std::int64_t>(x.size()));
  CHECK(t.overruns == 0);
  CHECK_FALSE(t.hasPlayAlong);  // no song loaded
  CHECK_FALSE(t.createdUtc.empty());
  double rate = 0.0;
  const auto wav = readMono(t.wav, &rate);
  CHECK(rate == kFs);
  REQUIRE(wav.size() == x.size());
  CHECK(std::memcmp(wav.data(), x.data(), x.size() * sizeof(float)) == 0);  // bit-exact

  // The sidecar file itself.
  const json j = readJson(t.json);
  CHECK(j["version"] == 1);
  CHECK(j["sampleRate"] == kFs);
  CHECK(j["channels"] == 1);
  CHECK(j["lengthSamples"] == static_cast<std::int64_t>(x.size()));
  CHECK(j["overruns"] == 0);
  CHECK(j["playAlong"].is_null());
  CHECK(j.contains("createdUtc"));
}

TEST_CASE("record: the input before the rig is recorded, not the output", "[record]") {
  TempDir tmp;
  Host h(kFs, 256);
  h.setParam(kInputGain, 12.0);   // the rig changes the level; the take must not
  h.setParam(kOutputGain, -12.0);
  h.p.recorder().setTakesDir(tmp.dir / "takes");
  const auto x = signal(4800);
  REQUIRE(h.p.recorder().start(""));
  std::vector<float> y;
  h.run(x, y, {256});
  finishTake(h);
  const auto takes = h.p.recorder().listTakes();
  REQUIRE(takes.size() == 1);
  const auto wav = readMono(takes[0].wav);
  REQUIRE(wav.size() == x.size());
  CHECK(std::memcmp(wav.data(), x.data(), x.size() * sizeof(float)) == 0);
}

TEST_CASE("record: a stalled writer makes overruns that are counted, never blocked on, and padded with silence", "[record]") {
  TempDir tmp;
  Host h(kFs, 512);
  auto& rec = h.p.recorder();
  rec.setTakesDir(tmp.dir / "takes");
  const std::size_t cap = rec.ringCapacity();
  const int fitBlocks = static_cast<int>(cap / 512);
  const int total = fitBlocks + 44;
  const auto x = signal(static_cast<std::size_t>(total + 20) * 512);

  rec.setWriterStalledForTest(true);
  REQUIRE(rec.start(""));
  std::size_t at = 0;
  runBlocks(h, x, at, total, 512);
  CHECK(rec.overruns() == 44);
  CHECK(rec.droppedSamples() == 44u * 512u);
  CHECK(rec.recordedSamples() == static_cast<std::uint64_t>(total) * 512u);
  CHECK(h.allocs == 0);
  CHECK(h.locks == 0);

  // The writer comes back; the take continues and its timeline stays intact (the gap is silence).
  rec.setWriterStalledForTest(false);
  std::this_thread::sleep_for(200ms);  // let it drain the ring
  runBlocks(h, x, at, 20, 512);
  CHECK(rec.overruns() == 44);
  finishTake(h);

  const auto takes = rec.listTakes();
  REQUIRE(takes.size() == 1);
  CHECK(takes[0].overruns == 44);
  CHECK(takes[0].droppedSamples == 44u * 512u);
  CHECK(takes[0].lengthSamples == static_cast<std::int64_t>(total + 20) * 512);
  const auto wav = readMono(takes[0].wav);
  REQUIRE(wav.size() == static_cast<std::size_t>(total + 20) * 512);
  const std::size_t good = static_cast<std::size_t>(fitBlocks) * 512, gapEnd = static_cast<std::size_t>(total) * 512;
  CHECK(std::memcmp(wav.data(), x.data(), good * sizeof(float)) == 0);
  for (std::size_t i = good; i < gapEnd; ++i) REQUIRE(wav[i] == 0.0f);
  CHECK(std::memcmp(wav.data() + gapEnd, x.data() + gapEnd, (wav.size() - gapEnd) * sizeof(float)) == 0);
  CHECK(readJson(takes[0].json)["overruns"] == 44);
}

TEST_CASE("record: an overrun at the very end of a take is padded too", "[record]") {
  TempDir tmp;
  Host h(kFs, 512);
  auto& rec = h.p.recorder();
  rec.setTakesDir(tmp.dir / "takes");
  const int total = static_cast<int>(rec.ringCapacity() / 512) + 10;
  const auto x = signal(static_cast<std::size_t>(total) * 512);
  rec.setWriterStalledForTest(true);
  REQUIRE(rec.start(""));
  std::size_t at = 0;
  runBlocks(h, x, at, total, 512);
  rec.stop();
  std::vector<float> in(8, 0.0f), out(8);
  h.process(in.data(), out.data(), 8);  // the End event (and the pending gap) are queued
  rec.setWriterStalledForTest(false);
  REQUIRE(rec.waitIdle());
  const auto takes = rec.listTakes();
  REQUIRE(takes.size() == 1);
  CHECK(takes[0].overruns == 10);
  CHECK(takes[0].lengthSamples == static_cast<std::int64_t>(total) * 512);
}

TEST_CASE("record: start() needs a prepared engine and refuses a second take", "[record]") {
  TempDir tmp;
  {
    SawbladeProcessor p;  // never prepared
    p.recorder().setTakesDir(tmp.dir / "takes");
    CHECK_FALSE(p.recorder().start(""));
    CHECK_FALSE(p.recorder().lastError().empty());
  }
  Host h(kFs, 256);
  h.p.recorder().setTakesDir(tmp.dir / "takes");
  REQUIRE(h.p.recorder().start(""));
  CHECK_FALSE(h.p.recorder().start(""));  // already armed
  h.p.recorder().stop();                  // cancelled before it began: no take, no stuck state
  CHECK(h.p.recorder().state() == TakeRecorder::State::Idle);
  CHECK(h.p.recorder().listTakes().empty());
  REQUIRE(h.p.recorder().start(""));
  const auto x = signal(2560);
  std::size_t at = 0;
  runBlocks(h, x, at, 10, 256);
  CHECK_FALSE(h.p.recorder().start(""));  // recording
  finishTake(h);
  CHECK(h.p.recorder().listTakes().size() == 1);
}

TEST_CASE("record: a take that is shorter than the writer's wake-up period still comes out whole", "[record]") {
  TempDir tmp;
  Host h(kFs, 128);
  auto& rec = h.p.recorder();
  rec.setTakesDir(tmp.dir / "takes");
  const auto x = signal(128 * 4);
  std::size_t at = 0;
  for (int take = 0; take < 3; ++take) {
    rec.setWriterStalledForTest(true);  // start, record and stop between two writer passes
    REQUIRE(rec.start(""));
    runBlocks(h, x, at, 2, 128);
    rec.stop();
    std::vector<float> in(16, 0.0f), out(16);
    h.process(in.data(), out.data(), 16);
    rec.setWriterStalledForTest(false);
    REQUIRE(rec.waitIdle());
    at = 0;
  }
  const auto takes = rec.listTakes();
  REQUIRE(takes.size() == 3);
  for (const auto& t : takes) {
    CHECK(t.lengthSamples == 256);
    const auto wav = readMono(t.wav);
    CHECK(std::memcmp(wav.data(), x.data(), 256 * sizeof(float)) == 0);
  }
}

TEST_CASE("record: the sidecar stores the stem sample played at the take's first sample (Standalone)", "[record][offset]") {
  TempDir tmp;
  Host h(kFs, 480);
  PlayAlong& pa = h.p.playAlong();
  pa.setStandalone(true);
  auto& rec = h.p.recorder();
  rec.setTakesDir(tmp.dir / "takes");
  const auto song = writeSong(tmp.dir, "song", 48000 * 20);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  pa.setLevelDb(0.0);
  // The matcher's sign: the DI starts 1 s into the song, so stem sample 48000 plays at playhead 0.
  pa.setOffsetMs(1000.0);
  const auto x = signal(480 * 64);
  std::size_t at = 0;
  runBlocks(h, x, at, 3, 480);  // stopped: the set and the offset are adopted
  pa.play();
  runBlocks(h, x, at, 10, 480);
  REQUIRE(pa.snapshot().playing);
  const std::int64_t playhead = pa.snapshot().position;  // where the player is when the next block starts
  REQUIRE(playhead > 0);

  REQUIRE(rec.start(song.string()));
  runBlocks(h, x, at, 5, 480);
  CHECK(h.allocs == 0);  // the take began with a StemPlayer running: still nothing allocated or locked on the audio thread
  CHECK(h.locks == 0);
  finishTake(h);
  const auto takes = rec.listTakes();
  REQUIRE(takes.size() == 1);
  const TakeInfo& t = takes[0];
  REQUIRE(t.hasPlayAlong);
  CHECK(t.running);
  CHECK(t.stemSampleRate == kFs);
  CHECK(t.stemSampleIndex == playhead + 48000);  // player offset applied: playhead p plays stem sample p + 48000
  CHECK(t.songFolder == song.string());
  REQUIRE(t.offsetMs().has_value());
  CHECK(*t.offsetMs() == Catch::Approx(1000.0 * static_cast<double>(playhead + 48000) / kFs));
  const json j = readJson(t.json)["playAlong"];
  CHECK(j["running"] == true);
  CHECK(j["stemSampleIndex"] == playhead + 48000);
  CHECK(j["stemSampleRate"] == kFs);
  CHECK(j["songFolder"] == song.string());

  // Paused: the song is loaded but not running; the position is recorded, flagged as not running.
  pa.pause();
  runBlocks(h, x, at, 20, 480);
  REQUIRE_FALSE(pa.snapshot().playing);
  REQUIRE(rec.start(song.string()));
  runBlocks(h, x, at, 3, 480);
  finishTake(h);
  const auto two = rec.listTakes();
  REQUIRE(two.size() == 2);
  int pausedTakes = 0;
  for (const auto& tk : two)
    if (tk.hasPlayAlong && !tk.running) {
      ++pausedTakes;
      CHECK_FALSE(tk.offsetMs().has_value());
    }
  CHECK(pausedTakes == 1);
}

TEST_CASE("record: the sidecar offset follows the host position in plugin mode", "[record][offset]") {
  TempDir tmp;
  Host h(kFs, 256);
  PlayAlong& pa = h.p.playAlong();
  REQUIRE_FALSE(pa.standalone());
  FakeHead head;
  h.p.setPlayHead(&head);
  h.p.recorder().setTakesDir(tmp.dir / "takes");
  const auto song = writeSong(tmp.dir, "song", 48000 * 10);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  pa.setHostSync(true);
  pa.setOffsetMs(100.0);  // stem audio from 4800 plays at host 0
  const auto x = signal(256 * 64);
  std::size_t at = 0;
  runBlocks(h, x, at, 2, 256, &head);
  head.playing = true;
  head.pos = 96000;
  runBlocks(h, x, at, 4, 256, &head);
  const std::int64_t hostPos = head.pos;
  REQUIRE(h.p.recorder().start(song.string()));
  runBlocks(h, x, at, 3, 256, &head);
  CHECK(h.allocs == 0);
  CHECK(h.locks == 0);
  finishTake(h);
  const auto takes = h.p.recorder().listTakes();
  REQUIRE(takes.size() == 1);
  CHECK(takes[0].running);
  CHECK(takes[0].stemSampleIndex == hostPos + 4800);

  // Host stopped: not running.
  head.playing = false;
  REQUIRE(h.p.recorder().start(song.string()));
  runBlocks(h, x, at, 3, 256, &head);
  finishTake(h);
  const auto two = h.p.recorder().listTakes();
  REQUIRE(two.size() == 2);
  int notRunning = 0;
  for (const auto& t : two) notRunning += t.hasPlayAlong && !t.running;
  CHECK(notRunning == 1);
}

TEST_CASE("record: rename and delete keep the WAV and the sidecar together", "[record]") {
  TempDir tmp;
  Host h(kFs, 256);
  auto& rec = h.p.recorder();
  rec.setTakesDir(tmp.dir / "takes");
  const auto x = signal(2560);
  std::size_t at = 0;
  for (int i = 0; i < 2; ++i) {
    REQUIRE(rec.start(""));
    at = 0;
    runBlocks(h, x, at, 5, 256);
    finishTake(h);
  }
  auto takes = rec.listTakes();
  REQUIRE(takes.size() == 2);
  const std::string a = takes[0].name, b = takes[1].name;
  const auto v0 = rec.takesVersion();

  std::string err;
  CHECK_FALSE(rec.renameTake(a, "", &err));
  CHECK_FALSE(rec.renameTake(a, b, &err));  // exists
  CHECK(err.find("already exists") != std::string::npos);
  CHECK(rec.renameTake(a, "verse riff / take 1", &err));
  takes = rec.listTakes();
  REQUIRE(takes.size() == 2);
  bool found = false;
  for (const auto& t : takes)
    if (t.name == "verse riff  take 1") {
      found = true;
      CHECK(fs::exists(t.wav));
      CHECK(fs::exists(t.json));
    }
  CHECK(found);
  CHECK_FALSE(fs::exists(tmp.dir / "takes" / (a + ".wav")));
  CHECK(rec.takesVersion() > v0);

  CHECK(rec.removeTake("verse riff  take 1"));
  CHECK_FALSE(fs::exists(tmp.dir / "takes" / "verse riff  take 1.wav"));
  CHECK_FALSE(fs::exists(tmp.dir / "takes" / "verse riff  take 1.json"));
  CHECK(rec.listTakes().size() == 1);
  CHECK_FALSE(rec.removeTake("../escape"));
}

TEST_CASE("record: re-preparing at another sample rate ends a take in progress and resizes the ring", "[record]") {
  TempDir tmp;
  Host h(kFs, 256);
  auto& rec = h.p.recorder();
  rec.setTakesDir(tmp.dir / "takes");
  const auto x = signal(2560);
  std::size_t at = 0;
  REQUIRE(rec.start(""));
  runBlocks(h, x, at, 5, 256);
  h.prepare(96000.0, 256);
  CHECK(rec.state() == TakeRecorder::State::Idle);
  CHECK(rec.ringCapacity() >= 2 * 96000);
  REQUIRE(rec.listTakes().size() == 1);
  CHECK(rec.listTakes()[0].sampleRate == kFs);
  CHECK(rec.listTakes()[0].lengthSamples == 1280);
  REQUIRE(rec.start(""));
  runBlocks(h, x, at, 3, 256);
  finishTake(h);
  CHECK(rec.listTakes().size() == 2);
}

TEST_CASE("record: recording does not change the saved tone state", "[record][state]") {
  TempDir tmp;
  Host h(kFs, 256);
  h.p.recorder().setTakesDir(tmp.dir / "takes");
  h.p.matchSettings().setFile(tmp.dir / "settings.xml");
  juce::MemoryBlock before, during, after;
  h.p.getStateInformation(before);
  REQUIRE(h.p.recorder().start(""));
  const auto x = signal(2560);
  std::size_t at = 0;
  runBlocks(h, x, at, 5, 256);
  h.p.matchSettings().setMatchExecutable("/somewhere/sawblade-match");
  h.p.matchSettings().setPoolManifest("/somewhere/pool.json");
  h.p.matchSettings().setSelectedTake("take-x");
  h.p.getStateInformation(during);
  finishTake(h);
  h.p.getStateInformation(after);
  CHECK(before == during);
  CHECK(before == after);
}

// ---- pure parsers -------------------------------------------------------------------------------------------------
TEST_CASE("match: progress parsers tolerate partial and odd input", "[match][runner]") {
  using namespace sawblade::plugin;
  JobProgress p;
  CHECK_FALSE(parseProgressJson("", p));
  CHECK_FALSE(parseProgressJson("{\"stage\": \"stage 1", p));  // partial write
  CHECK_FALSE(parseProgressJson("[1,2]", p));
  CHECK(parseProgressJson(R"({"stage":"stage 2","fraction":0.4,"etaSeconds":90,"bestErrorDb":3.5,"message":"m"})", p));
  CHECK(p.stage == "stage 2");
  CHECK(p.fraction == Catch::Approx(0.4));
  CHECK(p.etaSeconds == Catch::Approx(90.0));
  REQUIRE(p.bestErrorDb.has_value());
  CHECK(*p.bestErrorDb == Catch::Approx(3.5));
  CHECK(p.message == "m");
  // null / missing numbers mean unknown
  CHECK(parseProgressJson(R"({"stage":"x","fraction":null,"etaSeconds":null,"bestErrorDb":null})", p));
  CHECK(p.fraction < 0.0);
  CHECK(p.etaSeconds < 0.0);
  CHECK_FALSE(p.bestErrorDb.has_value());
  CHECK(parseProgressJson(R"({"fraction":7})", p));
  CHECK(p.fraction == 1.0);  // clamped

  // log lines: stage names, the last line as the message, an indeterminate fraction
  JobProgress q;
  parseLogLine(JobKind::Match, "[   0.1s] pool {'amps': 3} seed 0", q);
  CHECK(q.stage == "preparing");
  CHECK(q.message == "pool {'amps': 3} seed 0");
  CHECK(q.fraction < 0.0);
  parseLogLine(JobKind::Match, "[  61.2s] stage2 blend [2/3] A + B (screen loss 4.1)", q);
  CHECK(q.stage == "stage 2: fine-tuning");
  parseLogLine(JobKind::Match, "[ 300.0s] stage3: full-length renders ['best']", q);
  CHECK(q.stage == "stage 3: verifying");
  parseLogLine(JobKind::Match, "", q);  // blank lines change nothing
  CHECK(q.stage == "stage 3: verifying");
  JobProgress e;
  parseLogLine(JobKind::Export, "  epoch  12  val ESR 0.01234  (45 s)", e);
  CHECK(e.stage == "training");

  JobProgress x;
  CHECK(parseExportProgress(R"({"epoch": 3, "bestValEsr": 0.01, "elapsedTrainingS": 90, "config": {"epochs": 10, "maxMinutes": null}})", x));
  CHECK(x.fraction == Catch::Approx(0.3));
  CHECK(x.etaSeconds == Catch::Approx(210.0));
  CHECK_FALSE(parseExportProgress("{", x));
}

TEST_CASE("match: the reference is the guitar stem, else other, else the mix, else the first file", "[match]") {
  using namespace sawblade::plugin;
  TempDir tmp;
  const std::vector<float> s(480, 0.0f);
  auto touch = [&](const fs::path& d, const char* n) { sawblade::writeWavFloat32(d / n, kFs, s); };
  const fs::path d = tmp.dir / "song";
  fs::create_directories(d);
  CHECK_FALSE(chooseReferenceFile(d).found);
  CHECK_FALSE(chooseReferenceFile(tmp.dir / "nope").found);
  touch(d, "bass.wav");
  touch(d, "drums.wav");
  auto r = chooseReferenceFile(d);
  REQUIRE(r.found);
  CHECK(r.file.filename() == "bass.wav");  // nothing better: the first audio file
  touch(d, "mix.wav");
  CHECK(chooseReferenceFile(d).file.filename() == "mix.wav");
  touch(d, "other.wav");
  r = chooseReferenceFile(d);
  CHECK(r.file.filename() == "other.wav");
  CHECK(r.label.find("other") != std::string::npos);
  touch(d, "Guitars.wav");
  r = chooseReferenceFile(d);
  CHECK(r.file.filename() == "Guitars.wav");
  CHECK(r.label.find("guitar stem") != std::string::npos);
}

// ---- the runner with a fake child ------------------------------------------------------------------------------------------
TEST_CASE("runner: --progress-json mode, progress, result list and the command line", "[match][runner]") {
  using namespace sawblade::plugin;
  FakeTools t;
  t.cfgMatch({{"progressJson", true}, {"gates", json::array({"g1", "g2"})}});
  JobRunner runner(t.settings, t.jobs);
  std::string err;
  REQUIRE(runner.startMatch(t.request(), &err));
  CHECK(runner.snapshot(JobKind::Match).active());

  REQUIRE(waitUntil([&] { return runner.snapshot(JobKind::Match).progress.stage == "stage 1: screening"; }));
  JobSnapshot s = runner.snapshot(JobKind::Match);
  CHECK(s.state == JobState::Running);
  CHECK(s.progressJson);
  CHECK(s.progress.fraction == Catch::Approx(0.25));
  CHECK(s.progress.etaSeconds == Catch::Approx(120.0));
  CHECK(s.progress.message == "screening 12 pairs");
  CHECK(s.pid > 1);
  CHECK(s.reference == "Test Song (guitar stem: guitar.wav)");
  CHECK(s.di == "take-1");
  CHECK(fs::exists(s.dir / "job.json"));
  CHECK(readJson(s.dir / "job.json")["state"] == "running");
  CHECK(readJson(s.dir / "job.json")["pid"] == s.pid);
  CHECK(runner.snapshot(JobKind::Match).results.empty());

  // A second match while one runs is refused.
  CHECK_FALSE(runner.startMatch(t.request(), &err));
  CHECK(err.find("already running") != std::string::npos);

  const auto argv = argvOf(s.dir);
  CHECK(after(argv, "--di") == t.di.string());
  CHECK(after(argv, "--ref") == t.ref.string());
  CHECK(after(argv, "--ref-channel") == "mid");
  CHECK(after(argv, "--pool") == t.pool.string());
  CHECK(after(argv, "--out") == s.dir.string());
  CHECK(std::stod(after(argv, "--offset-ms")) == Catch::Approx(1234.5));
  CHECK(after(argv, "--progress-json") == (s.dir / "progress.json").string());
  CHECK_FALSE(has(argv, "--stems-dir"));
  CHECK(s.dir.filename().string().size() > 6);
  CHECK(s.dir.filename().string().substr(s.dir.filename().string().size() - 6) == "-match");
  CHECK(s.dir.parent_path() == t.jobs);

  release(s.dir, "g1");
  REQUIRE(waitUntil([&] { return runner.snapshot(JobKind::Match).progress.message == "refining 1/3"; }));  // from progress.json
  s = runner.snapshot(JobKind::Match);
  CHECK(s.progress.stage == "stage 2: fine-tuning");
  CHECK(s.progress.fraction == Catch::Approx(0.6));
  CHECK(s.progress.etaSeconds == Catch::Approx(60.0));
  REQUIRE(s.progress.bestErrorDb.has_value());
  CHECK(*s.progress.bestErrorDb == Catch::Approx(4.2));
  CHECK(s.elapsedSeconds >= 0.0);
  CHECK_FALSE(s.logTail.empty());

  release(s.dir, "g2");
  REQUIRE(runner.waitFinished(JobKind::Match));
  s = runner.snapshot(JobKind::Match);
  REQUIRE(s.state == JobState::Succeeded);
  CHECK(s.exitCode == 0);
  CHECK(s.progress.fraction == 1.0);
  REQUIRE(s.results.size() == 3);
  CHECK(s.results[0].rank == 1);
  CHECK(s.results[0].errorDb == Catch::Approx(3.21));
  CHECK(s.results[0].topology == "blend");
  CHECK(s.results[0].blend == Catch::Approx(0.62));
  CHECK(s.results[0].captures == "SAW HM-2 Chainsaw + JCM800 2203 | BODY TS808 + 5150III | V30 4x12");
  CHECK(s.results[0].presetExists);
  CHECK(s.results[0].preset == s.dir / "best.preset.resolved.json");
  CHECK(s.results[1].rank == 2);
  CHECK(s.results[1].errorDb == Catch::Approx(3.9));
  CHECK(s.results[1].topology == "single");
  CHECK(s.results[1].captures == "HM-2 Chainsaw + JCM800 2203 | V30 4x12");
  CHECK(s.results[1].preset == s.dir / "alt1.preset.resolved.json");
  CHECK(s.results[2].topology == "single2");
  CHECK(s.results[2].captures == "TS808 + HM-2 Chainsaw + 5150III | V30 4x12");
  const json jj = readJson(s.dir / "job.json");
  CHECK(jj["state"] == "succeeded");
  CHECK(jj["kind"] == "match");
  CHECK(jj["exitCode"] == 0);
  CHECK(jj["commandLine"][0] == t.match.string());
  CHECK(jj["progressMode"] == "json");
  CHECK(jj.contains("startedUtc"));
  CHECK(jj.contains("finishedUtc"));
  // The pipe was drained to the log file.
  std::ifstream log(s.dir / "log.txt");
  std::stringstream ss;
  ss << log.rdbuf();
  CHECK(ss.str().find("done in") != std::string::npos);

  // A finished job does not block a new one.
  t.cfgMatch({{"progressJson", true}});
  CHECK(runner.startMatch(t.request(std::nullopt), &err));
  REQUIRE(runner.waitFinished(JobKind::Match));
  CHECK_FALSE(has(argvOf(runner.snapshot(JobKind::Match).dir), "--offset-ms"));  // no offset: none passed
}

TEST_CASE("runner: log-line mode when the executable has no --progress-json", "[match][runner]") {
  using namespace sawblade::plugin;
  FakeTools t;
  t.cfgMatch({{"progressJson", false}, {"gates", json::array({"g1", "g2"})}});
  JobRunner runner(t.settings, t.jobs);
  std::string err;
  REQUIRE(runner.startMatch(t.request(), &err));
  REQUIRE(waitUntil([&] { return runner.snapshot(JobKind::Match).progress.message.find("excerpt") == 0; }));
  JobSnapshot s = runner.snapshot(JobKind::Match);
  CHECK_FALSE(s.progressJson);
  CHECK(s.state == JobState::Running);
  CHECK(s.progress.fraction < 0.0);          // indeterminate
  CHECK(s.progress.etaSeconds < 0.0);        // no ETA from log lines
  CHECK(s.progress.stage == "preparing");
  CHECK(s.progress.message == "excerpt 1.0-7.0 s (loudest)");  // the last line, prefix stripped
  const auto argv = argvOf(s.dir);
  CHECK_FALSE(has(argv, "--progress-json"));
  CHECK_FALSE(fs::exists(s.dir / "progress.json"));
  CHECK(readJson(s.dir / "job.json")["progressMode"] == "log");

  release(s.dir, "g1");
  REQUIRE(waitUntil([&] { return runner.snapshot(JobKind::Match).progress.stage == "stage 2: fine-tuning"; }));
  s = runner.snapshot(JobKind::Match);
  CHECK(s.progress.message == "stage2 blend [1/3] A + B (screen loss 4.2)");
  CHECK(s.progress.fraction < 0.0);
  CHECK(s.state == JobState::Running);

  release(s.dir, "g2");
  REQUIRE(runner.waitFinished(JobKind::Match));  // finishes on result.json / exit
  s = runner.snapshot(JobKind::Match);
  CHECK(s.state == JobState::Succeeded);
  CHECK(s.results.size() == 3);
  CHECK(s.progress.stage == "done");
}

TEST_CASE("runner: cancel stops the child and marks the job cancelled", "[match][runner]") {
  using namespace sawblade::plugin;
  FakeTools t;
  t.cfgMatch({{"progressJson", true}, {"gates", json::array({"g1"})}});
  JobRunner runner(t.settings, t.jobs);
  std::string err;
  REQUIRE(runner.startMatch(t.request(), &err));
  REQUIRE(waitUntil([&] { return runner.snapshot(JobKind::Match).progress.stage == "stage 1: screening" && runner.snapshot(JobKind::Match).pid > 1; }));
  const JobSnapshot before = runner.snapshot(JobKind::Match);
  CHECK_FALSE(processGone(before.pid));
  runner.cancel(JobKind::Match);
  REQUIRE(runner.waitFinished(JobKind::Match, 10000ms));
  const JobSnapshot s = runner.snapshot(JobKind::Match);
  CHECK(s.state == JobState::Cancelled);
  CHECK(processGone(before.pid));
  CHECK(readJson(s.dir / "job.json")["state"] == "cancelled");
  CHECK(s.results.empty());
  // And a new job can start afterwards.
  t.cfgMatch({{"progressJson", true}});
  CHECK(runner.startMatch(t.request(), &err));
  CHECK(runner.waitFinished(JobKind::Match));
  CHECK(runner.snapshot(JobKind::Match).state == JobState::Succeeded);
}

TEST_CASE("runner: a child that ignores SIGTERM is killed after the grace period", "[match][runner]") {
  using namespace sawblade::plugin;
  FakeTools t;
  t.cfgMatch({{"progressJson", true}, {"ignoreTerm", true}, {"gates", json::array({"g1"})}});
  JobRunner runner(t.settings, t.jobs);
  runner.setCancelGrace(300ms);
  std::string err;
  REQUIRE(runner.startMatch(t.request(), &err));
  REQUIRE(waitUntil([&] { return runner.snapshot(JobKind::Match).progress.stage == "stage 1: screening" && runner.snapshot(JobKind::Match).pid > 1; }));
  const auto pid = runner.snapshot(JobKind::Match).pid;
  std::this_thread::sleep_for(300ms);  // the child installs its SIGTERM handler before printing progress, but be safe
  runner.cancel(JobKind::Match);
  REQUIRE(runner.waitFinished(JobKind::Match, 15000ms));
  CHECK(runner.snapshot(JobKind::Match).state == JobState::Cancelled);
  CHECK(processGone(pid));
}

TEST_CASE("runner: a failing child reports its error", "[match][runner]") {
  using namespace sawblade::plugin;
  FakeTools t;
  t.cfgMatch({{"progressJson", false}, {"fail", true}});
  JobRunner runner(t.settings, t.jobs);
  REQUIRE(runner.startMatch(t.request()));
  REQUIRE(runner.waitFinished(JobKind::Match));
  const JobSnapshot s = runner.snapshot(JobKind::Match);
  CHECK(s.state == JobState::Failed);
  CHECK(s.exitCode == 3);
  CHECK(s.message.find("pool needs amps and cabs") != std::string::npos);
  CHECK(s.results.empty());
  CHECK(readJson(s.dir / "job.json")["state"] == "failed");
}

TEST_CASE("runner: a missing executable or pool gives a clear message and does not start", "[match][runner]") {
  using namespace sawblade::plugin;
  FakeTools t;
  JobRunner runner(t.settings, t.jobs);
  CHECK(runner.checkTools(JobKind::Match).ok());

  t.settings.setMatchExecutable(t.root / "nope" / "sawblade-match");
  ToolCheck c = runner.checkTools(JobKind::Match);
  CHECK(c.missing == ToolCheck::Missing::Executable);
  CHECK(c.message.find("sawblade-match was not found") != std::string::npos);
  CHECK(c.message.find("Locate") != std::string::npos);
  std::string err;
  CHECK_FALSE(runner.startMatch(t.request(), &err));
  CHECK(err == c.message);
  CHECK(runner.snapshot(JobKind::Match).state == JobState::None);  // nothing started, no job folder
  CHECK_FALSE(fs::exists(t.jobs));

  // Not executable counts as missing.
  const fs::path plain = t.root / "plain.txt";
  std::ofstream(plain) << "x";
  t.settings.setMatchExecutable(plain);
  CHECK(runner.checkTools(JobKind::Match).missing == ToolCheck::Missing::Executable);

  t.settings.setMatchExecutable(t.match);
  t.settings.setPoolManifest(t.root / "missing_pool.json");
  c = runner.checkTools(JobKind::Match);
  CHECK(c.missing == ToolCheck::Missing::Pool);
  CHECK(c.message.find("pool manifest was not found") != std::string::npos);
  CHECK(c.message.find("Locate") != std::string::npos);
  CHECK_FALSE(runner.startMatch(t.request(), &err));

  t.settings.setExportExecutable(t.root / "nope" / "sawblade-export");
  CHECK(runner.checkTools(JobKind::Export).missing == ToolCheck::Missing::Executable);
  sawblade::plugin::ExportRequest er;
  er.preset = t.presetSrc;
  CHECK_FALSE(runner.startExport(er, &err));
  CHECK(err.find("sawblade-export was not found") != std::string::npos);
}

TEST_CASE("runner: settings persist in application properties", "[match][runner]") {
  using namespace sawblade::plugin;
  TempDir tmp;
  const fs::path f = tmp.dir / "sub" / "settings.xml";
  {
    MatchSettings s(f);
    CHECK(s.matchExecutable() == MatchSettings::defaultMatchExecutable());
    CHECK(s.exportExecutable() == MatchSettings::defaultExportExecutable());
    CHECK(s.poolManifest() == MatchSettings::defaultPoolManifest());
    CHECK(MatchSettings::defaultExportExecutable().string().find("match/.venv/bin/sawblade-export") != std::string::npos);
    CHECK(MatchSettings::defaultPoolManifest().string().find(".cache/sawblade/captures/pool_manifest.json") != std::string::npos);
    CHECK_FALSE(fs::exists(f));  // reading creates nothing
    s.setMatchExecutable("/opt/m/sawblade-match");
    s.setExportExecutable("/opt/m/sawblade-export");
    s.setPoolManifest("/data/pool_manifest.json");
    s.setSelectedTake("take-7");
  }
  MatchSettings again(f);
  CHECK(again.matchExecutable() == "/opt/m/sawblade-match");
  CHECK(again.exportExecutable() == "/opt/m/sawblade-export");
  CHECK(again.poolManifest() == "/data/pool_manifest.json");
  CHECK(again.selectedTake() == "take-7");
}

TEST_CASE("runner: export reads the checkpoint progress, passes mode, size and device auto", "[match][runner][export]") {
  using namespace sawblade::plugin;
  FakeTools t;
  t.cfgExport({{"gates", json::array({"g1", "g2"})}});
  JobRunner runner(t.settings, t.jobs);
  ExportRequest er;
  er.preset = t.presetSrc;
  er.mode = "withcab";
  er.size = "lite";
  er.di = t.di;
  std::string err;
  REQUIRE(runner.startExport(er, &err));
  JobSnapshot s = runner.snapshot(JobKind::Export);
  CHECK(s.dir.filename().string().substr(s.dir.filename().string().size() - 7) == "-export");
  CHECK(s.outDir == s.dir / "export");
  REQUIRE(waitUntil([&] { return runner.snapshot(JobKind::Export).progress.fraction > 0.29; }));
  s = runner.snapshot(JobKind::Export);
  CHECK(s.progress.fraction == Catch::Approx(0.3));
  CHECK(s.progress.stage == "training");
  CHECK(s.progress.etaSeconds == Catch::Approx(210.0));
  CHECK(s.exportMode == "withcab");
  CHECK(s.exportSize == "lite");
  const auto argv = argvOf(s.outDir);
  CHECK(argv.at(0) == t.presetSrc.string());
  CHECK(after(argv, "--mode") == "withcab");
  CHECK(after(argv, "--size") == "lite");
  CHECK(after(argv, "--device") == "auto");  // never a hard-wired cuda / mps
  CHECK(after(argv, "--out") == s.outDir.string());
  CHECK(after(argv, "--di") == t.di.string());

  release(s.outDir, "g1");
  REQUIRE(waitUntil([&] { return runner.snapshot(JobKind::Export).progress.fraction > 0.69; }));
  release(s.outDir, "g2");
  REQUIRE(runner.waitFinished(JobKind::Export));
  s = runner.snapshot(JobKind::Export);
  CHECK(s.state == JobState::Succeeded);
  CHECK(fs::exists(s.outDir / "model.nam"));
  CHECK(s.progress.fraction == 1.0);
  // A match and an export are separate jobs: the match slot is untouched.
  CHECK(runner.snapshot(JobKind::Match).state == JobState::None);
}

TEST_CASE("runner: a new runner re-attaches to a running job and to finished ones", "[match][runner][attach]") {
  using namespace sawblade::plugin;
  FakeTools t;
  t.cfgMatch({{"progressJson", true}, {"gates", json::array({"g1", "g2"})}});
  fs::path dir;
  {
    JobRunner first(t.settings, t.jobs);
    REQUIRE(first.startMatch(t.request()));
    REQUIRE(waitUntil([&] { return first.snapshot(JobKind::Match).progress.stage == "stage 1: screening"; }));
    dir = first.snapshot(JobKind::Match).dir;
  }  // the runner (and its monitor) are gone; the child keeps running

  JobRunner second(t.settings, t.jobs);
  CHECK(second.snapshot(JobKind::Match).state == JobState::None);
  second.attachExisting();
  JobSnapshot s = second.snapshot(JobKind::Match);
  REQUIRE(s.state == JobState::Running);
  CHECK(s.dir == dir);
  CHECK(s.pid > 1);
  CHECK(s.progress.stage == "stage 1: screening");
  CHECK(s.progressJson);
  CHECK(s.reference == "Test Song (guitar stem: guitar.wav)");
  CHECK(second.snapshot(JobKind::Export).state == JobState::None);

  release(dir, "g1");
  REQUIRE(waitUntil([&] { return second.snapshot(JobKind::Match).progress.stage == "stage 2: fine-tuning"; }));
  release(dir, "g2");
  REQUIRE(second.waitFinished(JobKind::Match));
  s = second.snapshot(JobKind::Match);
  CHECK(s.state == JobState::Succeeded);
  REQUIRE(s.results.size() == 3);
  CHECK(readJson(dir / "job.json")["state"] == "succeeded");

  // A third runner finds the finished job and its results.
  JobRunner third(t.settings, t.jobs);
  third.attachExisting();
  s = third.snapshot(JobKind::Match);
  CHECK(s.state == JobState::Succeeded);
  CHECK(s.results.size() == 3);
  CHECK(s.results[0].errorDb == Catch::Approx(3.21));
}

TEST_CASE("runner: a re-attached job can be cancelled, and a job whose process died is reported", "[match][runner][attach]") {
  using namespace sawblade::plugin;
  FakeTools t;
  t.cfgMatch({{"progressJson", true}, {"gates", json::array({"g1"})}});
  std::int64_t pid = 0;
  {
    JobRunner first(t.settings, t.jobs);
    REQUIRE(first.startMatch(t.request()));
    REQUIRE(waitUntil([&] { return first.snapshot(JobKind::Match).progress.stage == "stage 1: screening" && first.snapshot(JobKind::Match).pid > 1; }));
    pid = first.snapshot(JobKind::Match).pid;
  }
  JobRunner second(t.settings, t.jobs);
  second.attachExisting();
  REQUIRE(second.snapshot(JobKind::Match).state == JobState::Running);
  second.cancel(JobKind::Match);
  REQUIRE(second.waitFinished(JobKind::Match, 10000ms));
  CHECK(second.snapshot(JobKind::Match).state == JobState::Cancelled);
  CHECK(processGone(pid));

  // job.json says running but nobody is there (the app was killed, then the process): failed, with a reason.
  const fs::path dir = t.jobs / "29990101-000000-match";
  fs::create_directories(dir);
  json j = {{"version", 1}, {"kind", "match"}, {"state", "running"}, {"pid", 2000000000}, {"startedEpochMs", 1}, {"outDir", dir.string()}};
  std::ofstream(dir / "job.json") << j.dump();
  JobRunner third(t.settings, t.jobs);
  third.attachExisting();
  const JobSnapshot s = third.snapshot(JobKind::Match);
  CHECK(s.dir == dir);
  CHECK(s.state == JobState::Failed);
  CHECK_FALSE(s.message.empty());
}

TEST_CASE("runner: the message thread is never blocked by a running child", "[match][runner]") {
  using namespace sawblade::plugin;
  FakeTools t;
  // A child that prints a lot (more than a pipe holds) while the test thread does nothing: it must not stall,
  // because the reader drains the pipe on its own thread.
  const fs::path spam = t.root / "bin" / "sawblade-match";
  std::ofstream(spam) << "#!/bin/sh\n"
                         "case \"$*\" in *--help*) echo usage; exit 0;; esac\n"
                         "out=\"\"; while [ $# -gt 0 ]; do if [ \"$1\" = \"--out\" ]; then out=\"$2\"; fi; shift; done\n"
                         "mkdir -p \"$out\"\n"
                         "i=0; while [ $i -lt 4000 ]; do echo \"[ 1.0s] stage2 line $i xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"; i=$((i+1)); done\n"
                         "exit 3\n";
  fs::permissions(spam, fs::perms::owner_all);
  JobRunner runner(t.settings, t.jobs);
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(runner.startMatch(t.request()));
  CHECK(std::chrono::steady_clock::now() - t0 < 500ms);  // returns at once
  REQUIRE(runner.waitFinished(JobKind::Match));
  const JobSnapshot s = runner.snapshot(JobKind::Match);
  CHECK(s.state == JobState::Failed);
  CHECK(s.exitCode == 3);
  CHECK(s.logTail.size() <= 40);
  std::ifstream log(s.dir / "log.txt");
  const std::string all((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
  CHECK(all.find("line 3999") != std::string::npos);  // every line got through
}

// ---- plan, audition / apply ----------------------------------------------------------------------------------------------
TEST_CASE("match: the plan uses the loaded song's guitar stem and the take's offset", "[match][plan]") {
  using namespace sawblade::plugin;
  TempDir tmp;
  Host h(kFs, 480);
  PlayAlong& pa = h.p.playAlong();
  pa.setStandalone(true);
  h.p.recorder().setTakesDir(tmp.dir / "takes");
  h.p.matchSettings().setFile(tmp.dir / "settings.xml");

  MatchPlan plan = planMatch(h.p);
  CHECK_FALSE(plan.ok);
  CHECK(plan.message.find("Load a song") != std::string::npos);

  const auto song = writeSong(tmp.dir, "song", 48000 * 20);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  plan = planMatch(h.p);
  CHECK_FALSE(plan.ok);
  CHECK(plan.message.find("USE FOR MATCH") != std::string::npos);
  CHECK(plan.reference.found);
  CHECK(plan.reference.file.filename() == "other.wav");  // a 4-stem song: other is the guitar

  // A take recorded with the song playing.
  pa.setOffsetMs(2000.0);
  const auto x = signal(480 * 64);
  std::size_t at = 0;
  runBlocks(h, x, at, 3, 480);
  pa.play();
  runBlocks(h, x, at, 4, 480);
  REQUIRE(h.p.recorder().start(song.string()));
  runBlocks(h, x, at, 4, 480);
  finishTake(h);
  const auto takes = h.p.recorder().listTakes();
  REQUIRE(takes.size() == 1);
  h.p.matchSettings().setSelectedTake(takes[0].name);
  plan = planMatch(h.p);
  REQUIRE(plan.ok);
  CHECK(plan.request.di == takes[0].wav);
  CHECK(plan.request.ref == song / "other.wav");
  REQUIRE(plan.request.offsetMs.has_value());
  CHECK(*plan.request.offsetMs == Catch::Approx(*takes[0].offsetMs()));
  CHECK(*plan.request.offsetMs > 2000.0);
  CHECK(plan.offsetNote.find("into the song") != std::string::npos);

  // A guitar stem wins over other.
  sawblade::writeWavFloat32Stereo(song / "guitar.wav", kFs, std::vector<float>(480, 0.0f), std::vector<float>(480, 0.0f));
  CHECK(planMatch(h.p).request.ref == song / "guitar.wav");

  // The take was recorded against another song: its position is not used.
  const auto other = writeSong(tmp.dir, "other song", 48000 * 5);
  pa.loadFolder(other.string(), true);
  REQUIRE(pa.waitForLoader());
  plan = planMatch(h.p);
  REQUIRE(plan.ok);
  CHECK_FALSE(plan.request.offsetMs.has_value());
  CHECK(plan.offsetNote.find("another song") != std::string::npos);

  // A take with no song running has no position either.
  REQUIRE(h.p.recorder().start(other.string()));
  runBlocks(h, x, at, 2, 480);  // paused by the user load
  finishTake(h);
  for (const auto& t : h.p.recorder().listTakes())
    if (t.name != takes[0].name) h.p.matchSettings().setSelectedTake(t.name);
  plan = planMatch(h.p);
  REQUIRE(plan.ok);
  CHECK_FALSE(plan.request.offsetMs.has_value());
  CHECK(plan.offsetNote.find("unknown") != std::string::npos);
}

#ifdef SAWBLADE_WITH_SEPARATOR
TEST_CASE("match: a song loaded from a FILE is the reference and the take's songFolder (5.1b)", "[match][plan][separation]") {
  using namespace sawblade::plugin;
  TempDir tmp;
  struct Env {
    std::string name;
    Env(const char* n, const std::string& v) : name(n) { ::setenv(n, v.c_str(), 1); }
    ~Env() { ::unsetenv(name.c_str()); }
  } models("SAWBLADE_MODELS_DIR", (tmp.dir / "models").string()), stems("SAWBLADE_STEMS_DIR", (tmp.dir / "stems").string());
  fs::create_directories(tmp.dir / "models");
  const std::vector<float> t(6, 0.5f), f(6, 0.0f);
  const std::string bytes = sawblade::test::onnx_synth::buildCoreModel(t, f);
  const fs::path mp = tmp.dir / "models" / "htdemucs_6s-core-opset17.onnx";
  sawblade::test::onnx_synth::write(mp, bytes);
  std::ofstream(mp.string() + ".sha256") << sawblade::sha256Hex(bytes.data(), bytes.size()) << "\n";
  const fs::path file = tmp.dir / "my song.wav";
  sawblade::writeWavFloat32Stereo(file, 44100.0, signal(44100 * 4, 3), signal(44100 * 4, 4));

  Host h(kFs, 480);
  PlayAlong& pa = h.p.playAlong();
  pa.setStandalone(true);
  h.p.recorder().setTakesDir(tmp.dir / "takes");
  h.p.matchSettings().setFile(tmp.dir / "settings.xml");
  pa.loadSong(file.string(), true);
  REQUIRE(pa.waitForLoader());
  REQUIRE(pa.loadStatus().state == PlayAlong::LoadStatus::State::Ready);
  CHECK(pa.settings().folder.empty());  // exclusive with songFile: the old code saw "no song"

  const std::string dir = pa.activeStemsDir();
  REQUIRE_FALSE(dir.empty());
  CHECK(fs::path(dir).lexically_relative(tmp.dir / "stems").native().rfind("..", 0) != 0);  // inside the stem cache

  MatchPlan plan = planMatch(h.p);
  CHECK_FALSE(plan.ok);
  CHECK(plan.message.find("USE FOR MATCH") != std::string::npos);  // not "Load a song"
  CHECK(plan.reference.found);

  const auto x = signal(480 * 64);
  std::size_t at = 0;
  runBlocks(h, x, at, 3, 480);
  pa.play();
  runBlocks(h, x, at, 4, 480);
  REQUIRE(h.p.recorder().start(pa.activeStemsDir()));
  runBlocks(h, x, at, 4, 480);
  finishTake(h);
  const auto takes = h.p.recorder().listTakes();
  REQUIRE(takes.size() == 1);
  CHECK(fs::path(takes[0].songFolder) == fs::path(dir));
  h.p.matchSettings().setSelectedTake(takes[0].name);
  plan = planMatch(h.p);
  REQUIRE(plan.ok);
  CHECK(fs::path(plan.request.ref).parent_path() == fs::path(dir));
  CHECK(plan.request.referenceLabel.rfind("my song (", 0) == 0);
  CHECK(activeSongName(h.p) == "my song");
  REQUIRE(plan.request.offsetMs.has_value());  // same song: the position is used
  CHECK(plan.offsetNote.find("into the song") != std::string::npos);
}
#endif

TEST_CASE("match: audition, A/B and apply go through the loader and keep the previous preset", "[match][audition]") {
  TempDir tmp;
  Host h(kFs, 256);
  auto& au = h.p.audition();
  const fs::path candA = writeIdentityPreset(tmp.dir, "candA", 0);
  const fs::path candB = writeIdentityPreset(tmp.dir, "candB", 64);
  REQUIRE(h.p.status().presetName == "Init");
  const auto builds0 = h.p.engineBuilds();
  CHECK_FALSE(au.state().active);
  CHECK_FALSE(au.toggleAB());
  CHECK_FALSE(au.apply());

  std::string err;
  REQUIRE(au.audition(candA, &err));
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().presetName == "candA");
  CHECK(h.p.status().latencySamples == 0);
  CHECK(h.p.engineBuilds() > builds0);  // built by the loader, not on this thread
  CHECK(au.state().active);
  CHECK(au.state().onCandidate);
  CHECK(au.state().originalName == "Init");
  CHECK(au.state().candidateName == "candA");

  // Another candidate: A stays the preset from before the first audition.
  REQUIRE(au.audition(candB, &err));
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().presetName == "candB");
  CHECK(h.p.status().latencySamples == 64);
  CHECK(au.state().originalName == "Init");

  // A/B back and forth.
  REQUIRE(au.toggleAB());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().presetName == "Init");
  CHECK(h.p.status().latencySamples == 0);
  CHECK_FALSE(au.state().onCandidate);
  CHECK(au.currentCandidateFile() == std::optional<fs::path>{});  // Init is loaded
  REQUIRE(au.toggleAB());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().presetName == "candB");
  CHECK(au.state().onCandidate);
  CHECK(au.currentCandidateFile() == std::optional<fs::path>{candB});

  // Apply: the candidate stays, the audition ends, a normal preset load result.
  REQUIRE(au.apply());
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(au.state().active);
  CHECK(h.p.status().presetName == "candB");
  CHECK(h.p.currentPreset().name == "candB");
  CHECK(au.currentCandidateFile() == std::optional<fs::path>{candB});
  CHECK_FALSE(au.toggleAB());  // nothing to compare against any more
  juce::MemoryBlock state;
  h.p.getStateInformation(state);  // the applied preset is what a host would save
  CHECK(juce::String::fromUTF8(static_cast<const char*>(state.getData()), static_cast<int>(state.getSize())).contains("candB"));

  // Apply while A is playing loads the candidate; revert goes back.
  REQUIRE(au.audition(candA, &err));
  REQUIRE(au.toggleAB());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().presetName == "candB");  // A is now the applied candB
  REQUIRE(au.apply());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().presetName == "candA");
  REQUIRE(au.audition(candB, &err));
  REQUIRE(au.revert());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().presetName == "candA");
  CHECK_FALSE(au.state().active);

  // A candidate that cannot be parsed changes nothing.
  const fs::path bad = tmp.dir / "bad.json";
  std::ofstream(bad) << "{ not a preset";
  CHECK_FALSE(au.audition(bad, &err));
  CHECK_FALSE(err.empty());
  CHECK_FALSE(au.state().active);
  CHECK(h.p.status().presetName == "candA");
}

TEST_CASE("match: the export source is the loaded candidate, else the current preset as written", "[match][export]") {
  using namespace sawblade::plugin;
  TempDir tmp;
  Host h(kFs, 256);
  h.p.jobs().setJobsDir(tmp.dir / "jobs");
  const fs::path cand = writeIdentityPreset(tmp.dir, "cand", 0);
  ExportSource s = prepareExportSource(h.p);
  REQUIRE(s.ok);
  CHECK(s.file.parent_path() == tmp.dir / "jobs" / "inputs");
  CHECK(s.description.find("Current preset: Init") == 0);
  const json j = readJson(s.file);
  CHECK(j["name"] == "Init");
  CHECK(j["schema"] == "sawblade.preset");

  REQUIRE(h.p.audition().audition(cand));
  REQUIRE(h.p.waitForLoader());
  s = prepareExportSource(h.p);
  REQUIRE(s.ok);
  CHECK(s.file == cand);
  CHECK(s.description.find("cand.json") != std::string::npos);
}

TEST_CASE("plugin mode: MATCH and EXPORT are Standalone-only", "[match][gating]") {
  Host h(kFs, 256);
  CHECK_FALSE(h.p.matchEnabled());  // a processor outside the Standalone wrapper counts as a plugin
  h.p.playAlong().setStandalone(true);
  CHECK(h.p.matchEnabled());
  h.p.playAlong().setStandalone(false);
  CHECK_FALSE(h.p.matchEnabled());
  // Recording is not gated: it works in plugin mode.
  TempDir tmp;
  h.p.recorder().setTakesDir(tmp.dir / "takes");
  CHECK(h.p.recorder().start(""));
  h.p.recorder().stop();
}

TEST_CASE("match: a finished match job's candidates audition and apply through the processor", "[match][integration]") {
  using namespace sawblade::plugin;
  FakeTools t;
  t.cfgMatch({{"progressJson", true}});
  Host h(kFs, 256);
  h.p.jobs().setJobsDir(t.jobs);
  h.p.matchSettings().setFile(t.root / "settings.xml");  // the fake tool paths saved by the toolbox
  REQUIRE(h.p.jobs().startMatch(t.request()));
  REQUIRE(h.p.jobs().waitFinished(JobKind::Match));
  const JobSnapshot s = h.p.jobs().snapshot(JobKind::Match);
  REQUIRE(s.state == JobState::Succeeded);
  REQUIRE(s.results.size() == 3);

  REQUIRE(h.p.audition().audition(s.results[1].preset));  // alternative #2
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().presetName == "match alt 1");
  REQUIRE(h.p.audition().toggleAB());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().presetName == "Init");
  REQUIRE(h.p.audition().audition(s.results[0].preset));  // the best
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().presetName == "match best");
  REQUIRE(h.p.audition().apply());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().presetName == "match best");
  CHECK(h.p.status().error.empty());
}

TEST_CASE("runner: cancel kills the tool's whole process group, helpers included", "[match][runner][cancel]") {
  using namespace sawblade::plugin;
  for (bool ignoreTerm : {false, true}) {
    INFO("ignoreTerm " << ignoreTerm);
    FakeTools t;
    t.cfgMatch({{"progressJson", true}, {"grandchild", true}, {"ignoreTerm", ignoreTerm}, {"gates", json::array({"g1"})}});
    JobRunner runner(t.settings, t.jobs);
    runner.setCancelGrace(300ms);
    REQUIRE(runner.startMatch(t.request()));
    REQUIRE(waitUntil([&] { return runner.snapshot(JobKind::Match).progress.stage == "stage 1: screening"; }));
    const JobSnapshot s = runner.snapshot(JobKind::Match);
    const auto gc = grandchildPid(s.dir);
    REQUIRE(gc > 1);
    CHECK_FALSE(processGone(gc));
    CHECK(readJson(s.dir / "job.json")["pgid"] == s.pid);  // its own process group
    runner.cancel(JobKind::Match);
    REQUIRE(runner.waitFinished(JobKind::Match, 15000ms));
    CHECK(runner.snapshot(JobKind::Match).state == JobState::Cancelled);
    CHECK(processGone(s.pid));
    CHECK(waitUntil([&] { return processGone(gc); }, 5000ms));  // the grandchild died with the group
  }
}

TEST_CASE("runner: a re-attached runner cancels the whole group too", "[match][runner][cancel][attach]") {
  using namespace sawblade::plugin;
  FakeTools t;
  t.cfgMatch({{"progressJson", true}, {"grandchild", true}, {"gates", json::array({"g1"})}});
  fs::path dir;
  {
    JobRunner first(t.settings, t.jobs);
    REQUIRE(first.startMatch(t.request()));
    REQUIRE(waitUntil([&] { return first.snapshot(JobKind::Match).progress.stage == "stage 1: screening"; }));
    dir = first.snapshot(JobKind::Match).dir;
  }
  const auto gc = grandchildPid(dir);
  REQUIRE(gc > 1);
  JobRunner second(t.settings, t.jobs);
  second.attachExisting();
  REQUIRE(second.snapshot(JobKind::Match).state == JobState::Running);
  second.cancel(JobKind::Match);
  REQUIRE(second.waitFinished(JobKind::Match, 15000ms));
  CHECK(second.snapshot(JobKind::Match).state == JobState::Cancelled);
  CHECK(waitUntil([&] { return processGone(gc); }, 5000ms));
}

TEST_CASE("match: an applied candidate is forgotten after any later change, so EXPORT takes the current state", "[match][audition][export]") {
  using namespace sawblade::plugin;
  TempDir tmp;
  Host h(kFs, 256);
  h.p.jobs().setJobsDir(tmp.dir / "jobs");
  const fs::path candA = writeIdentityPreset(tmp.dir, "candA", 0);
  const fs::path candB = writeIdentityPreset(tmp.dir, "candB", 0);
  auto& au = h.p.audition();
  REQUIRE(au.audition(candA));
  REQUIRE(h.p.waitForLoader());
  REQUIRE(au.apply());
  REQUIRE(h.p.waitForLoader());
  CHECK(au.currentCandidateFile() == std::optional<fs::path>{candA});
  CHECK(prepareExportSource(h.p).file == candA);

  // A parameter change: the state is no longer the candidate.
  h.setParam(kBlend, 0.9);
  CHECK(au.currentCandidateFile() == std::optional<fs::path>{});
  ExportSource s = prepareExportSource(h.p);
  REQUIRE(s.ok);
  CHECK(s.file != candA);
  CHECK(s.description.find("Current preset") == 0);
  CHECK(readJson(s.file)["blend"] == Catch::Approx(0.9));

  // Back to the candidate's value does not matter; loading another preset does.
  REQUIRE(au.audition(candA));
  REQUIRE(h.p.waitForLoader());
  REQUIRE(au.apply());
  REQUIRE(h.p.waitForLoader());
  CHECK(au.currentCandidateFile().has_value());
  REQUIRE(h.p.loadPresetFile(candB));
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(au.currentCandidateFile().has_value());
  CHECK(readJson(prepareExportSource(h.p).file)["name"] == "candB");
}

TEST_CASE("match: the written export source loads back with its capture paths resolved; a capture without a file blocks the export", "[match][export]") {
  using namespace sawblade::plugin;
  TempDir tmp;
  Host h(kFs, 256);
  h.p.jobs().setJobsDir(tmp.dir / "jobs");
  const fs::path src = writeIdentityPreset(tmp.dir, "withcaptures", 0);
  h.load(src);
  const ExportSource s = prepareExportSource(h.p);
  REQUIRE(s.ok);
  REQUIRE(fs::exists(s.file));
  // The way sawblade-export reads it: through the preset loader, from another directory.
  const Preset back = sawblade::loadPresetFile(s.file);
  int nam = 0;
  for (const PathPreset* path : {&back.a, &back.b})
    for (const auto& b : path->blocks)
      if (const auto* n = dynamic_cast<const NamBlockParams*>(b.params.get())) {
        ++nam;
        CHECK(n->model.resolvedPath.is_absolute());
        CHECK(fs::exists(n->model.resolvedPath));
        CHECK(n->model.resolvedPath.filename() == "linear_identity.nam");
      }
  CHECK(nam == 2);
  CHECK(back.cab.ir.resolvedPath.is_absolute());
  CHECK(fs::exists(back.cab.ir.resolvedPath));
  CHECK(exportBlockedReason(back).empty());

  // A capture with no resolved path cannot be exported.
  Preset bad = back;
  for (auto& b : bad.a.blocks)
    if (const auto* n = dynamic_cast<const NamBlockParams*>(b.params.get())) {
      auto copy = std::make_shared<NamBlockParams>(*n);
      copy->model.resolvedPath.clear();
      b.params = copy;
    }
  CHECK(exportBlockedReason(bad).find("no model file") != std::string::npos);
  Preset badIr = back;
  badIr.cab.ir.resolvedPath.clear();
  CHECK(exportBlockedReason(badIr).find("cab IR") != std::string::npos);
  badIr.cab.enabled = false;  // a disabled cab is not exported
  CHECK(exportBlockedReason(badIr).empty());
  CHECK(exportBlockedReason(makeInitPreset()).empty());
}

TEST_CASE("runner: a job.json whose pid belongs to an unrelated process is never treated as running or signalled", "[match][runner][attach][pid]") {
  using namespace sawblade::plugin;
  FakeTools t;
  // An unrelated live process (not a child of this test) leading its own process group, as a real tool would.
  pid_t victim = 0;
  {
    FILE* p = ::popen("python3 -c \"import subprocess; print(subprocess.Popen(['sleep','60'], start_new_session=True).pid)\"", "r");
    REQUIRE(p != nullptr);
    long v = 0;
    REQUIRE(std::fscanf(p, "%ld", &v) == 1);
    ::pclose(p);
    victim = static_cast<pid_t>(v);
  }
  REQUIRE(victim > 1);
  auto alive = [&] { return !processGone(victim); };

  struct Case {
    const char* dir;
    std::int64_t pgid;
    std::int64_t spawnedMs;
    const char* what;
  };
  const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  const Case cases[] = {
      {"29990101-000000-match", victim, now - 3600 * 1000, "same pgid, started an hour before the recorded spawn"},
      {"29990102-000000-match", ::getpgrp(), now, "right start time, different process group"},
      {"29990103-000000-match", victim, 0, "no recorded spawn time"},
  };
  for (const Case& c : cases) {
    INFO(c.what);
    const fs::path dir = t.jobs / c.dir;
    fs::create_directories(dir);
    json j = {{"version", 1}, {"kind", "match"}, {"state", "running"}, {"pid", victim}, {"pgid", c.pgid},
              {"startedEpochMs", c.spawnedMs}, {"spawnedEpochMs", c.spawnedMs}, {"outDir", dir.string()}};
    std::ofstream(dir / "job.json") << j.dump();
    JobRunner runner(t.settings, t.jobs);
    runner.attachExisting();
    JobSnapshot s = runner.snapshot(JobKind::Match);
    CHECK(s.dir == dir);
    CHECK(s.state == JobState::Failed);
    CHECK(s.message.find("interrupted") != std::string::npos);
    runner.cancel(JobKind::Match);
    std::this_thread::sleep_for(400ms);
    CHECK(alive());  // never signalled
    fs::remove_all(dir);
  }

  // Control: the same process with a matching identity IS recognised (and then cancelled like a real tool).
  {
    const fs::path dir = t.jobs / "29990104-000000-match";
    fs::create_directories(dir);
    json j = {{"version", 1}, {"kind", "match"}, {"state", "running"}, {"pid", victim}, {"pgid", victim},
              {"startedEpochMs", now}, {"spawnedEpochMs", now}, {"outDir", dir.string()}};
    std::ofstream(dir / "job.json") << j.dump();
    JobRunner runner(t.settings, t.jobs);
    runner.setCancelGrace(300ms);
    runner.attachExisting();
    CHECK(runner.snapshot(JobKind::Match).state == JobState::Running);
    runner.cancel(JobKind::Match);
    REQUIRE(runner.waitFinished(JobKind::Match, 10000ms));
    CHECK(runner.snapshot(JobKind::Match).state == JobState::Cancelled);
    CHECK(waitUntil([&] { return processGone(victim); }, 5000ms));
  }
  ::kill(victim, SIGKILL);
}
