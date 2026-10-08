// v0.8 I2 Parts 2 and 3 in the engine and the processor: calibration off builds the pre-I2 engine bit for bit, the toggle and the device
// record reach the engine through the Settings store only, blocks without metadata are reported for the "uncalibrated" mark, the notice
// state, trims are measured with the calibration (and never written into the preset), a hop block gets no make-up, and the live gate's
// floor: seeded from the device record on prepare, learned on the audio thread (no allocation, no lock), persisted by the message thread
// with throttling, never taken from a preset. Headless (no window): the processor is driven like a host.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "Engine.h"
#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "alloc_guard.h"
#include "lock_guard.h"
#include "sawblade/auto_trim.h"
#include "sawblade/gate.h"
#include "settings/DeviceCalibration.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::plugin;
using namespace sawblade::test;
using Catch::Approx;
using nlohmann::json;
namespace fs = std::filesystem;

#include "processor_harness.h"

namespace {

constexpr double kRate = 48000.0;

json nb(const std::string& id, const char* file, const char* slot = "amp") {
  return json{{"id", id}, {"type", "nam"}, {"slot", slot}, {"model", {{"file", (kFixtures / "nam" / file).string()}}}};
}

// Path A = `a`; path B = the linear identity; blend 0.5, the impulse cab. `liveGate`: a match-origin preset whose live gate is floor
// relative (the live dynamics policy), with a stored gate that differs from preset to preset by `gateDb`.
json calRig(const std::vector<json>& a, const std::string& name = "cal", bool liveGate = false, double gateDb = -50.0, double inputDb = 10.0) {
  json blocks = json::array();
  for (const auto& b : a) blocks.push_back(b);
  json j = {{"schema", "sawblade.preset"}, {"version", liveGate ? 4 : 3}, {"name", name},
            {"input", {{"gainDb", inputDb}}},
            {"paths", {{"a", {{"blocks", blocks}}}, {"b", {{"blocks", json::array({nb("b1", "linear_identity.nam")})}}}}},
            {"align", {{"mode", "off"}}},
            {"blend", 0.5},
            {"cab", {{"mode", "shared"}, {"ir", {{"file", (kFixtures / "ir" / "impulse.wav").string()}}}}}};
  if (liveGate) {
    j["gate"] = {{"enabled", true}, {"thresholdDb", gateDb}, {"holdMs", 10.0}, {"releaseMs", 20.0}, {"attackMs", 1.0}, {"hysteresisDb", 5.0},
                 {"releaseCurve", "linear-db"}};
    j["origin"] = "match";
    j["dynamicsMode"] = "live";
  }
  return j;
}

fs::path writeJson(const fs::path& dir, const std::string& name, const json& j) {
  const fs::path p = dir / (name + ".json");
  std::ofstream(p) << j.dump(2);
  return p;
}

Preset parse(const json& j) { return parsePreset(j, kFixtures); }

std::vector<float> runEngine(Engine& e, const std::vector<float>& x, int block = 512) {
  std::vector<float> y(x.size());
  for (std::size_t pos = 0; pos < x.size();) {
    const auto n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos));
    e.process(x.data() + pos, y.data() + pos, n);
    pos += static_cast<std::size_t>(n);
  }
  return y;
}

LevelWorker::MakeupResult makeupOf(SawbladeProcessor& p, const Preset& before, const Preset& after, int path, int blockIndex) {
  std::promise<LevelWorker::MakeupResult> pr;
  const auto done = [&](const LevelWorker::MakeupResult& r) { pr.set_value(r); };
  if (blockIndex == -2) p.computeSlotMakeup(before, after, path, done);  // the 4-argument form
  else p.computeSlotMakeup(before, after, path, blockIndex, done);
  auto f = pr.get_future();
  REQUIRE(f.wait_for(std::chrono::seconds(60)) == std::future_status::ready);
  return f.get();
}

struct World {
  SettingsEnv env;
  TempDir tmp;
  Host h;
  explicit World(const char* settingsJson = "{}") : env(settingsJson), h(kRate, 512) { h.p.setLevelDebounceMs(0); }
  settings::Settings& st() { return settings::Settings::shared(); }
  void settle() { REQUIRE(h.p.waitForLoader()); }
  // Applies the Settings change the way the 10 Hz timer would.
  void tick() {
    h.p.calibrationTick();
    settle();
  }
};

settings::DeviceCalibrationRecord recordOf(const char* presetId) { return settings::recordFromPreset(*settings::findDevicePreset(presetId), "2026-10-08"); }

}  // namespace

