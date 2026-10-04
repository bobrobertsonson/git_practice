// Headless tests of the rig editor's controller and the processor hooks behind it (spec phase 10, section 8,
// tests 1-7): structural edits through the loader, live edits, mutes, off-thread re-measure, blend
// automation, zero allocations on the audio thread, gate LEARN.
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <thread>

#include "processor_harness.h"
#include "rig/InputMeter.h"
#include "rig/RigController.h"

namespace {

using namespace sawblade::plugin::rig;

constexpr double kFs = 48000.0;

// Both paths: identity; B's model can be chosen. blend 0.5, align off, impulse cab, one post peak band.
fs::path writePreset(const fs::path& dir, const std::string& name, const char* bModel = "linear_identity.nam",
                     const char* alignMode = "off", double maxLagMs = 5.0) {
  auto nam = [&](const std::string& id, const char* file) {
    return json{{"id", id}, {"type", "nam"}, {"model", {{"file", (kFixtures / "nam" / file).string()}}}};
  };
  json j = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", name},
            {"paths", {{"a", {{"blocks", json::array({nam("a1", "linear_identity.nam")})}}},
                       {"b", {{"blocks", json::array({nam("b1", bModel)})}}}}},
            {"align", {{"mode", alignMode}, {"maxLagMs", maxLagMs}}},
            {"blend", 0.5},
            {"cab", {{"mode", "shared"}, {"ir", {{"file", (kFixtures / "ir" / "impulse.wav").string()}}}}},
            {"postEq", json::array({{{"type", "peak"}, {"freq", 1000}, {"gainDb", 0.0}, {"q", 1.0}}})}};
  const fs::path p = dir / (name + ".json");
  std::ofstream(p) << j.dump(2);
  return p;
}

Block eqBlock(const std::string& id, double gainDb = 0.0) {
  json f = flatEqFields();
  f["bands"][0]["gainDb"] = gainDb;
  return makeBlock("eq", id, "fx", kFixtures, f);
}

std::vector<float> tone(double hz, std::size_t n, double amp = 0.1) { return sine(hz, kFs, n, amp); }

double toneLevelDb(Host& h, double hz) {
  const auto x = tone(hz, 24000);
  std::vector<float> y;
  h.run(x, y, {256});
  return toDb(rms(y.data() + 12000, 12000) / rms(x.data() + 12000, 12000));
}

void waitLoaded(Host& h) {
  REQUIRE(h.p.waitForLoader());
  REQUIRE(h.p.status().error.empty());
}

// State -> a fresh processor (as a host does on session load); returns it ready and loaded.
std::unique_ptr<Host> restoreCopy(SawbladeProcessor& src) {
  juce::MemoryBlock s;
  src.getStateInformation(s);
  auto h = std::make_unique<Host>(kFs, 512);
  h->p.setStateInformation(s.getData(), static_cast<int>(s.getSize()));
  REQUIRE(h->p.waitForLoader());
  REQUIRE(h->p.status().error.empty());
  return h;
}

// An audio thread that processes blocks continuously with the allocation and lock counters armed.
struct AudioThread {
  SawbladeProcessor& p;
  std::atomic<bool> stop{false};
  std::atomic<long> allocs{0}, locks{0}, blocks{0};
  std::atomic<bool> bad{false};
  std::thread th;
  int block;
  float amp;

  AudioThread(SawbladeProcessor& proc, int blockSize = 256, float a = 0.2f) : p(proc), block(blockSize), amp(a) {
    th = std::thread([this] {
      const auto x = noise(static_cast<std::size_t>(block), 33, amp);
      juce::AudioBuffer<float> storage(2, block);
      juce::MidiBuffer midi;
      while (!stop.load()) {
        float* ch[2] = {storage.getWritePointer(0), storage.getWritePointer(1)};
        std::copy(x.begin(), x.end(), ch[0]);
        juce::AudioBuffer<float> view(ch, 2, block);
        long a2, l2;
        {
          AllocGuard ag;
          LockGuard lg;
          p.processBlock(view, midi);
          a2 = ag.count();
          l2 = lg.count();
        }
        allocs += a2;
        locks += l2;
        for (int i = 0; i < block; ++i)
          if (!std::isfinite(ch[0][i]) || std::fabs(ch[0][i]) > 100.0f) bad = true;
        ++blocks;
        std::this_thread::sleep_for(std::chrono::microseconds(300));
      }
    });
  }
  void finish() {
    stop = true;
    if (th.joinable()) th.join();
  }
  ~AudioThread() { finish(); }
};

}  // namespace

