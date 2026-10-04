// Tests of the play-along backing in the plugin (docs/specs/phase5_2_playalong_plugin.md): settings,
// loudness rule, command queue, transport in Standalone and plugin mode, rig-latency alignment, state,
// and real-time safety of processBlock with the backing playing. Stems are synthesised into a temp dir.
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <thread>

#include "PlayAlong.h"
#include "processor_harness.h"
#include "sawblade/loudness.h"
#include "sawblade/wav_io.h"

namespace {

constexpr double kFs = 48000.0;

// Distinct, exactly representable ramps: L[i] = (i + 1) * 1e-6 * scale, R = -L / 2.
struct Stems {
  std::vector<float> l, r;
};
Stems rampStems(std::size_t n, float scale = 1.0f) {
  Stems s;
  s.l.resize(n);
  s.r.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    s.l[i] = static_cast<float>(i + 1) * 1e-6f * scale;
    s.r[i] = -0.5f * s.l[i];
  }
  return s;
}

// A song folder holding drums.wav = the ramp stems (48 kHz, so no resampling) plus optional extras.
fs::path writeSong(const fs::path& root, const std::string& name, const Stems& drums) {
  const fs::path d = root / name;
  fs::create_directories(d);
  writeWavFloat32Stereo(d / "drums.wav", kFs, drums.l, drums.r);
  return d;
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

struct Out {
  std::vector<float> l, r;
};

// Runs `blocks` blocks of `n` samples of silence in; the host head (if any) advances by n per block.
Out run(Host& h, int blocks, int n, FakeHead* head = nullptr) {
  Out o;
  const std::vector<float> in(static_cast<std::size_t>(n), 0.0f);
  std::vector<float> l(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n));
  for (int b = 0; b < blocks; ++b) {
    h.process(in.data(), l.data(), n, nullptr, r.data());
    o.l.insert(o.l.end(), l.begin(), l.end());
    o.r.insert(o.r.end(), r.begin(), r.end());
    if (head && head->playing) head->pos += n;
  }
  return o;
}

constexpr int kFade = 240;  // 5 ms at 48 kHz

}  // namespace

// ---- pure pieces ------------------------------------------------------------------------------------
TEST_CASE("PlayAlong: suggested backing level = rig loudness - backing loudness, clamped", "[playalong][level]") {
  using sawblade::plugin::suggestedBackingLevelDb;
  CHECK(suggestedBackingLevelDb(-20.0, -14.0) == Catch::Approx(-6.0));
  CHECK(suggestedBackingLevelDb(-14.0, -20.0) == Catch::Approx(6.0));  // exactly the top of the range
  CHECK(suggestedBackingLevelDb(-10.0, -30.0) == kBackingLevelMaxDb);  // +20 clamps to +6
  CHECK(suggestedBackingLevelDb(-60.0, -3.0) == kBackingLevelMinDb);   // -57 clamps to -40
  // No rig signal observed: the fixed -18 LUFS reference.
  CHECK(suggestedBackingLevelDb(std::nullopt, -14.0) == Catch::Approx(-4.0));
  // No backing loudness (silent stems): the default level.
  CHECK(suggestedBackingLevelDb(-20.0, std::nullopt) == kBackingLevelDefaultDb);
  CHECK(suggestedBackingLevelDb(std::nullopt, std::nullopt) == kBackingLevelDefaultDb);
}

TEST_CASE("PlayAlong: RigLoudness tracks a tone, ignores silence, and is none before 0.5 s of signal", "[playalong][level]") {
  RigLoudness rl;
  rl.prepare(kFs);
  CHECK_FALSE(rl.lufs().has_value());
  const auto silence = std::vector<float>(48000, 0.0f);
  rl.process(silence.data(), static_cast<int>(silence.size()));
  CHECK_FALSE(rl.lufs().has_value());
  const auto tone = sine(997.0, kFs, 48000 * 4, 0.1);
  rl.process(tone.data(), 12000);  // 0.25 s
  CHECK_FALSE(rl.lufs().has_value());
  rl.process(tone.data() + 12000, static_cast<int>(tone.size()) - 12000);
  REQUIRE(rl.lufs().has_value());
  // Two equal channels of a 0.1-amplitude 997 Hz sine: the K-weighting gain at 997 Hz is +0.691 dB, which the
  // -0.691 offset cancels, so it is 10 log10(2 * 0.1^2 / 2) = -20.0 LUFS.
  CHECK(*rl.lufs() == Catch::Approx(-20.0).margin(0.15));
  // Silence in between does not lower the estimate.
  rl.process(silence.data(), static_cast<int>(silence.size()));
  CHECK(*rl.lufs() == Catch::Approx(-20.0).margin(0.15));
  // Agrees with the core's integrated loudness of the same dual-mono signal.
  const auto ref = integratedLoudnessLufs(tone.data(), tone.data(), static_cast<std::int64_t>(tone.size()), kFs);
  REQUIRE(ref.has_value());
  CHECK(*rl.lufs() == Catch::Approx(*ref).margin(0.2));
}