// ---- calibration off is the pre-I2 engine ---------------------------------------------------------------------------------------
TEST_CASE("device calibration engine: calibration off builds the very same engine, whatever the record and the seed say", "[devicecal][engine]") {
  const Preset p = parse(calRig({nb("p1", "cal_pedal_a.nam", "pedal"), nb("a1", "cal_amp_hi.nam")}, "off", /*liveGate=*/true));
  auto plain = Engine::build(p, kRate, 512);
  EngineCalibration offWithEverything;
  offWithEverything.chain.enabled = false;
  offWithEverything.chain.device.dbu = 20.0;
  offWithEverything.gateFloorSeedDb = -42.0;
  auto off = Engine::build(p, kRate, 512, nullptr, offWithEverything);
  CHECK_FALSE(plain->calibrationSummary().enabled);
  CHECK_FALSE(off->calibrationSummary().enabled);
  CHECK(off->calibrationSummary().uncalibratedBlocks.empty());
  CHECK(off->gateFloorSeedDb() == Gate::kFloorSeedDb);  // the seed is used only with calibration on
  const auto x = noise(static_cast<std::size_t>(2.0 * kRate), 11, 0.05f);
  const auto a = runEngine(*plain, x), b = runEngine(*off, x);
  REQUIRE(a.size() == b.size());
  CHECK(a == b);  // bit-identical
  // On it is a different engine (the pedal and amp metadata move the level), so the comparison above is not vacuous.
  EngineCalibration on;
  on.chain.enabled = true;
  on.chain.device.dbu = 12.0;
  auto cal = Engine::build(p, kRate, 512, nullptr, on);
  const auto c = runEngine(*cal, x);
  CHECK(c != a);
}

TEST_CASE("device calibration engine: the summary names the capture blocks whose metadata lacks a level and says when +12 is assumed", "[devicecal][engine]") {
  const Preset noMeta = parse(calRig({nb("a1", "cal_amp_nometa.nam")}));
  const Preset full = parse(calRig({nb("p1", "cal_pedal_a.nam", "pedal"), nb("a1", "cal_amp_hi.nam")}));
  EngineCalibration assumed;
  assumed.chain.enabled = true;  // no device dBu
  auto e1 = Engine::build(noMeta, kRate, 512, nullptr, assumed);
  CHECK(e1->calibrationSummary().enabled);
  CHECK(e1->calibrationSummary().deviceAssumed);
  CHECK(e1->calibrationSummary().anyUncalibrated);
  // Path B is the linear identity fixture, which has no levels either: it is listed too (path A first).
  CHECK(e1->calibrationSummary().uncalibratedBlocks == std::vector<std::string>{"a1", "b1"});
  auto e2 = Engine::build(full, kRate, 512, nullptr, assumed);
  CHECK(e2->calibrationSummary().deviceAssumed);
  CHECK(e2->calibrationSummary().uncalibratedBlocks == std::vector<std::string>{"b1"});  // the pedal and the amp of path A carry both levels
}

// ---- Settings reach the engine; the record is not plugin state -----------------------------------------------------------------
TEST_CASE("device calibration: the toggle is off by default and the processor builds an uncalibrated engine", "[devicecal][plugin]") {
  World w;
  CHECK_FALSE(w.st().calibratedInputLevels());
  CHECK_FALSE(w.h.p.currentEngineCalibration().chain.enabled);
  w.h.load(writeJson(w.tmp.dir, "p", calRig({nb("a1", "cal_amp_nometa.nam")})));
  CHECK_FALSE(w.h.p.status().calibrationOn);
  CHECK_FALSE(w.h.p.status().calibrationAssumed);
  CHECK(w.h.p.uncalibratedBlocks().empty());
  // A device record alone (toggle off) changes nothing: no rebuild, no notice.
  const auto builds = w.h.p.engineBuilds();
  REQUIRE(w.st().setDeviceCalibration(recordOf("scarlett-4i4-3g")).ok);
  w.tick();
  CHECK(w.h.p.engineBuilds() == builds);
  CHECK_FALSE(w.h.p.status().calibrationAssumed);
}