TEST_CASE("RigController: topology through the processor, state round trip at each step", "[rig][controller][topology]") {
  TempDir t;
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "rig"));
  RigController c(h.p);
  CHECK(c.topology() == Topology::Blend);

  // Two pedal slots on path A (eq blocks), each a structural edit with exactly one build.
  std::uint64_t builds = h.p.engineBuilds();
  for (const char* id : {"a2", "a3"}) {
    c.edit([id](Preset& p) { REQUIRE(addBlock(p.a, 0, eqBlock(id))); });
    waitLoaded(h);
    CHECK(h.p.engineBuilds() == ++builds);
  }
  const Preset start = h.p.currentPreset();
  REQUIRE(start.a.blocks.size() == 3);
  CHECK(topologyOf(start) == Topology::Blend);

  c.setTopology(Topology::Single);
  waitLoaded(h);
  CHECK(h.p.engineBuilds() == ++builds);
  CHECK(topologyOf(h.p.currentPreset()) == Topology::Single);
  CHECK(c.topology() == Topology::Single);
  CHECK(h.p.currentPreset().a.blocks[1].bypass);
  CHECK(h.param(kBlend) == 0.0);
  {
    auto r = restoreCopy(h.p);
    CHECK(topologyOf(r->p.currentPreset()) == Topology::Single);
    CHECK(r->p.currentPreset() == h.p.currentPreset());
  }

  c.setTopology(Topology::SinglePlusTwoPedals);
  waitLoaded(h);
  CHECK(h.p.engineBuilds() == ++builds);
  CHECK(topologyOf(h.p.currentPreset()) == Topology::SinglePlusTwoPedals);
  CHECK(c.topology() == Topology::SinglePlusTwoPedals);
  {
    auto r = restoreCopy(h.p);
    CHECK(topologyOf(r->p.currentPreset()) == Topology::SinglePlusTwoPedals);
  }

  c.setTopology(Topology::Blend);
  waitLoaded(h);
  CHECK(h.p.engineBuilds() == ++builds);
  const Preset end = h.p.currentPreset();
  CHECK(topologyOf(end) == Topology::Blend);
  CHECK(h.param(kBlend) == Catch::Approx(0.5).margin(1e-6));  // the remembered blend comes back
  CHECK(end.a.blocks == start.a.blocks);
  CHECK(end.b == start.b);
  {
    auto r = restoreCopy(h.p);
    CHECK(topologyOf(r->p.currentPreset()) == Topology::Blend);
    CHECK(r->p.currentPreset() == end);
  }
}

TEST_CASE("RigController: SINGLE + 2 PEDALS with no second pedal is remembered as UI state until a load",
          "[rig][controller][topology]") {
  TempDir t;
  Host h(kFs, 512);
  const fs::path f = writePreset(t.dir, "rig");
  h.load(f);
  RigController c(h.p);
  c.setTopology(Topology::SinglePlusTwoPedals);
  waitLoaded(h);
  CHECK(topologyOf(h.p.currentPreset()) == Topology::Single);
  CHECK(c.topology() == Topology::SinglePlusTwoPedals);
  h.load(f);  // a user load resets the choice
  c.sync();
  CHECK(c.topology() == Topology::Blend);
}

