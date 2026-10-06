// v0.2.1 Task B (plugin part): IMPORT DI... without any UI. The decode / channel / check / write path (DiImport.h,
// TakeRecorder::importTake), the sidecar, the match plan and the matcher's command line for an imported take. The dialog, the take
// lists and the drops are in test_di_import_ui.cpp (needs a display). Audio is synthesised into temp dirs; nothing is committed.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include "DiImport.h"
#include "JobRunner.h"
#include "MatchGlue.h"
#include "TakeRecorder.h"
#include "fake_tools.h"
#include "processor_harness.h"
#include "sawblade/wav_io.h"

namespace {

namespace di = sawblade::plugin::di_import;
using sawblade::plugin::ImportedInfo;
using sawblade::plugin::TakeRecorder;

// Anything that falls back to the default data folder stays inside a temp dir.
[[maybe_unused]] const bool kImportDataDirSet = [] {
  const fs::path d = fs::temp_directory_path() / ("sawblade_di_import_tests_data_" + std::to_string(::getpid()));
  ::setenv("SAWBLADE_DATA_DIR", d.c_str(), 1);
  return true;
}();

// ---- fixtures ---------------------------------------------------------------------------------------------------------------
// Interleaved sample VALUES that the format stores exactly: every file format below holds these floats without rounding, so
// the take can be compared sample for sample. Codes are 24-bit integers c (|c| <= 6e6, i.e. peak 0.71 FS); a 16-bit file stores c >> 8,
// a 32-bit integer file c << 8, a float file the float c / 2^23.
struct Fixture {
  std::vector<float> values;           // interleaved, what a decoder must give back
  std::vector<std::int32_t> codes;     // interleaved, scaled to `bits`
};

Fixture makeFixture(int channels, int bits, bool isFloat, std::size_t frames) {
  Fixture f;
  f.values.resize(frames * static_cast<std::size_t>(channels));
  f.codes.resize(f.values.size());
  for (std::size_t i = 0; i < frames; ++i)
    for (int c = 0; c < channels; ++c) {
      const double s = 0.6 * std::sin(0.0137 * static_cast<double>(i) * (1.0 + 0.41 * c)) + 0.11 * std::sin(0.31 * static_cast<double>(i) + c);
      std::int32_t c24 = static_cast<std::int32_t>(std::lround(s * 8388608.0));
      std::int32_t code = c24, shift = 0;
      if (!isFloat && bits == 16) {
        code = c24 >> 8;
        shift = 15;
      } else if (!isFloat && bits == 24) {
        shift = 23;
      } else if (!isFloat && bits == 32) {
        code = static_cast<std::int32_t>(static_cast<std::uint32_t>(c24) << 8);
        shift = 31;
      } else {
        shift = 23;  // float: c24 / 2^23
      }
      const std::size_t k = i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c);
      f.codes[k] = code;
      f.values[k] = static_cast<float>(code) / static_cast<float>(std::int64_t{1} << shift);
    }
  return f;
}

void put16(std::ofstream& o, std::uint32_t v) {
  const char b[2] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff)};
  o.write(b, 2);
}
void put32(std::ofstream& o, std::uint32_t v) {
  put16(o, v & 0xffff);
  put16(o, v >> 16);
}

// A PCM (tag 1) or float (tag 3) WAV written by hand, so the stored values are known exactly.
void writePcmWav(const fs::path& p, double rate, int channels, int bits, bool isFloat, const Fixture& f) {
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  const std::uint32_t bytes = static_cast<std::uint32_t>(f.codes.size()) * static_cast<std::uint32_t>(bits / 8);
  o.write("RIFF", 4);
  put32(o, 36 + bytes);
  o.write("WAVEfmt ", 8);
  put32(o, 16);
  put16(o, isFloat ? 3 : 1);
  put16(o, static_cast<std::uint32_t>(channels));
  put32(o, static_cast<std::uint32_t>(rate));
  put32(o, static_cast<std::uint32_t>(rate) * static_cast<std::uint32_t>(channels * bits / 8));
  put16(o, static_cast<std::uint32_t>(channels * bits / 8));
  put16(o, static_cast<std::uint32_t>(bits));
  o.write("data", 4);
  put32(o, bytes);
  for (std::size_t k = 0; k < f.codes.size(); ++k) {
    if (isFloat) {
      float v = f.values[k];
      o.write(reinterpret_cast<const char*>(&v), 4);
    } else {
      const std::uint32_t u = static_cast<std::uint32_t>(f.codes[k]);
      for (int b = 0; b < bits / 8; ++b) o.put(static_cast<char>((u >> (8 * b)) & 0xff));
    }
  }
}