TEST_CASE("PlayAlong: settings JSON round trip and tolerant parsing", "[playalong][state]") {
  PlayAlongSettings s;
  CHECK(s.isDefault());
  s.folder = "/songs/x";
  s.offsetMs = -190.5;
  s.loopOn = true;
  s.loopAMs = 1000.0;
  s.loopBMs = 9000.0;
  s.countIn = true;
  s.bpm = 142.0;
  s.guitarMode = GuitarMode::Ghost;
  s.levelDb = -7.25;
  s.keepOther = true;
  s.hostSync = true;
  CHECK_FALSE(s.isDefault());
  const json j = playAlongToJson(s);
  CHECK(playAlongFromJson(j) == s);
  CHECK(j["guitarMode"] == "ghost");
  CHECK(j["otherRole"] == "other");

  // Wrong types, out-of-range numbers, unknown keys and a non-object never throw.
  const json bad = {{"folder", 5}, {"offsetMs", "x"}, {"loop", 3}, {"countIn", {{"bpm", 9999}, {"on", "yes"}}},
                    {"guitarMode", "loud"}, {"backingLevelDb", 1000}, {"otherRole", 1}, {"future", true}};
  PlayAlongSettings b;
  REQUIRE_NOTHROW(b = playAlongFromJson(bad));
  CHECK(b.folder.empty());
  CHECK(b.offsetMs == 0.0);
  CHECK(b.bpm == 300.0);
  CHECK(b.guitarMode == GuitarMode::Muted);
  CHECK(b.levelDb == kBackingLevelMaxDb);
  CHECK_FALSE(b.keepOther);
  CHECK(playAlongFromJson(json(3)).isDefault());
  CHECK(playAlongFromJson(json::array()).isDefault());
}

TEST_CASE("PlayAlong: command queue is FIFO, bounded and safe across threads", "[playalong][queue]") {
  PlayAlongQueue q;
  PlayAlongCmd c;
  CHECK_FALSE(q.pop(c));
  for (std::size_t i = 0; i < PlayAlongQueue::kCapacity; ++i) REQUIRE(q.push({PlayAlongCmd::Type::Seek, static_cast<std::int64_t>(i), 0, 0.0}));
  CHECK_FALSE(q.push({PlayAlongCmd::Type::Pause, 0, 0, 0.0}));  // full: nothing stored
  for (std::size_t i = 0; i < PlayAlongQueue::kCapacity; ++i) {
    REQUIRE(q.pop(c));
    REQUIRE(c.a == static_cast<std::int64_t>(i));
  }
  CHECK_FALSE(q.pop(c));

  constexpr std::int64_t kN = 200000;
  std::thread producer([&] {
    for (std::int64_t i = 0; i < kN;) {
      if (q.push({PlayAlongCmd::Type::Seek, i, 0, 0.0})) ++i;
    }
  });
  std::int64_t next = 0;
  bool ordered = true;
  while (next < kN) {
    if (q.pop(c)) {
      ordered = ordered && c.a == next;
      ++next;
    }
  }
  producer.join();
  CHECK(ordered);
}

// ---- Standalone: free-run transport -------------------------------------------------------------------
TEST_CASE("PlayAlong Standalone: play, seek, pause and loop through the command queue", "[playalong][standalone]") {
  TempDir t;
  const Stems st = rampStems(120000);
  const fs::path song = writeSong(t.dir, "song", st);
  Host h(kFs, 256);
  PlayAlong& pa = h.p.playAlong();
  pa.setStandalone(true);
  h.prepare(kFs, 256);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  const auto status = pa.loadStatus();
  REQUIRE(status.state == PlayAlong::LoadStatus::State::Ready);
  CHECK(status.songName == "song");
  CHECK(status.lengthSeconds == Catch::Approx(2.5));
  pa.setLevelDb(0.0);

  run(h, 2, 256);  // adopt the set, apply the level
  CHECK(pa.snapshot().hasSet);
  CHECK(pa.snapshot().length == 120000);
  CHECK_FALSE(pa.snapshot().playing);

  pa.seekSamples(30000);
  pa.play();
  const Out o = run(h, 20, 256);
  CHECK(pa.snapshot().playing);
  for (int i = kFade + 8; i < 20 * 256; ++i) {
    REQUIRE(o.l[static_cast<std::size_t>(i)] == st.l[static_cast<std::size_t>(30000 + i)]);
    REQUIRE(o.r[static_cast<std::size_t>(i)] == st.r[static_cast<std::size_t>(30000 + i)]);
  }

  pa.pause();
  run(h, 8, 256);
  CHECK_FALSE(pa.snapshot().playing);
  const Out quiet = run(h, 2, 256);
  for (float v : quiet.l) REQUIRE(v == 0.0f);

  // Loop: 0.2 s .. 0.5 s, playing from inside it; the playhead stays within it.
  pa.setLoopMs(200.0, 500.0, true);
  pa.seekSamples(10000);
  pa.play();
  std::int64_t lo = 1 << 30, hi = 0;
  for (int b = 0; b < 200; ++b) {
    run(h, 1, 256);
    if (b < 10) continue;
    const auto sn = pa.snapshot();
    lo = std::min(lo, sn.position);
    hi = std::max(hi, sn.position);
  }
  CHECK(pa.snapshot().loopActive);
  CHECK(lo >= 9600);
  CHECK(hi < 24000 + 256);
  CHECK(pa.commandsDropped() == 0);

  // The loop survives a set swap (re-applied once the new set is adopted).
  pa.pause();
  run(h, 8, 256);
  const fs::path song2 = writeSong(t.dir, "song2", rampStems(100000, 2.0f));
  pa.loadFolder(song2.string(), true);
  REQUIRE(pa.waitForLoader());
  run(h, 4, 256);
  CHECK(pa.snapshot().length == 100000);
  CHECK(pa.snapshot().loopActive);
  CHECK(pa.loadStatus().songName == "song2");
  CHECK(h.allocs == 0);
  if (LockGuard::enabled()) CHECK(h.locks == 0);
}