TEST_CASE("RigController: slot add, move and remove go through the loader", "[rig][controller][slots]") {
  TempDir t;
  test::registerLatencyStub();
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "rig"));
  RigController c(h.p);
  c.setTopology(Topology::Single);
  waitLoaded(h);
  CHECK(toneLevelDb(h, 1000.0) == Catch::Approx(0.0).margin(0.05));  // A only, identity
  std::uint64_t builds = h.p.engineBuilds();
  const long allocs0 = h.allocs, locks0 = h.locks;

  c.edit([](Preset& p) { addBlock(p.a, 1, eqBlock("a2", 6.0)); });  // after the nam block
  waitLoaded(h);
  CHECK(h.p.engineBuilds() == ++builds);
  CHECK(toneLevelDb(h, 1000.0) == Catch::Approx(6.0).margin(0.1));

  c.edit([](Preset& p) { moveBlock(p.a, 1, 0); });  // before the nam block: linear, same output
  waitLoaded(h);
  CHECK(h.p.engineBuilds() == ++builds);
  CHECK(h.p.currentPreset().a.blocks[0].id == "a2");
  CHECK(toneLevelDb(h, 1000.0) == Catch::Approx(6.0).margin(0.1));

  c.edit([](Preset& p) { removeBlock(p.a, 0); });
  waitLoaded(h);
  CHECK(h.p.engineBuilds() == ++builds);
  CHECK(toneLevelDb(h, 1000.0) == Catch::Approx(0.0).margin(0.05));

  CHECK(h.p.getLatencySamples() == 0);
  c.edit([](Preset& p) { addBlock(p.a, 0, makeBlock("test.latency", "a9", "pedal", kFixtures, {{"latency", 100}})); });
  waitLoaded(h);
  CHECK(h.p.engineBuilds() == ++builds);
  CHECK(h.p.getLatencySamples() == 100);
  c.edit([](Preset& p) { setBypass(p.a, 0, true); });
  waitLoaded(h);
  CHECK(h.p.engineBuilds() == ++builds);
  CHECK(h.p.getLatencySamples() == 0);

  CHECK(h.allocs == allocs0);
  if (LockGuard::enabled()) CHECK(h.locks == locks0);
  CHECK_FALSE(h.nonFinite);
}

TEST_CASE("RigController: a failing block keeps the running preset and reports the error", "[rig][controller][slots]") {
  TempDir t;
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "rig"));
  RigController c(h.p);
  const Preset before = h.p.currentPreset();
  c.edit([](Preset& p) {
    json f = {{"model", {{"file", "/does/not/exist.nam"}}}};
    addBlock(p.a, 0, makeBlock("nam", "bad", "pedal", kFixtures, f));
  });
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(h.p.status().error.empty());
  CHECK(h.p.currentPreset() == before);
}

TEST_CASE("RigController: debounced edits are coalesced, latest wins", "[rig][controller]") {
  TempDir t;
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "rig"));
  RigController c(h.p);
  const std::uint64_t builds = h.p.engineBuilds();
  c.editDebounced([](Preset& p) { setGateField(p, GateField::Release, 80.0); });
  c.editDebounced([](Preset& p) { setGateField(p, GateField::Release, 120.0); });
  CHECK(c.hasPending());
  CHECK(c.view().gate.releaseMs == 120.0);  // the display shows the pending value immediately
  CHECK(h.p.currentPreset().gate.releaseMs != 120.0);
  c.flushPending();
  waitLoaded(h);
  CHECK_FALSE(c.hasPending());
  CHECK(h.p.engineBuilds() == builds + 1);
  CHECK(h.p.currentPreset().gate.releaseMs == 120.0);
}