// AIFF / FLAC through JUCE's writers (the codes cannot be chosen there: callers compare within one LSB).
void writeJuceFile(const fs::path& p, bool flac, double rate, int channels, int bits, const Fixture& f) {
  const std::size_t frames = f.values.size() / static_cast<std::size_t>(channels);
  std::vector<std::vector<float>> ch(static_cast<std::size_t>(channels), std::vector<float>(frames));
  for (std::size_t i = 0; i < frames; ++i)
    for (int c = 0; c < channels; ++c) ch[static_cast<std::size_t>(c)][i] = f.values[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
  std::vector<const float*> ptrs;
  for (auto& v : ch) ptrs.push_back(v.data());
  std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(juce::File(juce::String(p.string())));
  REQUIRE(static_cast<juce::FileOutputStream*>(stream.get())->openedOk());
  const auto opts = juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(channels).withBitsPerSample(bits);
  std::unique_ptr<juce::AudioFormatWriter> w;
  juce::AiffAudioFormat aiff;
  juce::FlacAudioFormat flacFmt;
  w = flac ? flacFmt.createWriterFor(stream, opts) : aiff.createWriterFor(stream, opts);
  REQUIRE(w != nullptr);
  REQUIRE(w->writeFromFloatArrays(ptrs.data(), channels, static_cast<int>(frames)));
}

struct Takes {
  TempDir tmp;
  TakeRecorder rec;
  Takes() { rec.setTakesDir(tmp.dir / "takes"); }
  fs::path takes() const { return tmp.dir / "takes"; }
};

std::vector<float> takeSamples(const fs::path& wav, double* rate) {
  const sawblade::AudioFile f = sawblade::readWav(wav);
  REQUIRE(f.channels == 1);
  *rate = f.sampleRate;
  return f.interleaved;
}

json readJsonFile(const fs::path& p) {
  std::ifstream f(p);
  return json::parse(f, nullptr, false);
}

std::size_t countTakeFiles(const fs::path& dir) {
  std::size_t n = 0;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return 0;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.path().extension() == ".wav" || e.path().extension() == ".json") ++n;
  return n;
}

std::vector<std::string> argvOf(const fs::path& outDir) {
  const json j = readJsonFile(outDir / "argv.json");
  std::vector<std::string> v;
  if (j.is_object() && j.contains("argv")) v = j["argv"].get<std::vector<std::string>>();
  return v;
}
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
std::string after(const std::vector<std::string>& v, const std::string& s) {
  const auto it = std::find(v.begin(), v.end(), s);
  return it != v.end() && it + 1 != v.end() ? *(it + 1) : std::string();
}

}  // namespace

// ---- pure pieces -----------------------------------------------------------------------------------------------------------
TEST_CASE("import: the start time is m:ss.mmm and nothing else", "[import]") {
  CHECK(di::parseSongTime("0:00.000") == 0.0);
  CHECK(di::parseSongTime("1:23.500") == 83500.0);
  CHECK(di::parseSongTime("1:23") == 83000.0);
  CHECK(di::parseSongTime("12:05.5") == 725500.0);
  CHECK(di::parseSongTime("0:00.25") == 250.0);
  for (const char* bad : {"", " ", "1:75", "1.5", "abc", "-1:00", "1:5", "1:05.", "1:05.1234", "1:05 ", ":30", "1:2:3", "1:05.x"}) {
    INFO("'" << bad << "'");
    CHECK_FALSE(di::parseSongTime(bad).has_value());
  }
  CHECK(di::formatSongTime(83500.0) == "1:23.500");
  CHECK(di::formatSongTime(0.0) == "0:00.000");
  CHECK(di::formatSongTime(*di::parseSongTime("7:08.009")) == "7:08.009");
}