TEST_CASE("PlayAlong Standalone: count-in clicks first, then the stems", "[playalong][standalone]") {
  TempDir t;
  const Stems st = rampStems(300000);
  const fs::path song = writeSong(t.dir, "song", st);
  Host h(kFs, 512);
  PlayAlong& pa = h.p.playAlong();
  pa.setStandalone(true);
  h.prepare(kFs, 512);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  pa.setLevelDb(0.0);
  pa.setCountIn(true, 120.0);  // one bar = 2 s
  run(h, 2, 512);
  pa.play();
  const Out first = run(h, 93, 512);  // 0.99 s
  CHECK(pa.snapshot().countingIn);
  CHECK(pa.snapshot().position == 0);
  double peak = 0.0;
  for (float v : first.l) peak = std::max(peak, static_cast<double>(std::fabs(v)));
  CHECK(peak > 0.2);  // the -6 dB click
  CHECK(peak < 0.6);
  run(h, 110, 512);  // past 2 s
  CHECK_FALSE(pa.snapshot().countingIn);
  CHECK(pa.snapshot().position > 0);
  pa.setCountIn(false, 120.0);
}

TEST_CASE("PlayAlong Standalone: the backing is delayed by the rig latency", "[playalong][latency]") {
  TempDir t;
  const fs::path presetFile = writeIdentityPreset(t.dir, "lat", 100);  // reports and has a 100-sample delay
  const Stems st = rampStems(80000);
  const fs::path song = writeSong(t.dir, "song", st);
  Host h(kFs, 256);
  PlayAlong& pa = h.p.playAlong();
  pa.setStandalone(true);
  h.prepare(kFs, 256);
  h.load(presetFile);
  REQUIRE(h.p.getLatencySamples() == 100);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  pa.setLevelDb(0.0);
  run(h, 2, 256);
  pa.play();
  const Out o = run(h, 30, 256);
  // Output sample j carries stem sample j - 100 (the same delay the rig has).
  for (int j = 100 + kFade + 8; j < 30 * 256; ++j) REQUIRE(o.l[static_cast<std::size_t>(j)] == st.l[static_cast<std::size_t>(j - 100)]);
  for (int j = 0; j < 100; ++j) REQUIRE(o.l[static_cast<std::size_t>(j)] == 0.0f);
  CHECK(h.allocs == 0);
}