TEST_CASE("RigController: live EQ and block-gain edits need no rebuild and match a loaded preset", "[rig][controller][live]") {
  TempDir t;
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "rig"));
  RigController c(h.p);
  c.edit([](Preset& p) {
    addBlock(p.a, 1, eqBlock("a2", 3.0));
    p.a.eq.push_back(EqBand{EqType::Peak, 2000.0, 0.0, 1.0, true});
  });
  waitLoaded(h);
  h.setParam(kPostEqFirst, 6.0);  // post band 0 (1 kHz peak) gain: host automation
  const std::uint64_t builds = h.p.engineBuilds();

  // post band freq, path EQ gain: live
  h.p.applyLiveEdit([](Preset& p) { setBandLive(p, EqTarget::Post, 0, 2000.0, p.postEq[0].gainDb, 2.0); });
  h.p.applyLiveEdit([](Preset& p) { setBandLive(p, EqTarget::EqA, 0, 2000.0, 9.0, 1.0); });
  const auto x = noise(30000, 77, 0.2f);
  std::vector<float> y;
  h.run(x, y, {200, 64, 512});
  CHECK(h.p.engineBuilds() == builds);
  const auto st = h.p.engineParamState();
  REQUIRE(st.valid);
  CHECK(st.live.postEq[0].freq == 2000.0);
  CHECK(st.live.postEq[0].q == 2.0);
  CHECK(st.live.pathEq[0][0].gainDb == 9.0);
  CHECK(st.live != st.baseline);

  // A processor loaded with the same preset sounds the same once the ramps are over.
  Host ref(kFs, 512);
  REQUIRE(ref.p.loadPresetJson(presetToStateJson(h.p.currentPreset()), t.dir, nullptr));
  REQUIRE(ref.p.waitForLoader());
  std::vector<float> yr;
  ref.run(x, yr, {512});
  double maxDiff = 0.0;
  for (std::size_t i = 4800; i < x.size(); ++i) maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(y[i] - yr[i])));
  CHECK(maxDiff <= 1e-5);

  // The state carries the live edits.
  const json j = json::parse(presetToStateJson(h.p.currentPreset()));
  CHECK(j["postEq"][0]["freq"].get<double>() == 2000.0);
  CHECK(j["paths"]["a"]["eq"][0]["gainDb"].get<double>() == 9.0);
  auto restored = restoreCopy(h.p);
  restored->run(x, yr, {512});
  CHECK(restored->p.presetGeneration() == restored->p.status().generation);
  const auto st2 = restored->p.engineParamState();
  REQUIRE(st2.valid);
  CHECK(st2.live == st2.baseline);
}

TEST_CASE("RigController: eqLive sends a post band's gain through the parameter", "[rig][controller][live]") {
  TempDir t;
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "rig"));
  RigController c(h.p);
  const std::uint64_t builds = h.p.engineBuilds();
  c.beginParam(kPostEqFirst);
  c.eqLive(EqTarget::Post, 0, 1500.0, 4.5, 1.2);
  c.endParam(kPostEqFirst);
  CHECK(h.param(kPostEqFirst) == Catch::Approx(4.5).margin(1e-4));
  const Preset p = h.p.currentPreset();
  CHECK(p.postEq[0].freq == 1500.0);
  CHECK(p.postEq[0].q == 1.2);
  CHECK(p.postEq[0].gainDb == Catch::Approx(4.5).margin(1e-4));
  CHECK(c.postSlotOfBand(0) == 0);
  CHECK(c.postSlotOfBand(3) == -1);
  CHECK(h.p.engineBuilds() == builds);
}