TEST_CASE("device calibration: turning the toggle on rebuilds with calibration; no record shows the assumed notice state", "[devicecal][plugin]") {
  World w;
  w.h.load(writeJson(w.tmp.dir, "p", calRig({nb("a1", "cal_amp_nometa.nam")})));
  const auto builds = w.h.p.engineBuilds();
  REQUIRE(w.st().setCalibratedInputLevels(true).ok);
  w.tick();
  CHECK(w.h.p.engineBuilds() == builds + 1);
  auto s = w.h.p.status();
  CHECK(s.calibrationOn);
  CHECK(s.calibrationAssumed);  // the main view shows "Interface not calibrated: assuming +12 dBu"
  CHECK(s.uncalibratedBlocks == std::vector<std::string>{"a1", "b1"});
  CHECK(w.h.p.uncalibratedBlocks() == s.uncalibratedBlocks);
  CHECK(settings::uncalibratedNotice() == "Interface not calibrated: assuming +12 dBu");
  // Nothing changed since: the next tick does not rebuild again.
  w.tick();
  CHECK(w.h.p.engineBuilds() == builds + 1);
  // A device record: no longer assumed (one rebuild for the new dBu).
  REQUIRE(w.st().setDeviceCalibration(recordOf("scarlett-4i4-3g")).ok);
  w.tick();
  CHECK(w.h.p.engineBuilds() == builds + 2);
  s = w.h.p.status();
  CHECK(s.calibrationOn);
  CHECK_FALSE(s.calibrationAssumed);
  CHECK(w.h.p.currentEngineCalibration().chain.device.dbu == 12.5);
  // Changing only metadata of the record (a date, the learned floor) never rebuilds.
  REQUIRE(w.st().setLiveGateFloor(-60.0).ok);
  w.tick();
  CHECK(w.h.p.engineBuilds() == builds + 2);
  // Off again: the pre-I2 engine.
  REQUIRE(w.st().setCalibratedInputLevels(false).ok);
  w.tick();
  CHECK(w.h.p.engineBuilds() == builds + 3);
  CHECK_FALSE(w.h.p.status().calibrationOn);
  CHECK(w.h.p.uncalibratedBlocks().empty());
}

TEST_CASE("device calibration: neither the preset nor the plugin state contains the device record", "[devicecal][plugin]") {
  World w;
  REQUIRE(w.st().setDeviceCalibration(recordOf("scarlett-4i4-3g-pad")).ok);
  REQUIRE(w.st().setLiveGateFloor(-43.0).ok);
  REQUIRE(w.st().setCalibratedInputLevels(true).ok);
  w.h.load(writeJson(w.tmp.dir, "p", calRig({nb("a1", "cal_amp_hi.nam")}, "p", true)));
  const std::string preset = presetToStateJson(w.h.p.currentPreset());
  juce::MemoryBlock mb;
  w.h.p.getStateInformation(mb);
  const std::string state = mb.toString().toStdString();
  for (const char* key : {"deviceCalibration", "liveGateFloor", "gainAtMinimum", "calibratedInputLevels", "scarlett"}) {
    INFO(key);
    CHECK(preset.find(key) == std::string::npos);
    CHECK(state.find(key) == std::string::npos);
  }
  // The device record never reaches a preset file, and a preset never reaches the record.
  const auto before = w.st().deviceCalibration();
  w.h.load(writeJson(w.tmp.dir, "q", calRig({nb("a1", "cal_amp_lo.nam")}, "q", true, -60.0)));
  CHECK(w.st().deviceCalibration() == before);
}

// ---- level measurements follow the calibration ----------------------------------------------------------------------------------
TEST_CASE("device calibration: the trim is measured with the calibration, kept apart from the uncalibrated one, and never written into the preset",
          "[devicecal][plugin][levelmatch]") {
  World w;
  w.h.load(writeJson(w.tmp.dir, "p", calRig({nb("p1", "cal_pedal_a.nam", "pedal"), nb("a1", "cal_amp_hi.nam")})));
  w.h.p.levelTick();
  REQUIRE(w.h.p.waitForLevelWork());
  const double offTrim = w.h.p.status().trimDb;
  const Preset stored = w.h.p.currentPreset();
  CHECK_FALSE(stored.autoTrim.hash.empty());  // uncalibrated: the measured trim is written back, as before I2
  CHECK(stored.autoTrim.db == Approx(offTrim).margin(1e-9));

  REQUIRE(w.st().setCalibratedInputLevels(true).ok);
  w.tick();
  w.h.p.levelTick();
  REQUIRE(w.h.p.waitForLevelWork());
  const double onTrim = w.h.p.status().trimDb;
  CHECK(std::fabs(onTrim - offTrim) > 1.0);  // the calibrated chain plays at another level, so it needs another trim
  const Preset now = w.h.p.currentPreset();
  CHECK(now.autoTrim.hash == stored.autoTrim.hash);  // the calibrated trim is not in the preset
  CHECK(now.autoTrim.db == Approx(stored.autoTrim.db).margin(1e-9));
  // The measurement is the core's calibrated one.
  ChainCalibration c;
  c.enabled = true;  // no device record yet: +12 assumed
  const auto expected = computeAutoTrim(now, nullptr, nullptr, c);
  REQUIRE(expected.has_value());
  CHECK(onTrim == Approx(expected->trimDb).margin(0.01));

  REQUIRE(w.st().setCalibratedInputLevels(false).ok);
  w.tick();
  w.h.p.levelTick();
  REQUIRE(w.h.p.waitForLevelWork());
  CHECK(w.h.p.status().trimDb == Approx(offTrim).margin(1e-9));  // back to the uncalibrated trim, from the cache
}