// ---- plugin mode: off by default, follows the host once enabled ------------------------------------------
TEST_CASE("PlayAlong plugin: the backing is off by default and follows the host transport when enabled", "[playalong][plugin]") {
  TempDir t;
  const Stems st = rampStems(200000);
  const fs::path song = writeSong(t.dir, "song", st);
  Host h(kFs, 256);
  PlayAlong& pa = h.p.playAlong();
  REQUIRE_FALSE(pa.standalone());  // a processor built outside a wrapper is plugin mode
  FakeHead head;
  h.p.setPlayHead(&head);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  pa.setLevelDb(0.0);

  // Host playing, sync off: silence.
  head.playing = true;
  head.pos = 4800;
  const Out off = run(h, 10, 256, &head);
  for (float v : off.l) REQUIRE(v == 0.0f);
  CHECK(pa.snapshot().hasSet);
  CHECK_FALSE(pa.snapshot().playing);
  CHECK_FALSE(pa.snapshot().following);

  // Enable: the backing follows the host position (a host sample p plays stem sample p).
  pa.setHostSync(true);
  head.playing = false;
  run(h, 2, 256, &head);
  head.playing = true;
  head.pos = 4800;
  const Out on = run(h, 20, 256, &head);
  CHECK(pa.snapshot().following);
  CHECK(pa.snapshot().playing);
  for (int j = kFade + 8; j < 20 * 256; ++j) {
    REQUIRE(on.l[static_cast<std::size_t>(j)] == st.l[static_cast<std::size_t>(4800 + j)]);
    REQUIRE(on.r[static_cast<std::size_t>(j)] == st.r[static_cast<std::size_t>(4800 + j)]);
  }

  // A host jump (loop, locate) is followed.
  head.pos = 90000;
  const Out jumped = run(h, 6, 256, &head);
  for (int j = 256; j < 6 * 256; ++j) REQUIRE(jumped.l[static_cast<std::size_t>(j)] == Catch::Approx(st.l[static_cast<std::size_t>(90000 + j)]).margin(1e-6));

  // Host stops: the backing fades out and stops; sync off silences it again.
  head.playing = false;
  run(h, 4, 256, &head);
  CHECK_FALSE(pa.snapshot().playing);
  head.playing = true;
  pa.setHostSync(false);
  run(h, 4, 256, &head);
  const Out off2 = run(h, 4, 256, &head);
  for (float v : off2.l) REQUIRE(v == 0.0f);
  CHECK(h.allocs == 0);
  if (LockGuard::enabled()) CHECK(h.locks == 0);
  h.p.setPlayHead(nullptr);
}

TEST_CASE("PlayAlong plugin: a start offset moves the song inside the host timeline", "[playalong][plugin][offset]") {
  TempDir t;
  const Stems st = rampStems(100000);
  const fs::path song = writeSong(t.dir, "song", st);
  Host h(kFs, 256);
  PlayAlong& pa = h.p.playAlong();
  FakeHead head;
  h.p.setPlayHead(&head);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  pa.setLevelDb(0.0);
  pa.setHostSync(true);
  // The matcher's sign: the DI starts 100 ms (4800 samples) inside the song: stem audio from 4800 plays at host 0.
  pa.setOffsetMs(100.0);
  head.playing = false;
  head.pos = 0;
  run(h, 2, 256, &head);
  head.playing = true;
  head.pos = 0;
  const Out o = run(h, 20, 256, &head);
  for (int j = kFade + 8; j < 20 * 256; ++j) REQUIRE(o.l[static_cast<std::size_t>(j)] == st.l[static_cast<std::size_t>(j + 4800)]);
  // Negative: the song starts 50 ms into the timeline; silence before it.
  head.playing = false;
  run(h, 8, 256, &head);
  pa.setOffsetMs(-50.0);
  run(h, 2, 256, &head);
  head.playing = true;
  head.pos = 0;
  const Out n = run(h, 20, 256, &head);
  for (int j = 0; j < 2400; ++j) REQUIRE(n.l[static_cast<std::size_t>(j)] == 0.0f);
  for (int j = 2400 + 8; j < 20 * 256; ++j) REQUIRE(n.l[static_cast<std::size_t>(j)] == st.l[static_cast<std::size_t>(j - 2400)]);
  h.p.setPlayHead(nullptr);
}