TEST_CASE("RigController: live block input gain and mutes", "[rig][controller][live]") {
  TempDir t;
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "rig"));
  RigController c(h.p);
  c.setTopology(Topology::Single);
  waitLoaded(h);
  const std::uint64_t builds = h.p.engineBuilds();
  const std::vector<float> x(12000, 0.1f);
  std::vector<float> y;
  h.run(x, y, {512});
  CHECK(y.back() == Catch::Approx(0.1).epsilon(1e-4));

  c.live([](Preset& p) { setBlockInputGainDb(p.a, 0, 6.0); });
  h.run(x, y, {512});
  CHECK(y.back() == Catch::Approx(0.1 * std::pow(10.0, 6.0 / 20.0)).epsilon(1e-4));
  CHECK(h.p.engineBuilds() == builds);

  // Mute A (the only audible path): ramps to silence.
  c.setMute(0, true);
  CHECK(h.p.monitor().muteA);
  h.run(x, y, {64});
  for (std::size_t i = 2000; i < y.size(); ++i) REQUIRE(y[i] == 0.0f);
  c.setMute(0, false);
  h.run(x, y, {512});
  CHECK(y.back() == Catch::Approx(0.1 * std::pow(10.0, 6.0 / 20.0)).epsilon(1e-4));

  // Solo A == mute B and not A; solo B on top of it mutes nothing (both soloed).
  c.setSolo(0, true);
  CHECK(h.p.monitor().muteB);
  CHECK_FALSE(h.p.monitor().muteA);
  c.setSolo(1, true);
  CHECK_FALSE(h.p.monitor().muteA);
  CHECK_FALSE(h.p.monitor().muteB);
  c.setSolo(1, false);
  c.setSolo(0, false);
  CHECK_FALSE(h.p.monitor().muteB);

  // A user load clears the monitor state; a structural edit keeps it and a rebuilt engine is muted too.
  c.setMute(0, true);
  c.edit([](Preset& p) { addBlock(p.a, 1, eqBlock("a2")); });
  waitLoaded(h);
  CHECK(h.p.monitor().muteA);
  h.run(x, y, {512});
  h.run(x, y, {512});
  for (std::size_t i = 2000; i < y.size(); ++i) REQUIRE(y[i] == 0.0f);
  h.load(writePreset(t.dir, "other"));
  CHECK_FALSE(h.p.monitor().muteA);
  c.sync();
  CHECK_FALSE(c.muted(0));
}

TEST_CASE("RigController: re-measuring the alignment runs off the audio thread", "[rig][controller][align]") {
  TempDir t;
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "delay", "linear_delay_300.nam", "off", 10.0));
  RigController c(h.p);
  CHECK(h.p.currentPreset().align.mode == AlignMode::Off);
  const std::uint64_t builds = h.p.engineBuilds();
  {
    AudioThread audio(h.p);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    c.remeasure();
    CHECK(h.p.status().alignMeasuring);
    REQUIRE(h.p.waitForLoader());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    audio.finish();
    CHECK(audio.blocks.load() > 20);
    CHECK(audio.allocs.load() == 0);
    if (LockGuard::enabled()) CHECK(audio.locks.load() == 0);
    CHECK_FALSE(audio.bad.load());
  }
  const Preset p = h.p.currentPreset();
  CHECK(p.align.mode == AlignMode::Manual);
  CHECK(p.align.delaySamplesB == -300);
  CHECK_FALSE(p.align.invertB);
  CHECK(p.align.maxLagMs == 10.0);
  const auto st = h.p.status();
  CHECK_FALSE(st.alignMeasuring);
  CHECK(st.measuredAlign.peakCorrelation > 0.99);
  CHECK(st.measuredAlign.delaySamplesB == -300);
  CHECK(h.p.engineBuilds() == builds + 1);  // the write-back needs no rebuild

  auto r = restoreCopy(h.p);
  CHECK(r->p.currentPreset().align.mode == AlignMode::Manual);
  CHECK(r->p.currentPreset().align.delaySamplesB == -300);
  // The manual engine behaves exactly like the measured one.
  const auto x = noise(8000, 9, 0.2f);
  std::vector<float> ya, yb;
  h.run(x, ya, {512});
  r->run(x, yb, {512});
  CHECK(std::equal(ya.begin() + 1500, ya.end(), yb.begin() + 1500));

  // Align nudges in Manual go through the loader, and the controller seeds Auto from the measured values.
  c.nudgeAlign(5);
  waitLoaded(h);
  CHECK(h.p.currentPreset().align.delaySamplesB == -295);
}