TEST_CASE("import: WAV, AIFF and FLAC are accepted by name, in any case; nothing else", "[import]") {
  for (const char* ok : {"a.wav", "A.WAV", "x/y/di.Aif", "di.aiff", "di.FLAC"}) CHECK(di::isImportableName(ok));
  for (const char* no : {"a.mp3", "a.m4a", "a.ogg", "a", "a.wav.txt", "wav", "a.aac"}) CHECK_FALSE(di::isImportableName(no));
}

TEST_CASE("import: silent means a peak below -60 dBFS; clipped means 4 samples in a row at 0.999 or more", "[import][reject]") {
  const double fs48 = 48000.0;
  std::vector<float> x(48000, 0.2f);
  CHECK(di::rejectReason(x, fs48).empty());
  CHECK_FALSE(di::rejectReason({}, fs48).empty());

  // silent: -60 dBFS = 0.001
  std::vector<float> quiet(48000, 0.0f);
  quiet[100] = 0.00099f;  // -60.09 dBFS
  CHECK(di::rejectReason(quiet, fs48).find("silent") != std::string::npos);
  quiet[100] = -0.00101f;  // -59.9 dBFS, either sign
  CHECK(di::rejectReason(quiet, fs48).empty());
  CHECK(di::rejectReason(std::vector<float>(100, 0.0f), fs48).find("silent") != std::string::npos);

  // clipped: a run of >= 4 at |x| >= 0.999 (the first one is reported)
  std::vector<float> c = x;
  c[1000] = c[1001] = 1.0f;
  c[1002] = -1.0f;                 // 3 in a row (mixed signs count): a peak, not clipping
  c[5000] = 0.9989f;               // just under the level
  c[5001] = 0.9989f; c[5002] = 0.9989f; c[5003] = 0.9989f; c[5004] = 0.9989f;
  CHECK(di::rejectReason(c, fs48).empty());
  c[1003] = 0.999f;                // 4 in a row
  const std::string why = di::rejectReason(c, fs48);
  CHECK(why.find("clipped") != std::string::npos);
  CHECK(why.find("0:00.021") != std::string::npos);  // sample 1000 at 48 kHz = 20.8 ms
  CHECK(why.find('\n') == std::string::npos);        // one line
  std::vector<float> run(2000, 0.1f);
  run[1996] = run[1997] = run[1998] = run[1999] = 1.0f;  // a run at the very end counts
  CHECK(di::rejectReason(run, fs48).find("clipped") != std::string::npos);
  // 16-bit full scale: +32767 / 32768 and -32768 / 32768
  std::vector<float> fsc(2000, 0.1f);
  fsc[10] = fsc[11] = 32767.0f / 32768.0f;
  fsc[12] = fsc[13] = -1.0f;
  CHECK(di::rejectReason(fsc, fs48).find("clipped") != std::string::npos);
}