// ---- suggested level ---------------------------------------------------------------------------------
TEST_CASE("PlayAlong: the level is suggested once, on a user load, from the observed rig loudness", "[playalong][level]") {
  TempDir t;
  // Stems at about -17 LUFS (stereo noise amplitude 0.25).
  Stems loud;
  loud.l = noise(96000, 5, 0.25f);
  loud.r = noise(96000, 6, 0.25f);
  const fs::path song = writeSong(t.dir, "loud", loud);

  SECTION("no rig signal observed yet: the -18 LUFS reference") {
    Host h(kFs, 512);
    PlayAlong& pa = h.p.playAlong();
    REQUIRE_FALSE(pa.rigLoudnessLufs().has_value());
    pa.loadFolder(song.string(), true);
    REQUIRE(pa.waitForLoader());
    const auto st = pa.loadStatus();
    REQUIRE(st.backingLufs.has_value());
    REQUIRE(st.suggestedLevelDb.has_value());
    CHECK(*st.suggestedLevelDb == Catch::Approx(std::clamp(kRigReferenceLufs - *st.backingLufs, kBackingLevelMinDb, kBackingLevelMaxDb)));
    CHECK(pa.settings().levelDb == Catch::Approx(*st.suggestedLevelDb));
  }

  SECTION("with a rig signal: rig loudness minus backing loudness, and never adjusted again") {
    Host h(kFs, 512);
    PlayAlong& pa = h.p.playAlong();
    // The rig is the pass-through Init preset: the DI is the rig output. 3 s of noise at amplitude 0.05.
    const auto di = noise(48000 * 3, 8, 0.05f);
    std::vector<float> out;
    h.run(di, out, {512});
    REQUIRE(pa.rigLoudnessLufs().has_value());
    const double rig = *pa.rigLoudnessLufs();
    CHECK(rig == Catch::Approx(*integratedLoudnessLufs(out.data(), out.data(), static_cast<std::int64_t>(out.size()), kFs)).margin(0.5));
    pa.loadFolder(song.string(), true);
    REQUIRE(pa.waitForLoader());
    const auto st = pa.loadStatus();
    REQUIRE(st.backingLufs.has_value());
    const double expect = std::clamp(rig - *st.backingLufs, kBackingLevelMinDb, kBackingLevelMaxDb);
    CHECK(pa.settings().levelDb == Catch::Approx(expect).margin(1e-9));
    CHECK(expect < -3.0);  // a quiet rig against a loud song: the backing comes down

    // Louder rig later: the level does not follow.
    const auto di2 = noise(48000 * 3, 9, 0.4f);
    h.run(di2, out, {512});
    CHECK(pa.settings().levelDb == Catch::Approx(expect).margin(1e-9));

    // Loading again is a user action: it suggests again (from the new rig loudness).
    pa.loadFolder(song.string(), true);
    REQUIRE(pa.waitForLoader());
    CHECK(pa.settings().levelDb != Catch::Approx(expect).margin(0.5));
  }

  SECTION("a state restore keeps the saved level; a reload (rate or role change) does too") {
    Host h(kFs, 512);
    PlayAlong& pa = h.p.playAlong();
    PlayAlongSettings s;
    s.folder = song.string();
    s.levelDb = -7.5;
    pa.restore(s);
    REQUIRE(pa.waitForLoader());
    CHECK(pa.loadStatus().state == PlayAlong::LoadStatus::State::Ready);
    CHECK_FALSE(pa.loadStatus().suggestedLevelDb.has_value());
    CHECK(pa.settings().levelDb == -7.5);
    pa.setKeepOther(true);
    REQUIRE(pa.waitForLoader());
    CHECK(pa.settings().levelDb == -7.5);
    h.prepare(44100.0, 512);  // new rate: the song reloads at it
    REQUIRE(pa.waitForLoader());
    CHECK(pa.settings().levelDb == -7.5);
  }

  SECTION("clamped to the control range") {
    Stems quiet;
    quiet.l = noise(96000, 5, 0.002f);
    quiet.r = noise(96000, 6, 0.002f);
    const fs::path q = writeSong(t.dir, "quiet", quiet);
    Host h(kFs, 512);
    h.p.playAlong().loadFolder(q.string(), true);
    REQUIRE(h.p.playAlong().waitForLoader());
    CHECK(h.p.playAlong().settings().levelDb == kBackingLevelMaxDb);
  }
}

// ---- state ----------------------------------------------------------------------------------------------
TEST_CASE("PlayAlong: state carries the optional playAlong object; older state loads unchanged", "[playalong][state]") {
  TempDir t;
  const fs::path song = writeSong(t.dir, "song", rampStems(60000));
  Host a(kFs, 512);
  juce::MemoryBlock plain;
  a.p.getStateInformation(plain);
  CHECK_FALSE(json::parse(std::string(static_cast<const char*>(plain.getData()), plain.getSize())).contains("playAlong"));  // untouched

  PlayAlong& pa = a.p.playAlong();
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  pa.setOffsetMs(-120.0);
  pa.setLoopMs(500.0, 1000.0, true);
  pa.setCountIn(true, 133.0);
  pa.setGuitarMode(GuitarMode::Full);
  pa.setLevelDb(-9.0);
  pa.setHostSync(true);
  pa.setKeepOther(true);
  REQUIRE(pa.waitForLoader());
  juce::MemoryBlock saved;
  a.p.getStateInformation(saved);
  const json j = json::parse(std::string(static_cast<const char*>(saved.getData()), saved.getSize()));
  REQUIRE(j.contains("playAlong"));
  CHECK(j["playAlong"]["folder"] == song.string());
  CHECK(j["playAlong"]["offsetMs"] == -120.0);
  CHECK(j["playAlong"]["otherRole"] == "other");
  CHECK(j["playAlong"]["guitarMode"] == "full");
  CHECK(j["playAlong"]["backingLevelDb"] == -9.0);
  CHECK(j["playAlong"]["countIn"]["bpm"] == 133.0);
  CHECK(j["playAlong"]["loop"]["on"] == true);
  CHECK(j["playAlong"]["hostSync"] == true);

  // Restore into a fresh processor, before prepareToPlay (as hosts do): the folder loads once prepared.
  SawbladeProcessor b;
  b.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
  CHECK(b.playAlong().settings() == pa.settings());
  CHECK(b.playAlong().settings().levelDb == -9.0);  // the restore keeps the saved level
  b.setRateAndBufferSizeDetails(kFs, 512);
  b.prepareToPlay(kFs, 512);
  REQUIRE(b.playAlong().waitForLoader());
  CHECK(b.playAlong().loadStatus().state == PlayAlong::LoadStatus::State::Ready);
  CHECK(b.playAlong().settings().levelDb == -9.0);
  // And a state round trip is stable.
  juce::MemoryBlock again;
  b.getStateInformation(again);
  CHECK(std::string(static_cast<const char*>(again.getData()), again.getSize()) == std::string(static_cast<const char*>(saved.getData()), saved.getSize()));

  // A state without the object (older sessions) leaves the play-along as it is.
  b.setStateInformation(plain.getData(), static_cast<int>(plain.getSize()));
  CHECK(b.playAlong().settings() == pa.settings());
  // Garbage in the object is tolerated.
  json g = json::parse(std::string(static_cast<const char*>(plain.getData()), plain.getSize()));
  g["playAlong"] = "nonsense";
  const std::string gs = g.dump();
  REQUIRE_NOTHROW(b.setStateInformation(gs.data(), static_cast<int>(gs.size())));
}