TEST_CASE("device calibration: a swap of a block that feeds another NAM gets no make-up with calibration on; the last block does", "[devicecal][plugin][swap]") {
  World w;
  const Preset before = parse(calRig({nb("p1", "cal_pedal_a.nam", "pedal"), nb("a1", "cal_amp_hi.nam")}));
  const Preset after = parse(calRig({nb("p1", "cal_pedal_b.nam", "pedal"), nb("a1", "cal_amp_hi.nam")}));
  // Off: today's make-up for the pedal swap, whatever the block index.
  for (int idx : {-2, -1, 0}) {
    INFO("off, block " << idx);
    const auto r = makeupOf(w.h.p, before, after, 0, idx);
    CHECK_FALSE(r.skippedHop);
    CHECK(r.makeupDb.has_value());
    CHECK(r.error.empty());
  }
  const double offMakeup = *makeupOf(w.h.p, before, after, 0, -2).makeupDb;
  CHECK(*makeupOf(w.h.p, before, after, 0, 0).makeupDb == Approx(offMakeup).margin(1e-12));  // the block index changes nothing off

  REQUIRE(w.st().setCalibratedInputLevels(true).ok);
  REQUIRE(w.st().setDeviceCalibration(recordOf("scarlett-4i4-3g")).ok);
  // On: the pedal feeds the amp, so its make-up is skipped (no render, 0, flagged) ...
  const auto hop = makeupOf(w.h.p, before, after, 0, 0);
  CHECK(hop.skippedHop);
  REQUIRE(hop.makeupDb.has_value());
  CHECK(*hop.makeupDb == 0.0);
  CHECK(hop.error.empty());
  // ... the amp is the last block, so it keeps its make-up (measured with the calibration); an unknown index is not skipped either.
  const Preset ampAfter = parse(calRig({nb("p1", "cal_pedal_a.nam", "pedal"), nb("a1", "lstm.nam")}));
  const Preset ampBefore = parse(calRig({nb("p1", "cal_pedal_a.nam", "pedal"), nb("a1", "wavenet.nam")}));
  const auto last = makeupOf(w.h.p, ampBefore, ampAfter, 0, 1);
  CHECK_FALSE(last.skippedHop);
  CHECK(last.makeupDb.has_value());
  ChainCalibration c;
  c.enabled = true;
  c.device.dbu = 12.5;
  CHECK(*last.makeupDb == Approx(*slotMakeupDb(ampBefore, ampAfter, 0, nullptr, nullptr, c, 1)).margin(1e-9));
  CHECK_FALSE(makeupOf(w.h.p, before, after, 0, -1).skippedHop);
  CHECK_FALSE(makeupOf(w.h.p, before, after, 0, -2).skippedHop);
}