// ---- the import --------------------------------------------------------------------------------------------------------------
TEST_CASE("import: mono, stereo (left, right, sum), 44.1 / 48 kHz, 16 / 24 / 32-bit and float files become takes, sample for sample", "[import][take]") {
  struct Fmt { int bits; bool isFloat; const char* name; };
  const Fmt fmts[] = {{16, false, "16-bit"}, {24, false, "24-bit"}, {32, false, "32-bit int"}, {32, true, "32-bit float"}};
  Takes t;
  for (const double rate : {44100.0, 48000.0})
    for (const Fmt& fm : fmts)
      for (const int channels : {1, 2}) {
        const std::size_t frames = 6000;
        const Fixture fx = makeFixture(channels, fm.bits, fm.isFloat, frames);
        const fs::path src = t.tmp.dir / ("src_" + std::to_string(static_cast<int>(rate)) + "_" + fm.name + "_" + std::to_string(channels) + ".wav");
        writePcmWav(src, rate, channels, fm.bits, fm.isFloat, fx);
        const std::string before = [&] { std::ifstream f(src, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), {}); }();

        for (const di::Channel ch : {di::Channel::Left, di::Channel::Right, di::Channel::Sum}) {
          if (channels == 1 && ch != di::Channel::Left) continue;  // a mono file has no choice
          INFO(fm.name << " " << rate << " Hz " << channels << " ch, channel " << di::channelName(ch));
          di::Options opt;
          opt.channel = ch;
          const di::Outcome out = di::importFile(t.rec, src, opt);
          REQUIRE(out.ok);
          REQUIRE_FALSE(out.takeName.empty());

          // The take: a 32-bit float mono WAV at the file's own rate.
          double gotRate = 0.0;
          const auto got = takeSamples(t.takes() / (out.takeName + ".wav"), &gotRate);
          CHECK(gotRate == rate);
          REQUIRE(got.size() == frames);
          bool exact = true;
          for (std::size_t i = 0; i < frames; ++i) {
            float want;
            if (channels == 1) want = fx.values[i];
            else if (ch == di::Channel::Left) want = fx.values[2 * i];
            else if (ch == di::Channel::Right) want = fx.values[2 * i + 1];
            else want = 0.5f * (fx.values[2 * i] + fx.values[2 * i + 1]);
            if (got[i] != want) exact = false;
          }
          CHECK(exact);

          // The sidecar.
          const json j = readJsonFile(t.takes() / (out.takeName + ".json"));
          CHECK(j["sampleRate"] == rate);
          CHECK(j["channels"] == 1);
          CHECK(j["lengthSamples"] == frames);
          CHECK(j["overruns"] == 0);
          CHECK(j["playAlong"].is_null());
          CHECK(j["imported"]["source"] == src.filename().string());
          CHECK(j["imported"]["channel"] == (channels == 1 ? "mono" : di::channelName(ch)));
          CHECK(j["imported"]["samePerformance"] == false);
          CHECK(j["imported"]["offsetMs"].is_null());
          CHECK(j.contains("createdUtc"));

          const auto listed = t.rec.listTakes();
          const auto it = std::find_if(listed.begin(), listed.end(), [&](const auto& x) { return x.name == out.takeName; });
          REQUIRE(it != listed.end());
          CHECK(it->imported.present);
          CHECK(it->imported.source == src.filename().string());
          CHECK(it->sampleRate == rate);
          CHECK(it->lengthSamples == static_cast<std::int64_t>(frames));
          CHECK_FALSE(it->offsetMs().has_value());
        }
        // A copy: the original is untouched, still there, still the same bytes.
        REQUIRE(fs::exists(src));
        const std::string afterBytes = [&] { std::ifstream f(src, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), {}); }();
        CHECK(afterBytes == before);
      }
}

TEST_CASE("import: AIFF and FLAC files import too (within one LSB: JUCE's writers scale by 2^31 - 1)", "[import][take]") {
  Takes t;
  for (const bool flac : {false, true})
    for (const int channels : {1, 2}) {
      const Fixture fx = makeFixture(channels, 24, false, 5000);
      const fs::path src = t.tmp.dir / (std::string(flac ? "di_flac_" : "di_aiff_") + std::to_string(channels) + (flac ? ".flac" : ".aif"));
      writeJuceFile(src, flac, 44100.0, channels, 24, fx);
      INFO(src.filename().string());
      const di::Probe pr = di::probe(src);
      REQUIRE(pr.ok());
      CHECK(pr.channels == channels);
      CHECK(pr.sampleRate == 44100.0);
      CHECK(pr.frames == 5000);
      const di::Outcome out = di::importFile(t.rec, src, {});
      REQUIRE(out.ok);
      double rate = 0;
      const auto got = takeSamples(t.takes() / (out.takeName + ".wav"), &rate);
      CHECK(rate == 44100.0);
      REQUIRE(got.size() == 5000);
      double worst = 0.0;
      for (std::size_t i = 0; i < got.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(got[i]) - fx.values[i * static_cast<std::size_t>(channels)]));
      CHECK(worst <= 2.0 / 8388608.0);
      CHECK(readJsonFile(t.takes() / (out.takeName + ".json"))["imported"]["channel"] == (channels == 1 ? "mono" : "left"));
    }
}