TEST_CASE("PlayAlong: a missing, empty or unreadable folder shows a message and never throws", "[playalong][state]") {
  TempDir t;
  Host h(kFs, 512);
  PlayAlong& pa = h.p.playAlong();
  using State = PlayAlong::LoadStatus::State;

  PlayAlongSettings s;
  s.folder = (t.dir / "gone").string();
  REQUIRE_NOTHROW(pa.restore(s));
  REQUIRE(pa.waitForLoader());
  CHECK(pa.loadStatus().state == State::Failed);
  CHECK(pa.loadStatus().message.find("not found") != std::string::npos);
  CHECK(pa.settings().folder == s.folder);  // kept: the drive may be mounted later

  const fs::path empty = t.dir / "empty";
  fs::create_directories(empty);
  REQUIRE_NOTHROW(pa.loadFolder(empty.string(), true));
  REQUIRE(pa.waitForLoader());
  CHECK(pa.loadStatus().state == State::Failed);
  CHECK(pa.loadStatus().message.find("No .wav or .flac stems") != std::string::npos);

  { std::ofstream(empty / "drums.wav") << "garbage"; }
  REQUIRE_NOTHROW(pa.loadFolder(empty.string(), true));
  REQUIRE(pa.waitForLoader());
  CHECK(pa.loadStatus().state == State::Failed);
  CHECK(pa.loadStatus().message.find("drums.wav") != std::string::npos);

  // The audio path is unaffected.
  const auto x = noise(2048, 3, 0.4f);
  std::vector<float> y;
  h.run(x, y, {512});
  CHECK(y == x);
  CHECK_FALSE(pa.snapshot().hasSet);

  // A good folder afterwards recovers.
  const fs::path song = writeSong(t.dir, "song", rampStems(48000));
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  CHECK(pa.loadStatus().state == State::Ready);
  CHECK(pa.loadStatus().message.empty());
}

TEST_CASE("PlayAlong: 4-stem other is the guitar by default, KEEP KEYS keeps it", "[playalong][role]") {
  TempDir t;
  const fs::path d = t.dir / "four";
  fs::create_directories(d);
  const Stems o = rampStems(48000 * 2);
  writeWavFloat32Stereo(d / "bass.wav", kFs, noise(48000 * 2, 1, 0.2f), noise(48000 * 2, 2, 0.2f));
  writeWavFloat32Stereo(d / "other.wav", kFs, o.l, o.r);
  Host h(kFs, 256);
  PlayAlong& pa = h.p.playAlong();
  pa.setStandalone(true);
  h.prepare(kFs, 256);
  pa.loadFolder(d.string(), true);
  REQUIRE(pa.waitForLoader());
  CHECK(pa.loadStatus().otherMappedToGuitar);
  CHECK(pa.loadStatus().hasGuitarStem);
  pa.setLevelDb(0.0);
  pa.play();
  run(h, 30, 256);
  CHECK(pa.snapshot().playing);

  pa.pause();
  run(h, 8, 256);
  pa.setKeepOther(true);
  REQUIRE(pa.waitForLoader());
  CHECK_FALSE(pa.loadStatus().otherMappedToGuitar);
  CHECK_FALSE(pa.loadStatus().hasGuitarStem);
}