TEST_CASE("RigController: the host can automate blend without a rebuild", "[rig][controller][automation]") {
  TempDir t;
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "blend", "linear_05_025.nam"));
  const std::uint64_t builds = h.p.engineBuilds();
  const std::vector<float> x(512, 0.25f);
  std::vector<float> y(512);

  h.setParam(kBlend, 0.0);
  for (int i = 0; i < 8; ++i) h.process(x.data(), y.data(), 512);
  const float a = y.back();
  CHECK(a == Catch::Approx(0.25).epsilon(1e-4));  // path A is the identity
  h.setParam(kBlend, 1.0);
  for (int i = 0; i < 8; ++i) h.process(x.data(), y.data(), 512);
  const float b = y.back();  // the B response to this input
  CHECK(b != Catch::Approx(a).margin(1e-3));
  h.setParam(kBlend, 0.0);
  for (int i = 0; i < 8; ++i) h.process(x.data(), y.data(), 512);

  auto* blend = h.p.getParameters()[kBlend];
  REQUIRE(blend->getName(32) == "Blend (Saw - Body)");
  double maxStep = 0.0;
  float last = y.back();
  for (int i = 0; i <= 200; ++i) {
    blend->setValueNotifyingHost(static_cast<float>(i) / 200.0f);
    h.process(x.data(), y.data(), 512);
    for (float v : y) {
      maxStep = std::max(maxStep, static_cast<double>(std::fabs(v - last)));
      last = v;
    }
  }
  for (int i = 0; i < 4; ++i) h.process(x.data(), y.data(), 512);  // the last 20 ms ramp settles
  CHECK(y.back() == Catch::Approx(b).epsilon(1e-4));
  // 1/200 of the blend range per block, spread over a 20 ms ramp: no sample-to-sample jump.
  CHECK(maxStep < 0.5 * std::fabs(b - a) / 200.0 + 1e-6);
  CHECK(h.p.engineBuilds() == builds);
  juce::MemoryBlock s;
  h.p.getStateInformation(s);
  const json j = json::parse(std::string(static_cast<const char*>(s.getData()), s.getSize()));
  CHECK(j["blend"].get<double>() == Catch::Approx(1.0).margin(1e-4));
  CHECK_FALSE(h.nonFinite);
  CHECK(h.allocs == 0);
}

TEST_CASE("RigController: processBlock stays allocation- and lock-free under concurrent edits", "[rig][controller][rt]") {
  TempDir t;
  Host h(kFs, 256);
  h.load(writePreset(t.dir, "rt", "linear_05_025.nam", "auto", 5.0));
  RigController c(h.p);
  c.edit([](Preset& p) { addBlock(p.a, 0, eqBlock("a2", 3.0)); p.a.eq.push_back(EqBand{EqType::Peak, 800.0, 2.0, 1.0, true}); });
  waitLoaded(h);

  AudioThread audio(h.p, 256);
  int i = 0;
  bool remeasured = false;
  while (audio.blocks.load() < 2000) {
    const double ph = 0.1 * i;
    c.live([&](Preset& p) {
      setBandLive(p, EqTarget::EqA, 0, 800.0 * std::pow(2.0, std::sin(ph)), 6.0 * std::sin(ph * 1.3), 1.0 + 0.5 * std::cos(ph));
      setBandLive(p, EqTarget::Post, 0, 1000.0 * std::pow(2.0, std::cos(ph)), p.postEq[0].gainDb, 1.0);
      setBlockInputGainDb(p.a, 1, 4.0 * std::sin(ph));
    });
    c.setParam(kPostEqFirst, 6.0 * std::sin(ph * 0.7));
    if (i % 10 == 3) c.setMute(i % 20 == 3 ? 0 : 1, (i / 10) % 2 == 0);
    if (i % 25 == 5) {
      c.edit([&](Preset& p) {
        if (p.postEq.size() > 1) removeBand(p, EqTarget::Post, 1);
        else addBand(p, EqTarget::Post, EqBand{EqType::HighShelf, 5000.0, 1.0, 0.7, true});
      });
    }
    if (!remeasured && i == 40) {
      c.remeasure();
      remeasured = true;
    }
    ++i;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  audio.finish();
  CHECK(audio.allocs.load() == 0);
  if (LockGuard::enabled()) CHECK(audio.locks.load() == 0);
  CHECK_FALSE(audio.bad.load());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.status().error.empty());
}