// ---- the live gate floor --------------------------------------------------------------------------------------------------------
TEST_CASE("gate floor: the engine seeds the follower from the device record on prepare and ignores the preset", "[devicecal][gatefloor][engine]") {
  const EngineCalibration none;
  EngineCalibration seeded;
  seeded.chain.enabled = true;
  seeded.chain.device.dbu = 12.5;
  seeded.gateFloorSeedDb = -44.0;
  const Preset a = parse(calRig({nb("a1", "cal_amp_hi.nam")}, "a", true, -50.0));
  const Preset b = parse(calRig({nb("a1", "cal_amp_lo.nam")}, "b", true, -62.0));
  CHECK(Engine::build(a, kRate, 512, nullptr, none)->gateFloorSeedDb() == Gate::kFloorSeedDb);  // no stored floor: today's -70
  CHECK(Engine::build(a, kRate, 512, nullptr, seeded)->gateFloorSeedDb() == -44.0);
  CHECK(Engine::build(b, kRate, 512, nullptr, seeded)->gateFloorSeedDb() == -44.0);  // the preset does not matter
  EngineCalibration offSeeded = seeded;
  offSeeded.chain.enabled = false;
  CHECK(Engine::build(a, kRate, 512, nullptr, offSeeded)->gateFloorSeedDb() == Gate::kFloorSeedDb);

  // A loud floor is learned within the 3 s window when seeded, and the learned value is readable through the atomic.
  const auto x = noise(static_cast<std::size_t>(4.5 * kRate), 3, 0.002f);
  // The key is the INPUT-gained signal (+10 dB in this preset).
  std::vector<float> keyed(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) keyed[i] = x[i] * static_cast<float>(std::pow(10.0, 10.0 / 20.0));
  const double ref = peakFloorDb(keyed, kRate);
  REQUIRE(ref > -60.0);
  REQUIRE(ref < -41.0);
  EngineCalibration atRef = seeded;
  atRef.gateFloorSeedDb = ref;
  auto e = Engine::build(a, kRate, 512, nullptr, atRef);
  CHECK(std::isnan(e->learnedGateFloorDb()));
  std::vector<float> y(x.size());
  {
    AllocGuard guard;
    for (std::size_t pos = 0; pos < x.size(); pos += 512) e->process(x.data() + pos, y.data() + pos, static_cast<int>(std::min<std::size_t>(512, x.size() - pos)));
    CHECK(guard.count() == 0);  // the audio thread allocates nothing, including the publication of the floor
  }
  REQUIRE(std::isfinite(e->learnedGateFloorDb()));
  CHECK(std::fabs(static_cast<double>(e->learnedGateFloorDb()) - ref) < 3.0);
  // The default seed (-70) has not learned a floor this loud by then.
  auto d = Engine::build(a, kRate, 512, nullptr, none);
  (void)runEngine(*d, x);
  CHECK(std::isnan(d->learnedGateFloorDb()));
}

TEST_CASE("gate floor: the processor seeds from the record only with calibration on, and a preset load never changes it", "[devicecal][gatefloor][plugin]") {
  World w;
  REQUIRE(w.st().setDeviceCalibration(recordOf("scarlett-4i4-3g")).ok);
  REQUIRE(w.st().setLiveGateFloor(-43.0).ok);
  CHECK_FALSE(w.h.p.currentEngineCalibration().gateFloorSeedDb.has_value());  // toggle off: today's seed
  REQUIRE(w.st().setCalibratedInputLevels(true).ok);
  CHECK(w.h.p.currentEngineCalibration().gateFloorSeedDb == -43.0);
  for (const char* name : {"one", "two", "three"}) {
    w.h.load(writeJson(w.tmp.dir, name, calRig({nb("a1", "cal_amp_hi.nam")}, name, true, name[1] == 'w' ? -62.0 : -48.0)));
    CHECK(w.h.p.currentEngineCalibration().gateFloorSeedDb == -43.0);
    CHECK(w.st().deviceCalibration()->liveGateFloorDbfs == -43.0);
  }
}

TEST_CASE("gate floor: learned on the audio thread, persisted by the message thread into the record, throttled; no allocation or lock in process()",
          "[devicecal][gatefloor][plugin]") {
  World w;
  w.h.p.setFloorPersistIntervalMs(0);
  // No record: nothing is stored, whatever is learned.
  w.h.load(writeJson(w.tmp.dir, "live", calRig({nb("a1", "linear_identity.nam")}, "live", true)));
  const auto x = noise(static_cast<std::size_t>(4.5 * kRate), 3, 0.002f);
  std::vector<float> keyed(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) keyed[i] = x[i] * static_cast<float>(std::pow(10.0, 10.0 / 20.0));
  const double ref = peakFloorDb(keyed, kRate);
  REQUIRE(ref < -41.0);
  REQUIRE(ref > -60.0);
  w.h.p.calibrationTick();
  CHECK_FALSE(w.st().deviceCalibration().has_value());

  // With a record whose seed is the loud floor (as a second session would have it), the floor is learned in 4.5 s.
  REQUIRE(w.st().setDeviceCalibration(recordOf("scarlett-4i4-3g")).ok);
  REQUIRE(w.st().setLiveGateFloor(ref).ok);
  REQUIRE(w.st().setCalibratedInputLevels(true).ok);
  w.tick();  // rebuild: now seeded
  std::vector<float> y;
  w.h.run(x, y, {512});
  CHECK(w.h.allocs == 0);
  CHECK(w.h.locks == 0);
  CHECK_FALSE(w.h.nonFinite);
  // Make the stored value stale, then let the message thread persist what the audio thread learned.
  REQUIRE(w.st().setLiveGateFloor(-90.0).ok);
  w.h.p.calibrationTick();
  const auto learned = w.st().deviceCalibration()->liveGateFloorDbfs;
  REQUIRE(learned.has_value());
  CHECK(std::fabs(*learned - ref) < 4.0);
  // Throttled: within the interval a stale value is left alone ...
  w.h.p.setFloorPersistIntervalMs(600000);
  REQUIRE(w.st().setLiveGateFloor(-90.0).ok);
  w.h.p.calibrationTick();
  CHECK(w.st().deviceCalibration()->liveGateFloorDbfs == -90.0);
  // ... and when the interval has passed it is written again.
  w.h.p.setFloorPersistIntervalMs(0);
  w.h.p.calibrationTick();
  CHECK(std::fabs(*w.st().deviceCalibration()->liveGateFloorDbfs - *learned) < 1.0);
  // A change of less than 1 dB is not worth a write.
  REQUIRE(w.st().setLiveGateFloor(*learned + 0.5).ok);
  w.h.p.calibrationTick();
  CHECK(w.st().deviceCalibration()->liveGateFloorDbfs == Approx(*learned + 0.5).margin(1e-9));
  // Nothing about it is in the preset.
  CHECK(presetToStateJson(w.h.p.currentPreset()).find("liveGateFloor") == std::string::npos);
}