// ---- real-time safety --------------------------------------------------------------------------------------
TEST_CASE("PlayAlong: processBlock allocates and locks nothing with the backing playing", "[playalong][rt]") {
  TempDir t;
  const Stems st = rampStems(400000);
  const fs::path songA = writeSong(t.dir, "songA", st);
  const fs::path songB = writeSong(t.dir, "songB", rampStems(300000, 0.5f));
  for (const bool standalone : {true, false}) {
    CAPTURE(standalone);
    Host h(kFs, 128);
    PlayAlong& pa = h.p.playAlong();
    pa.setStandalone(standalone);
    h.prepare(kFs, 128);
    FakeHead head;
    h.p.setPlayHead(&head);
    pa.loadFolder(songA.string(), true);
    REQUIRE(pa.waitForLoader());
    pa.setLevelDb(0.0);
    pa.setHostSync(true);
    pa.setCountIn(true, 240.0);  // one bar = 1 s (Standalone)
    pa.setLoopMs(1000.0, 3000.0, true);
    pa.setGuitarMode(GuitarMode::Ghost);
    const std::vector<int> sizes{128, 1, 64, 4096, 333, 512, 17};
    const auto in = noise(8192, 3, 0.3f);
    std::vector<float> out(8192), out2(8192);
    run(h, 3, 128, &head);  // adopt the set (not counted below: the guard is armed per process() call anyway)
    REQUIRE(pa.snapshot().length == 400000);
    h.allocs = h.locks = 0;

    struct Probe {
      int audible = 0, countIn = 0;
      bool wrapped = false;
      std::int64_t first = -1, last = -1;
    };
    // One block of silence-through-the-rig; the backing is whatever the output adds to the input.
    auto block = [&](int n, Probe& pr) {
      h.process(in.data(), out.data(), n, nullptr, out2.data());
      head.pos += n;
      double e = 0.0;
      for (int i = 0; i < n; ++i) {
        const double d = static_cast<double>(out[static_cast<std::size_t>(i)]) - in[static_cast<std::size_t>(i)];
        e += d * d;
      }
      const auto sn = pa.snapshot();
      if (std::sqrt(e / n) > 1e-3) ++pr.audible;
      if (sn.countingIn) ++pr.countIn;
      if (pr.last >= 0 && sn.position < pr.last - 1000) pr.wrapped = true;
      if (sn.playing && !sn.countingIn) {
        if (pr.first < 0) pr.first = sn.position;
        pr.last = sn.position;
      }
    };

    // Phase 1: play (count-in, then the loop wraps in Standalone) with commands arriving from the main thread.
    Probe p1;
    if (standalone) {
      pa.seekSamples(120000);
      pa.play();
    } else {
      head.playing = true;
      head.pos = 120000;
    }
    for (int b = 0; b < 1400; ++b) {
      if (b % 60 == 7) pa.setLevelDb(-3.0 - (b % 7));
      if (b % 97 == 5) pa.setGuitarMode(b % 2 ? GuitarMode::Full : GuitarMode::Muted);
      if (b % 311 == 100 && !standalone) pa.seekSamples(5);  // ignored in host-follow: must be harmless
      block(sizes[static_cast<std::size_t>(b) % sizes.size()], p1);
    }
    CHECK(pa.snapshot().playing);
    CHECK(p1.audible > 300);  // the backing is really there, not the idle path
    CHECK(p1.last > p1.first);
    if (standalone) {
      CHECK(p1.countIn > 30);  // about 1 s of blocks
      CHECK(p1.wrapped);  // the loop wrapped inside the measured window
      CHECK(pa.snapshot().loopActive);
    }

    // Phase 2: stop, swap the set with a NON-user reload (no automatic pause), and check it was adopted.
    if (standalone) pa.pause();
    else head.playing = false;
    for (int b = 0; b < 40; ++b) block(128, p1);
    CHECK_FALSE(pa.snapshot().playing);
    pa.loadFolder(songB.string(), false);
    REQUIRE(pa.waitForLoader());
    for (int b = 0; b < 10; ++b) block(128, p1);
    CHECK(pa.snapshot().length == 300000);  // songB adopted
    CHECK(pa.loadStatus().songName == "songB");
    if (standalone) CHECK(pa.snapshot().loopActive);  // re-applied to the new set

    // Phase 3: play the new set.
    Probe p3;
    if (standalone) {
      pa.seekSamples(100000);
      pa.play();
    } else {
      head.playing = true;
      head.pos = 100000;
    }
    for (int b = 0; b < 700; ++b) block(sizes[static_cast<std::size_t>(b) % sizes.size()], p3);
    CHECK(pa.snapshot().playing);
    CHECK(p3.audible > 150);
    CHECK((standalone ? p3.wrapped : p3.last > p3.first));  // advances; Standalone also wraps the re-applied loop

    CHECK(h.allocs == 0);
    if (LockGuard::enabled()) CHECK(h.locks == 0);
    CHECK_FALSE(h.nonFinite);
    h.p.setPlayHead(nullptr);
  }
}

TEST_CASE("PlayAlong plugin: with host sync off the backing is silent, with a set loaded and the host playing", "[playalong][plugin]") {
  TempDir t;
  const fs::path song = writeSong(t.dir, "song", rampStems(100000));
  Host h(kFs, 256);
  PlayAlong& pa = h.p.playAlong();
  FakeHead head;
  h.p.setPlayHead(&head);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  pa.setLevelDb(6.0);
  CHECK_FALSE(pa.settings().hostSync);
  head.playing = true;
  head.pos = 0;
  const Out o = run(h, 40, 256, &head);
  for (std::size_t i = 0; i < o.l.size(); ++i) {
    REQUIRE(o.l[i] == 0.0f);
    REQUIRE(o.r[i] == 0.0f);
  }
  CHECK(pa.snapshot().hasSet);
  CHECK_FALSE(pa.snapshot().playing);
  h.p.setPlayHead(nullptr);
}