TEST_CASE("import: a silent or a clipped file is refused with its reason and leaves nothing behind", "[import][reject]") {
  Takes t;
  const std::size_t frames = 4000;
  {  // silent: -70 dBFS sine
    Fixture fx = makeFixture(1, 32, true, frames);
    for (auto& v : fx.values) v *= 0.0003f / 0.71f;
    const fs::path src = t.tmp.dir / "silent.wav";
    writePcmWav(src, 48000.0, 1, 32, true, fx);
    const di::Outcome out = di::importFile(t.rec, src, {});
    CHECK_FALSE(out.ok);
    CHECK(out.error.find("silent") != std::string::npos);
    CHECK(out.takeName.empty());
  }
  {  // clipped: a flat top of 8 samples
    Fixture fx = makeFixture(1, 32, true, frames);
    for (std::size_t i = 2000; i < 2008; ++i) fx.values[i] = 1.0f;
    const fs::path src = t.tmp.dir / "clipped.wav";
    writePcmWav(src, 48000.0, 1, 32, true, fx);
    const di::Outcome out = di::importFile(t.rec, src, {});
    CHECK_FALSE(out.ok);
    CHECK(out.error.find("clipped") != std::string::npos);
  }
  {  // stereo: only the chosen channel counts (the right one is clipped, the left is fine)
    Fixture fx = makeFixture(2, 32, true, frames);
    for (std::size_t i = 1000; i < 1010; ++i) fx.values[2 * i + 1] = -1.0f;
    const fs::path src = t.tmp.dir / "right_clipped.wav";
    writePcmWav(src, 48000.0, 2, 32, true, fx);
    CHECK(di::importFile(t.rec, src, {}).ok);  // left: fine
    di::Options r;
    r.channel = di::Channel::Right;
    CHECK(di::importFile(t.rec, src, r).error.find("clipped") != std::string::npos);
  }
  {  // digital silence
    Fixture fx = makeFixture(1, 16, false, frames);
    std::fill(fx.values.begin(), fx.values.end(), 0.0f);
    std::fill(fx.codes.begin(), fx.codes.end(), 0);
    const fs::path src = t.tmp.dir / "zero.wav";
    writePcmWav(src, 44100.0, 1, 16, false, fx);
    CHECK(di::importFile(t.rec, src, {}).error.find("silent") != std::string::npos);
  }
  CHECK(countTakeFiles(t.takes()) == 2);  // only the one good import above (its WAV and sidecar)
}

TEST_CASE("import: unreadable, unsupported and surround files are refused before anything is written", "[import][reject]") {
  Takes t;
  const fs::path garbage = t.tmp.dir / "garbage.wav";
  std::ofstream(garbage) << "this is not audio";
  CHECK_FALSE(di::probe(garbage).ok());
  CHECK_FALSE(di::importFile(t.rec, garbage, {}).ok);
  const fs::path txt = t.tmp.dir / "notes.txt";
  std::ofstream(txt) << "x";
  CHECK(di::probe(txt).error.find("WAV, AIFF or FLAC") != std::string::npos);
  CHECK(di::probe(t.tmp.dir / "missing.wav").error.find("does not exist") != std::string::npos);
  const Fixture six = makeFixture(6, 16, false, 1000);
  const fs::path surround = t.tmp.dir / "six.wav";
  writePcmWav(surround, 48000.0, 6, 16, false, six);
  const di::Probe pr = di::probe(surround);
  CHECK(pr.channels == 6);
  CHECK(pr.error.find("mono or stereo") != std::string::npos);
  CHECK_FALSE(di::importFile(t.rec, surround, {}).ok);
  CHECK(countTakeFiles(t.takes()) == 0);
}