// ---- v0.8 I3: the input-level drift check ---------------------------------------------------------------------------------------
namespace {

// A played DI: 0.4 s notes (300 Hz, peak between -30 and -18 dBFS) every 0.7 s with -75 dBFS noise between; `gainDb` is the interface knob.
std::vector<float> playedDi(double seconds, double gainDb, unsigned seed) {
  const auto n = static_cast<std::size_t>(seconds * kRate);
  std::vector<float> x = noise(n, seed + 100, static_cast<float>(std::pow(10.0, -75.0 / 20.0)));
  std::mt19937 g(seed);
  std::uniform_real_distribution<double> lvl(-30.0, -18.0);
  const auto noteLen = static_cast<std::size_t>(0.4 * kRate), period = static_cast<std::size_t>(0.7 * kRate), ramp = static_cast<std::size_t>(0.005 * kRate);
  for (std::size_t s = 0; s + noteLen <= n; s += period) {
    const double amp = std::pow(10.0, lvl(g) / 20.0);
    for (std::size_t i = 0; i < noteLen; ++i) {
      const double env = std::min({1.0, static_cast<double>(i) / static_cast<double>(ramp), static_cast<double>(noteLen - i) / static_cast<double>(ramp)});
      x[s + i] = static_cast<float>(amp * env * std::sin(2.0 * 3.14159265358979323846 * 300.0 * static_cast<double>(i) / kRate));
    }
  }
  const auto k = static_cast<float>(std::pow(10.0, gainDb / 20.0));
  for (auto& v : x) v *= k;
  return x;
}

// `gateDb` nullopt: no gate in the preset.
json driftRig(std::optional<double> gateDb = -55.0, const std::string& name = "drift") {
  json j = calRig({nb("a1", "linear_identity.nam")}, name, false, -50.0, /*inputDb=*/0.0);
  if (gateDb) j["gate"] = {{"enabled", true}, {"thresholdDb", *gateDb}, {"hysteresisDb", 6.0}};
  return j;
}

// A 50 ms window is "played" when its peak is at least 12 dB above the DI's own noise floor (the tap; the preset's gate plays no part).
// With 0.4 s notes every 0.7 s that is 8 of every 14 windows = 57.1% of the audio, so kMinPlayedPerWall = 0.55 holds by construction.
// These tests therefore reckon in played time: wall(playedS) is the audio length that holds AT LEAST that much played time.
constexpr double kMinPlayedPerWall = 0.55;
double wall(double playedS) { return playedS / kMinPlayedPerWall; }

// Plays `seconds` of the DI through the processor in 0.1 s blocks and runs the 10 Hz tick after each, like the plugin timer does. The DI goes
// on BOTH channels: processBlock takes the mean of the two inputs, and an unset channel 1 would hold the previous block's output.
void playTicking(World& w, double seconds, double gainDb, unsigned seed) {
  const auto x = playedDi(seconds, gainDb, seed);
  std::vector<float> y(4800), y2(4800);
  for (std::size_t pos = 0; pos + 4800 <= x.size(); pos += 4800) {
    w.h.process(x.data() + pos, y.data(), 4800, x.data() + pos, y2.data());
    w.h.p.calibrationTick();
  }
}

// Everything that sets a gain: the host parameters, the preset state and the level-match trim.
struct GainSnapshot {
  std::vector<float> params;
  std::string preset;
  double trimDb = 0.0;
  bool operator==(const GainSnapshot&) const = default;
};
GainSnapshot snapshotOf(World& w) {
  GainSnapshot s;
  for (auto* prm : w.h.p.getParameters()) s.params.push_back(prm->getValue());
  s.preset = presetToStateJson(w.h.p.currentPreset());
  s.trimDb = w.h.p.status().trimDb;
  return s;
}

World& driftWorld(World& w, bool calibrated = true) {
  REQUIRE(w.st().setDeviceCalibration(recordOf("scarlett-4i4-3g")).ok);
  if (calibrated) REQUIRE(w.st().setCalibratedInputLevels(true).ok);
  w.h.load(writeJson(w.tmp.dir, "drift", driftRig()));
  w.tick();  // rebuild with the calibration
  return w;
}

}  // namespace