TEST_CASE("RigController: gate LEARN sets the threshold 6 dB above the quiet input", "[rig][controller][learn]") {
  TempDir t;
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "learn"));
  RigController c(h.p);
  CHECK(h.param(kGateThreshold) == Catch::Approx(-55.0).margin(1e-6));

  // No blocks processed: "no input", the parameter is unchanged.
  c.learnGate();
  CHECK(c.learning());
  CHECK(c.learnStatus().find("don't play") != std::string::npos);
  c.finishLearn();
  CHECK_FALSE(c.learning());
  CHECK(c.learnStatus() == "no input");
  CHECK(h.param(kGateThreshold) == Catch::Approx(-55.0).margin(1e-6));

  // -60 dBFS noise for 1 s.
  c.learnGate();
  const auto x = noise(static_cast<std::size_t>(kFs), 5, 0.001f);
  std::vector<float> y;
  h.run(x, y, {512});
  c.finishLearn();
  CHECK(c.learnStatus().rfind("thr set -", 0) == 0);
  CHECK(h.param(kGateThreshold) == Catch::Approx(-54.0).margin(1.0));

  // The gate sits after the input gain: +6 dB of input gain moves the learned threshold up by 6 dB.
  h.setParam(kInputGain, 6.0);
  c.learnGate();
  h.run(noise(static_cast<std::size_t>(kFs), 5, 0.001f), y, {512});
  c.finishLearn();
  CHECK(h.param(kGateThreshold) == Catch::Approx(-48.0).margin(1.0));
  h.setParam(kInputGain, 0.0);

  // A loud take clamps at -20 dB; silence at -80 dB.
  c.learnGate();
  h.run(noise(4800, 6, 0.9f), y, {512});
  c.finishLearn();
  CHECK(h.param(kGateThreshold) == Catch::Approx(-20.0).margin(1e-6));
  c.learnGate();
  h.run(std::vector<float>(4800, 0.0f), y, {512});
  c.finishLearn();
  CHECK(h.param(kGateThreshold) == Catch::Approx(-80.0).margin(1e-6));
}

TEST_CASE("InputMeter: ring of block peaks", "[rig][meter]") {
  InputMeter m;
  CHECK(m.counter() == 0);
  CHECK(m.since(0).empty());
  for (int i = 1; i <= 3; ++i) m.push(static_cast<float>(i));
  const auto v = m.since(1);
  REQUIRE(v.size() == 2);
  CHECK(v[0] == 2.0f);
  CHECK(v[1] == 3.0f);
  for (int i = 0; i < 1000; ++i) m.push(7.0f);
  CHECK(m.since(0).size() == InputMeter::kSize);  // only the last 512 blocks are kept
  CHECK(m.counter() == 1003);
}

TEST_CASE("RigController: an edit that supersedes a pending re-measure leaves no stuck state", "[rig][controller][align]") {
  TempDir t;
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "delay", "linear_delay_300.nam", "off", 10.0));
  RigController c(h.p);
  h.p.remeasureAlignment();
  CHECK(h.p.status().alignMeasuring);
  c.edit([](Preset& p) { addBlock(p.a, 1, eqBlock("a2")); });  // replaces the re-measure request in the loader
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(h.p.status().alignMeasuring);
  CHECK(h.p.currentPreset().align.mode == AlignMode::Off);
  CHECK(h.p.currentPreset().a.blocks.size() == 2);
  CHECK(h.p.status().error.empty());
}

TEST_CASE("RigController: mutes survive a rebuild without a blip", "[rig][controller][live]") {
  TempDir t;
  Host h(kFs, 512);
  h.load(writePreset(t.dir, "rig"));
  RigController c(h.p);
  c.setTopology(Topology::Single);
  waitLoaded(h);
  const std::vector<float> x(8000, 0.1f);
  std::vector<float> y;
  c.setMute(0, true);
  h.run(x, y, {512});
  c.edit([](Preset& p) { addBlock(p.a, 1, eqBlock("a2")); });
  waitLoaded(h);
  h.run(x, y, {64});  // the cross-fade: the old engine is silent, the new one must start silent too
  for (const float v : y) REQUIRE(v == 0.0f);
}