TEST_CASE("import: the offset is stored only for the same performance; 'don't know' stores none", "[import][sidecar]") {
  Takes t;
  const Fixture fx = makeFixture(1, 24, false, 3000);
  const fs::path src = t.tmp.dir / "My DI (v2).wav";
  writePcmWav(src, 48000.0, 1, 24, false, fx);
  auto sidecar = [&](const std::string& name) { return readJsonFile(t.takes() / (name + ".json"))["imported"]; };

  di::Options same;
  same.samePerformance = true;
  same.offsetMs = 83500.0;
  const auto a = di::importFile(t.rec, src, same);
  REQUIRE(a.ok);
  CHECK(a.takeName == "My DI (v2)");  // named after the file
  CHECK(sidecar(a.takeName)["samePerformance"] == true);
  CHECK(sidecar(a.takeName)["offsetMs"] == 83500.0);

  di::Options unknown;
  unknown.samePerformance = true;  // "don't know": no offset
  const auto b = di::importFile(t.rec, src, unknown);
  REQUIRE(b.ok);
  CHECK(b.takeName == "My DI (v2)-2");  // the same file twice: a new name, never an overwrite
  CHECK(sidecar(b.takeName)["samePerformance"] == true);
  CHECK(sidecar(b.takeName)["offsetMs"].is_null());

  di::Options other;
  other.offsetMs = 5000.0;  // not the same performance: the position is dropped
  const auto c = di::importFile(t.rec, src, other);
  REQUIRE(c.ok);
  CHECK(sidecar(c.takeName)["samePerformance"] == false);
  CHECK(sidecar(c.takeName)["offsetMs"].is_null());

  const auto takes = t.rec.listTakes();
  REQUIRE(takes.size() == 3);
  for (const auto& tk : takes) {
    if (tk.name == a.takeName) {
      REQUIRE(tk.offsetMs().has_value());
      CHECK(*tk.offsetMs() == Catch::Approx(83500.0));
    } else {
      CHECK_FALSE(tk.offsetMs().has_value());
    }
  }
  // Rename and delete work like for a recorded take (and keep the sidecar with the WAV).
  std::string err;
  REQUIRE(t.rec.renameTake(a.takeName, "renamed", &err));
  CHECK(readJsonFile(t.takes() / "renamed.json")["imported"]["source"] == "My DI (v2).wav");
  REQUIRE(t.rec.removeTake("renamed"));
  CHECK_FALSE(fs::exists(t.takes() / "renamed.wav"));
  CHECK_FALSE(fs::exists(t.takes() / "renamed.json"));
}

TEST_CASE("import: ImportJob runs off the calling thread and reports the outcome", "[import][thread]") {
  Takes t;
  const Fixture fx = makeFixture(2, 16, false, 20000);
  const fs::path src = t.tmp.dir / "job.wav";
  writePcmWav(src, 48000.0, 2, 16, false, fx);
  di::Options opt;
  opt.channel = di::Channel::Sum;
  {
    di::ImportJob job(t.rec, src, opt);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!job.done() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(job.done());
    CHECK(job.outcome().ok);
    CHECK(job.outcome().takeName == "job");
  }
  {  // a job that is destroyed at once cancels and joins: no crash, no half take
    Takes t2;
    di::ImportJob job(t2.rec, src, opt);
  }
  CHECK(t.rec.takesVersion() >= 1);
}