TEST_CASE("drift check: with calibration off nothing runs: no statistic, no baseline, no notice", "[devicecal][drift][plugin]") {
  World w;
  driftWorld(w, /*calibrated=*/false);
  playTicking(w, wall(70.0), 0.0, 1);
  CHECK(w.h.p.driftTracker().learning());
  CHECK(w.h.p.driftTracker().learnedS() == 0.0);
  CHECK_FALSE(w.h.p.driftNotice().active);
  CHECK_FALSE(w.st().deviceCalibration()->driftBaselineDbfs.has_value());
  CHECK(w.h.allocs == 0);
}

TEST_CASE("drift check: no device record means no statistic either", "[devicecal][drift][plugin]") {
  World w;
  REQUIRE(w.st().setCalibratedInputLevels(true).ok);
  w.h.load(writeJson(w.tmp.dir, "drift", driftRig()));
  w.tick();
  playTicking(w, wall(70.0), 0.0, 1);
  CHECK(w.h.p.driftTracker().learnedS() == 0.0);
  CHECK_FALSE(w.h.p.driftNotice().active);
}

TEST_CASE("drift check: learns a baseline into the device record, raises on a sustained +6 dB, never touches a gain", "[devicecal][drift][plugin]") {
  World w;
  driftWorld(w);
  // The first >= 60 s of PLAYED audio set the baseline, stored in the device record only. 50 s of audio holds at most 33 s of played time.
  playTicking(w, 50.0, 0.0, 1);
  CHECK(w.h.p.driftTracker().learnedS() > 10.0);  // windows do arrive from the live engine
  CHECK_FALSE(w.st().deviceCalibration()->driftBaselineDbfs.has_value());
  playTicking(w, wall(70.0) - 50.0, 0.0, 2);
  const auto baseline = w.st().deviceCalibration()->driftBaselineDbfs;
  REQUIRE(baseline.has_value());
  CHECK(*baseline > -31.0);
  CHECK(*baseline < -17.0);
  CHECK_FALSE(w.h.p.driftNotice().active);
  juce::MemoryBlock mb;
  w.h.p.getStateInformation(mb);
  CHECK(mb.toString().toStdString().find("driftBaseline") == std::string::npos);
  CHECK(presetToStateJson(w.h.p.currentPreset()).find("driftBaseline") == std::string::npos);

  const GainSnapshot before = snapshotOf(w);
  // +6 dB for 20 s of played time does not raise it, and neither does the return to normal.
  playTicking(w, 20.0 / kMinPlayedPerWall, 6.0, 3);  // 36 s of audio: 20 to 24 s played, short of the 30 s sustain
  CHECK_FALSE(w.h.p.driftNotice().active);
  playTicking(w, wall(40.0), 0.0, 4);
  CHECK_FALSE(w.h.p.driftNotice().active);
  // A true +6 dB sustained does, within 60 s of played time.
  playTicking(w, wall(60.0), 6.0, 5);
  const auto n = w.h.p.driftNotice();
  REQUIRE(n.active);
  CHECK(n.hotter);
  CHECK(n.db >= 5);
  CHECK(n.db <= 8);
  CHECK(w.st().deviceCalibration()->driftBaselineDbfs == baseline);  // the baseline did not drift with the signal
  // No gain changed anywhere while it fired.
  CHECK(snapshotOf(w) == before);
  // [Ignore] silences it.
  w.h.p.ignoreDrift();
  CHECK_FALSE(w.h.p.driftNotice().active);
  playTicking(w, wall(40.0), 6.0, 6);
  CHECK_FALSE(w.h.p.driftNotice().active);
  CHECK(snapshotOf(w) == before);
  CHECK(w.h.allocs == 0);
  CHECK(w.h.locks == 0);

  // Re-picking the device clears the baseline (and so the tracker learns again).
  REQUIRE(w.st().setDeviceCalibration(recordOf("scarlett-4i4-3g")).ok);
  w.h.p.calibrationTick();
  CHECK_FALSE(w.st().deviceCalibration()->driftBaselineDbfs.has_value());
  CHECK(w.h.p.driftTracker().learning());
  CHECK_FALSE(w.h.p.driftNotice().active);
}