TEST_CASE("PlayAlong Standalone: a user load while playing pauses so the new set is adopted; a non-user load does not", "[playalong][standalone]") {
  TempDir t;
  const fs::path a = writeSong(t.dir, "a", rampStems(200000));
  const fs::path b = writeSong(t.dir, "b", rampStems(150000));
  Host h(kFs, 256);
  PlayAlong& pa = h.p.playAlong();
  pa.setStandalone(true);
  h.prepare(kFs, 256);
  pa.loadFolder(a.string(), true);
  REQUIRE(pa.waitForLoader());
  run(h, 2, 256);
  pa.play();
  run(h, 20, 256);
  REQUIRE(pa.snapshot().playing);

  pa.loadFolder(b.string(), false);  // reload: no pause, the old set keeps playing
  REQUIRE(pa.waitForLoader());
  run(h, 40, 256);
  CHECK(pa.snapshot().playing);
  CHECK(pa.snapshot().length == 200000);

  pa.loadFolder(b.string(), true);  // user load: pauses, then the new set is adopted
  REQUIRE(pa.waitForLoader());
  run(h, 40, 256);
  CHECK_FALSE(pa.snapshot().playing);
  CHECK(pa.snapshot().length == 150000);
}

TEST_CASE("PlayAlong: a full command queue is resynchronised from the settings", "[playalong][queue]") {
  TempDir t;
  const Stems st = rampStems(200000);
  const fs::path song = writeSong(t.dir, "song", st);
  Host h(kFs, 256);
  PlayAlong& pa = h.p.playAlong();
  pa.setStandalone(true);
  h.prepare(kFs, 256);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  pa.setLevelDb(0.0);
  run(h, 3, 256);
  pa.seekSamples(50000);
  pa.play();
  run(h, 20, 256);
  REQUIRE(pa.snapshot().playing);
  REQUIRE_FALSE(pa.needsResync());

  // Flood with no audio running: the queue fills, later commands are dropped, the last value is among them.
  for (int i = 0; i < 400; ++i) pa.setLevelDb(-1.0 - (i % 8));  // ends at -8
  pa.setLevelDb(-12.0);
  CHECK(pa.commandsDropped() > 0);
  CHECK(pa.needsResync());
  CHECK(pa.settings().levelDb == -12.0);

  // The audio side drains what fit (a stale level), then the resync re-sends the settings.
  run(h, 4, 256);
  pa.resyncIfNeeded();  // what the loader's 0.5 s tick does
  CHECK_FALSE(pa.needsResync());
  const Out o = run(h, 30, 256);
  const double gain = std::pow(10.0, -12.0 / 20.0);
  const std::size_t i = o.l.size() - 1;
  const double pos = static_cast<double>(pa.snapshot().position) - 1.0;  // sample just rendered
  CHECK(static_cast<double>(o.l[i]) == Catch::Approx(gain * st.l[static_cast<std::size_t>(pos)]).epsilon(1e-3));

  // The loader tick does it on its own.
  for (int k = 0; k < 400; ++k) pa.setLevelDb(-2.0);
  pa.setLevelDb(-4.0);
  REQUIRE(pa.needsResync());
  run(h, 4, 256);
  for (int k = 0; k < 40 && pa.needsResync(); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_FALSE(pa.needsResync());
  const Out o2 = run(h, 30, 256);
  const double g2 = std::pow(10.0, -4.0 / 20.0);
  const double pos2 = static_cast<double>(pa.snapshot().position) - 1.0;
  CHECK(static_cast<double>(o2.l.back()) == Catch::Approx(g2 * st.l[static_cast<std::size_t>(pos2)]).epsilon(1e-3));
}

TEST_CASE("PlayAlong: a non-object playAlong in the state is ignored and the tone state still loads", "[playalong][state]") {
  Host a(kFs, 512);
  a.setParam(kBlend, 0.3);
  a.setParam(kInputGain, 4.0);
  juce::MemoryBlock plain;
  a.p.getStateInformation(plain);
  for (const json& bad : {json("nonsense"), json(7), json::array({1}), json(nullptr)}) {
    json g = json::parse(std::string(static_cast<const char*>(plain.getData()), plain.getSize()));
    g["playAlong"] = bad;
    const std::string gs = g.dump();
    SawbladeProcessor b;
    b.setStateInformation(gs.data(), static_cast<int>(gs.size()));
    CHECK(b.status().error.empty());
    CHECK(b.parameters().getRawParameterValue("blend")->load() == Catch::Approx(0.3f).margin(1e-4));
    CHECK(b.parameters().getRawParameterValue("inputGain")->load() == Catch::Approx(4.0f).margin(1e-4));
    CHECK(b.playAlong().settings().isDefault());
  }
}