// ---- the match plan and the matcher's command line ------------------------------------------------------------------------------
TEST_CASE("import: only a same-performance take passes --matched mono and --offset-ms; 'don't know' passes no offset", "[import][plan][runner]") {
  using namespace sawblade::plugin;
  TempDir tmp;
  Host h(48000.0, 480);
  PlayAlong& pa = h.p.playAlong();
  pa.setStandalone(true);
  h.p.recorder().setTakesDir(tmp.dir / "takes");
  h.p.matchSettings().setFile(tmp.dir / "settings.xml");
  // A song with a guitar stem.
  const fs::path song = tmp.dir / "song";
  fs::create_directories(song);
  for (const char* n : {"guitar.wav", "drums.wav", "other.wav"}) {
    std::vector<float> l(48000 * 10), r(48000 * 10);
    for (std::size_t i = 0; i < l.size(); ++i) {
      l[i] = static_cast<float>(i + 1) * 1e-7f;
      r[i] = -0.5f * l[i];
    }
    sawblade::writeWavFloat32Stereo(song / n, 48000.0, l, r);
  }
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());

  const Fixture fx = makeFixture(1, 24, false, 48000);
  const fs::path src = tmp.dir / "di.wav";
  writePcmWav(src, 48000.0, 1, 24, false, fx);

  di::Options same;
  same.samePerformance = true;
  same.offsetMs = 83500.0;
  di::Options unknown;
  unknown.samePerformance = true;
  const di::Options different;  // not the same performance, whatever offset there might have been

  fake_tools::Toolbox tb(tmp.dir / "tools");
  tb.cfgMatch({{"progressJson", true}});

  struct Case { const char* label; di::Options opt; bool matched, offset; };
  for (const Case& cs : {Case{"same", same, true, true}, Case{"unknown", unknown, true, false}, Case{"different", different, false, false}}) {
    INFO(cs.label);
    const auto out = di::importFile(h.p.recorder(), src, cs.opt);
    REQUIRE(out.ok);
    chooseTakeForMatch(h.p, out.takeName);
    const MatchPlan plan = planMatch(h.p);
    REQUIRE(plan.ok);
    CHECK(plan.request.matched == cs.matched);
    CHECK(plan.request.offsetMs.has_value() == cs.offset);
    if (cs.offset) CHECK(*plan.request.offsetMs == Catch::Approx(83500.0));
    CHECK(plan.request.di == h.p.recorder().takesDir() / (out.takeName + ".wav"));
    CHECK(plan.request.ref == song / "guitar.wav");
    CHECK_FALSE(plan.offsetNote.empty());

    JobRunner runner(tb.settings, tb.jobs);
    std::string err;
    REQUIRE(runner.startMatch(plan.request, &err));
    REQUIRE(runner.waitFinished(JobKind::Match));
    const auto s = runner.snapshot(JobKind::Match);
    const auto argv = argvOf(s.dir);
    CHECK(after(argv, "--ref-channel") == "mid");
    CHECK(has(argv, "--matched") == cs.matched);
    if (cs.matched) CHECK(after(argv, "--matched") == "mono");
    CHECK(has(argv, "--offset-ms") == cs.offset);
    if (cs.offset) CHECK(std::stod(after(argv, "--offset-ms")) == Catch::Approx(83500.0));
    // The request is in job.json, so a re-attached job (and the thorough pass built from it) keeps the pairing.
    const json jj = readJsonFile(s.dir / "job.json");
    CHECK(jj["request"].value("matched", false) == cs.matched);
  }

  // A recorded take (no `imported`) is as before: no --matched.
  REQUIRE(h.p.recorder().start(song.string()));
  std::vector<float> in(480, 0.1f), outb(480);
  h.process(in.data(), outb.data(), 480);
  h.p.recorder().stop();
  h.process(in.data(), outb.data(), 480);
  REQUIRE(h.p.recorder().waitIdle());
  chooseTakeForMatch(h.p, h.p.recorder().currentTakeName());
  const MatchPlan rp = planMatch(h.p);
  REQUIRE(rp.ok);
  CHECK_FALSE(rp.request.matched);
  JobRunner runner(tb.settings, tb.jobs);
  std::string err;
  REQUIRE(runner.startMatch(rp.request, &err));
  REQUIRE(runner.waitFinished(JobKind::Match));
  CHECK_FALSE(has(argvOf(runner.snapshot(JobKind::Match).dir), "--matched"));
}