TEST_CASE("drift check: a true -6 dB is raised too, as quieter, and changes no gain", "[devicecal][drift][plugin]") {
  World w;
  driftWorld(w);
  playTicking(w, wall(70.0), 0.0, 1);
  REQUIRE(w.st().deviceCalibration()->driftBaselineDbfs.has_value());
  const GainSnapshot before = snapshotOf(w);
  playTicking(w, wall(60.0), -6.0, 2);
  const auto n = w.h.p.driftNotice();
  REQUIRE(n.active);
  CHECK_FALSE(n.hotter);
  CHECK(n.db >= 5);
  CHECK(n.db <= 8);
  CHECK(drift::driftNoticeText(n).find("quieter") != std::string::npos);
  CHECK(snapshotOf(w) == before);
}

TEST_CASE("drift check: playing dynamics within +-4 dB over minutes never raise it", "[devicecal][drift][plugin]") {
  World w;
  driftWorld(w);
  playTicking(w, wall(70.0), 0.0, 1);
  REQUIRE(w.st().deviceCalibration()->driftBaselineDbfs.has_value());
  bool ever = false;
  double worst = 0.0;
  for (int rep = 0; rep < 16; ++rep) {  // alternating loud (+4 dB) and soft (-4 dB) passages of 15 s: 4 minutes
    const double g = rep % 2 ? 4.0 : -4.0;
    const auto x = playedDi(15.0, g, static_cast<unsigned>(10 + rep));
    std::vector<float> y(4800), y2(4800);
    for (std::size_t pos = 0; pos + 4800 <= x.size(); pos += 4800) {
      w.h.process(x.data() + pos, y.data(), 4800, x.data() + pos, y2.data());
      w.h.p.calibrationTick();
      ever = ever || w.h.p.driftNotice().active;
      worst = std::max(worst, std::fabs(w.h.p.driftTracker().driftDb()));
    }
  }
  INFO("largest |drift| " << worst << " dB");
  CHECK_FALSE(ever);
}

TEST_CASE("drift check: switching presets with different gate thresholds, or no gate, raises no notice and keeps the p95 within 0.5 dB", "[devicecal][drift][plugin]") {
  World w;
  driftWorld(w);
  playTicking(w, wall(70.0), 0.0, 1);  // the baseline, on the first preset (gate -55)
  REQUIRE(w.st().deviceCalibration()->driftBaselineDbfs.has_value());
  bool ever = false;
  // The same input (same seed and length, so the last 15 s the tracker sees are identical) through four presets.
  const auto phase = [&](const char* name, std::optional<double> gateDb) {
    w.h.load(writeJson(w.tmp.dir, name, driftRig(gateDb, name)));
    w.tick();
    const auto x = playedDi(wall(40.0), 0.0, 9);
    std::vector<float> y(4800), y2(4800);
    for (std::size_t pos = 0; pos + 4800 <= x.size(); pos += 4800) {
      w.h.process(x.data() + pos, y.data(), 4800, x.data() + pos, y2.data());
      w.h.p.calibrationTick();
      ever = ever || w.h.p.driftNotice().active;
    }
    const auto p95 = w.h.p.driftTracker().rollingP95Db();
    REQUIRE(p95.has_value());
    return *p95;
  };
  const double ref = phase("g55", -55.0);
  CHECK(std::fabs(phase("g25", -25.0) - ref) <= 0.5);  // a gate that closes on most of the playing
  CHECK(std::fabs(phase("gOff", std::nullopt) - ref) <= 0.5);
  CHECK(std::fabs(phase("g45", -45.0) - ref) <= 0.5);
  CHECK_FALSE(ever);
  CHECK(w.h.p.driftTracker().baselineDb() == w.st().deviceCalibration()->driftBaselineDbfs);
}
