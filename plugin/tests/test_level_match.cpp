// v0.3 Task B in the plugin: LEVEL MATCH (a Settings store flag), the background trim, the audio thread's trim gain, A/B at
// matched loudness, the capture-swap make-up on the level worker, and the NAM export staying un-trimmed.
// Headless (no window): the processor is driven like a host.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <vector>

#include "MatchGlue.h"
#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "alloc_guard.h"
#include "lock_guard.h"
#include "presets/AbCompare.h"
#include "sawblade/auto_trim.h"
#include "sawblade/loudness.h"
#include "sawblade/reference_di.h"
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

// A two-path identity preset with absolute paths: `gainDb` on the input (a plain level change), `modelB` on path B.
json rigJson(const std::string& name, double inputGainDb = 6.0, const std::string& modelB = "linear_identity.nam") {
  const auto block = [&](const std::string& id, const std::string& file) {
    return json{{"id", id}, {"type", "nam"}, {"slot", "amp"}, {"model", {{"file", (kFixtures / "nam" / file).string()}}}};
  };
  return {{"schema", "sawblade.preset"}, {"version", 3}, {"name", name},
          {"input", {{"gainDb", inputGainDb}}},
          {"paths", {{"a", {{"blocks", json::array({block("a1", "linear_identity.nam")})}}},
                     {"b", {{"blocks", json::array({block("b1", modelB)})}}}}},
          {"align", {{"mode", "off"}}},
          {"blend", 0.5},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", (kFixtures / "ir" / "impulse.wav").string()}}}}}};
}

fs::path writeJson(const fs::path& dir, const std::string& name, const json& j) {
  const fs::path p = dir / (name + ".json");
  std::ofstream(p) << j.dump(2);
  return p;
}

double lufsOf(const std::vector<float>& x) {
  const std::vector<float> zeros(x.size(), 0.0f);
  const auto l = integratedLoudnessLufs(x.data(), zeros.data(), static_cast<std::int64_t>(x.size()), kRate);
  REQUIRE(l.has_value());
  return *l;
}

// Plays the reference DI through the host (after `settle` seconds of the DI's own noise-free silence so ramps and cross-fades
// are over) and returns the loudness of the output, latency removed.
double playReference(Host& h, double settleSeconds = 1.5) {
  std::vector<float> silence(static_cast<std::size_t>(settleSeconds * kRate), 0.0f), sink;
  h.run(silence, sink, {512});
  const std::vector<float>& ref = referenceDi();
  std::vector<float> x = ref, y;
  x.insert(x.end(), static_cast<std::size_t>(h.p.getLatencySamples()), 0.0f);
  h.run(x, y, {512});
  y.erase(y.begin(), y.begin() + h.p.getLatencySamples());
  REQUIRE(y.size() == ref.size());
  return lufsOf(y);
}

struct World {
  TempDir tmp;
  SettingsEnv env;
  Host h;
  explicit World(const char* settingsJson = "{}") : env(settingsJson), h(kRate, 512) { h.p.setLevelDebounceMs(0); }
  void load(const json& j, const std::string& name = "p") {
    h.load(writeJson(tmp.dir, name, j));
    h.p.levelTick();
  }
};

}  // namespace

TEST_CASE("level match: the setting is on by default, lives in the Settings store and is not part of the preset", "[levelmatch][plugin]") {
  SettingsEnv env("{}");
  auto& s = settings::Settings::shared();
  CHECK(s.levelMatch());
  const fs::path file = s.file();
  CHECK(s.setLevelMatch(false).ok);
  CHECK_FALSE(s.levelMatch());
  {
    settings::Settings again(file);
    REQUIRE(again.load().empty());
    CHECK_FALSE(again.levelMatch());
    CHECK(again.setLevelMatch(true).ok);
  }
  CHECK(s.setLevelMatch(true).ok);
  // Toggling it never changes the preset.
  TempDir tmp;
  Host h(kRate, 512);
  h.load(writeJson(tmp.dir, "p", rigJson("p")));
  s.setLevelMatch(true);
  h.p.levelTick();
  const std::string on = presetToStateJson(h.p.currentPreset());
  s.setLevelMatch(false);
  h.p.levelTick();
  CHECK(presetToStateJson(h.p.currentPreset()) == on);
  CHECK(on.find("levelMatchSetting") == std::string::npos);
  s.setLevelMatch(true);
}

TEST_CASE("level match: a loaded preset plays at 0 dB trim until the background measure is done, then at -18 LUFS", "[levelmatch][plugin]") {
  World w;
  w.load(rigJson("quiet", /*inputGainDb=*/3.0));
  auto st = w.h.p.status();
  CHECK(st.levelMatchOn);
  CHECK(st.levelPending);  // the chip says "LEVEL ..."
  CHECK(st.trimDb == 0.0);
  const Preset parsed = loadPresetFile(w.tmp.dir / "p.json");
  const double raw = *measureReferenceLufs(parsed);
  REQUIRE(w.h.p.waitForLevelWork());
  st = w.h.p.status();
  CHECK_FALSE(st.levelPending);
  CHECK_FALSE(st.levelFailed);
  const auto expected = computeAutoTrim(parsed);
  REQUIRE(expected.has_value());
  CHECK(st.trimDb == Approx(expected->trimDb).margin(1e-6));
  CHECK(std::fabs(raw - kAutoTrimTargetLufs) > 1.0);  // not vacuous
  CHECK(playReference(w.h) == Approx(kAutoTrimTargetLufs).margin(0.5));
  CHECK(w.h.allocs == 0);
  if (LockGuard::enabled()) CHECK(w.h.locks == 0);
  // The measured trim is carried by the preset (saved state, A / B slots) so a reload needs no measurement.
  const Preset cur = w.h.p.currentPreset();
  CHECK(cur.autoTrim.hash == autoTrimHash(cur));
  CHECK(cur.autoTrim.db == Approx(expected->trimDb).margin(1e-6));
}

TEST_CASE("level match: a fresh stored trim applies at once and nothing is measured", "[levelmatch][plugin]") {
  World w;
  Preset p = parsePreset(rigJson("stored", 6.0), kFixtures);
  REQUIRE(ensureAutoTrim(p));
  const double trim = p.autoTrim.db;
  w.h.load(writeJson(w.tmp.dir, "stored", json::parse(presetToStateJson(p))));
  w.h.p.levelTick();
  const auto st = w.h.p.status();
  CHECK_FALSE(st.levelPending);
  CHECK(st.trimDb == Approx(trim).margin(1e-9));
  CHECK(w.h.p.waitForLevelWork());
  CHECK(w.h.p.levelWorker().trimJobsRun() == 0);
  // Right from the first block: no ramp up from 0 dB.
  CHECK(playReference(w.h, 0.2) == Approx(kAutoTrimTargetLufs).margin(0.5));
}

TEST_CASE("level match: a stale stored trim is ignored and measured again", "[levelmatch][plugin]") {
  World w;
  Preset p = parsePreset(rigJson("stale", 8.0), kFixtures);
  REQUIRE(ensureAutoTrim(p));
  json j = json::parse(presetToStateJson(p));
  j["input"]["gainDb"] = 20.0;  // the preset changed after the trim was measured
  w.h.load(writeJson(w.tmp.dir, "stale", j));
  w.h.p.levelTick();
  CHECK(w.h.p.status().levelPending);
  CHECK(w.h.p.status().trimDb == 0.0);
  REQUIRE(w.h.p.waitForLevelWork());
  CHECK(w.h.p.levelWorker().trimJobsRun() == 1);
  CHECK(w.h.p.status().trimDb == Approx(p.autoTrim.db - 12.0).margin(0.2));
}

TEST_CASE("level match: with LEVEL MATCH off no trim is applied and nothing is measured", "[levelmatch][plugin]") {
  World w(R"({"levelMatch": false})");
  w.load(rigJson("off", 3.0));
  CHECK_FALSE(w.h.p.status().levelMatchOn);
  CHECK_FALSE(w.h.p.status().levelPending);
  CHECK(w.h.p.waitForLevelWork());
  CHECK(w.h.p.levelWorker().trimJobsRun() == 0);
  const double raw = *measureReferenceLufs(loadPresetFile(w.tmp.dir / "p.json"));
  CHECK(playReference(w.h) == Approx(raw).margin(0.05));  // today's levels
  // Switched on while running: measured, then applied with a ramp.
  settings::Settings::shared().setLevelMatch(true);
  w.h.p.levelTick();
  REQUIRE(w.h.p.waitForLevelWork());
  CHECK(w.h.p.levelWorker().trimJobsRun() == 1);
  CHECK(playReference(w.h) == Approx(kAutoTrimTargetLufs).margin(0.5));
  // ... and off again: back to the raw level.
  settings::Settings::shared().setLevelMatch(false);
  w.h.p.levelTick();
  CHECK(playReference(w.h) == Approx(raw).margin(0.05));
}

TEST_CASE("level match: a rig change is re-measured once, debounced; the OUTPUT knob is not a rig change", "[levelmatch][plugin]") {
  World w;
  w.load(rigJson("rig"));
  REQUIRE(w.h.p.waitForLevelWork());
  const double trim0 = w.h.p.status().trimDb;
  const auto jobs0 = w.h.p.levelWorker().trimJobsRun();
  CHECK(jobs0 == 1);
  // The OUTPUT knob moves the level on top of the match: no new measurement, the trim stays.
  w.h.setParam(kOutputGain, 6.0);
  w.h.p.levelTick();
  CHECK(w.h.p.waitForLevelWork());
  CHECK(w.h.p.levelWorker().trimJobsRun() == jobs0);
  CHECK(w.h.p.status().trimDb == trim0);
  CHECK(playReference(w.h) == Approx(kAutoTrimTargetLufs + 6.0).margin(0.5));
  w.h.setParam(kOutputGain, 0.0);
  // INPUT +6 dB is a rig change: three quick edits inside the debounce give one job, and the trim follows.
  w.h.p.setLevelDebounceMs(600000);  // never elapses here: the test releases it explicitly, no wall-clock race
  const double before = w.h.p.status().trimDb;
  for (double v : {8.0, 10.0, 12.0}) {
    w.h.setParam(kInputGain, v);
    w.h.p.levelTick();
  }
  CHECK(w.h.p.status().levelPending);
  CHECK(w.h.p.levelWorker().trimJobsRun() == jobs0);  // still waiting for the debounce
  CHECK(w.h.p.status().trimDb == before);              // the previous trim keeps playing meanwhile: no jump to 0
  w.h.p.setLevelDebounceMs(0);                         // the debounce is over
  REQUIRE(w.h.p.waitForLevelWork());
  CHECK(w.h.p.levelWorker().trimJobsRun() == jobs0 + 1);
  CHECK(w.h.p.status().trimDb == Approx(before - 6.0).margin(0.3));
  CHECK(playReference(w.h) == Approx(kAutoTrimTargetLufs).margin(0.5));
}

TEST_CASE("level match: the audio thread allocates nothing and takes no lock while the trim changes", "[levelmatch][plugin][rt]") {
  World w;
  w.load(rigJson("rt"));
  const std::vector<float> x = noise(24000, 7, 0.1f);
  std::vector<float> y;
  w.h.run(x, y, {128});
  settings::Settings::shared().setLevelMatch(false);  // the trim goes 0 -> ... -> 0 under the audio thread
  w.h.p.levelTick();
  w.h.run(x, y, {128});
  settings::Settings::shared().setLevelMatch(true);
  w.h.p.levelTick();
  REQUIRE(w.h.p.waitForLevelWork());
  w.h.run(x, y, {128});
  CHECK(w.h.allocs == 0);
  if (LockGuard::enabled()) CHECK(w.h.locks == 0);
  CHECK_FALSE(w.h.nonFinite);
}

TEST_CASE("level match: A/B plays both sides at their trims, within 0.5 LU of each other", "[levelmatch][plugin][ab]") {
  World w;
  w.load(rigJson("A", 6.0), "a");
  REQUIRE(w.h.p.waitForLevelWork());
  AbCompare ab(w.h.p);
  const double a0 = playReference(w.h);
  ab.toggle();  // B starts as a copy of A
  REQUIRE(w.h.p.waitForLoader());
  // B: a different rig, 20 dB hotter and another model on path B
  w.h.p.loadPreset(parsePreset(rigJson("B", 20.0, "linear_05_025.nam"), kFixtures));
  REQUIRE(w.h.p.waitForLoader());
  w.h.p.levelTick();
  REQUIRE(w.h.p.waitForLevelWork());
  const double raw = *measureReferenceLufs(w.h.p.currentPreset());
  CHECK(std::fabs(raw - a0) > 3.0);  // without the match B is far louder
  const double b = playReference(w.h);
  ab.toggle();  // back to A
  REQUIRE(w.h.p.waitForLoader());
  w.h.p.levelTick();
  REQUIRE(w.h.p.waitForLevelWork());
  const double a = playReference(w.h);
  ab.toggle();  // and to B again: its trim travelled with the slot
  REQUIRE(w.h.p.waitForLoader());
  w.h.p.levelTick();
  REQUIRE(w.h.p.waitForLevelWork());
  const double b2 = playReference(w.h);
  INFO("A " << a << " B " << b << " A first " << a0 << " B again " << b2);
  CHECK(a == Approx(kAutoTrimTargetLufs).margin(0.5));
  CHECK(b == Approx(kAutoTrimTargetLufs).margin(0.5));
  CHECK(b2 == Approx(kAutoTrimTargetLufs).margin(0.5));
  CHECK(std::fabs(a - b) <= 0.5);
  CHECK(std::fabs(a - b2) <= 0.5);
  CHECK(w.h.p.levelWorker().trimJobsRun() <= 3);  // A, B, and at most one re-measure: the slots carried their trims
}

TEST_CASE("level match: the capture-swap make-up is computed on the level worker thread", "[levelmatch][plugin][swap]") {
  World w;
  const Preset before = parsePreset(rigJson("before"), kFixtures);
  const Preset after = parsePreset(rigJson("after", 0.0, "linear_05_025.nam"), kFixtures);
  std::promise<LevelWorker::MakeupResult> pr;
  std::thread::id workerThread;
  w.h.p.computeSlotMakeup(before, after, 1, [&](const LevelWorker::MakeupResult& r) {
    workerThread = std::this_thread::get_id();
    pr.set_value(r);
  });
  auto f = pr.get_future();
  REQUIRE(f.wait_for(std::chrono::seconds(60)) == std::future_status::ready);
  const auto r = f.get();
  REQUIRE(r.makeupDb.has_value());
  CHECK(workerThread != std::this_thread::get_id());
  CHECK(*r.makeupDb == Approx(*slotMakeupDb(before, after, 1)).margin(1e-9));
  const Preset fixed = withSlotMakeup(after, 1, 0, *r.makeupDb);
  CHECK(*measurePathLufs(fixed, 1) == Approx(*measurePathLufs(before, 1)).margin(0.5));
  // A disabled path reports why instead of throwing.
  Preset off = before;
  off.b.enabled = false;
  std::promise<LevelWorker::MakeupResult> pr2;
  w.h.p.computeSlotMakeup(off, after, 1, [&](const LevelWorker::MakeupResult& x) { pr2.set_value(x); });
  auto f2 = pr2.get_future();
  REQUIRE(f2.wait_for(std::chrono::seconds(60)) == std::future_status::ready);
  const auto r2 = f2.get();
  CHECK_FALSE(r2.makeupDb.has_value());
  CHECK_FALSE(r2.error.empty());
}

TEST_CASE("level match: the NAM export source never carries the trim; a swap make-up stays", "[levelmatch][plugin][export]") {
  World w;
  w.load(rigJson("export"));
  REQUIRE(w.h.p.waitForLevelWork());
  REQUIRE(w.h.p.currentPreset().autoTrim.db != 0.0);
  const ExportSource a = prepareExportSource(w.h.p, false, /*write=*/true);
  REQUIRE(a.ok);
  const json j = json::parse(std::ifstream(a.file));
  CHECK_FALSE(j["output"].contains("autoTrimDb"));
  CHECK_FALSE(j["output"].contains("autoTrimHash"));
  // The same rig always has the same key whether or not a trim has been measured.
  Preset fresh = parsePreset(rigJson("export"), kFixtures);
  Preset trimmed = fresh;
  REQUIRE(ensureAutoTrim(trimmed));
  CHECK(presetToStateJson(withSlotMakeup(fresh, 0, 0, 0.0)).size() > 0);
  const Preset swapped = withSlotMakeup(w.h.p.currentPreset(), 0, 0, 3.5);
  w.h.p.loadPreset(swapped);
  REQUIRE(w.h.p.waitForLoader());
  const ExportSource b = prepareExportSource(w.h.p, false, /*write=*/true);
  REQUIRE(b.ok);
  const json jb = json::parse(std::ifstream(b.file));
  CHECK(jb["paths"]["a"]["blocks"][0]["makeupDb"] == 3.5);
  CHECK_FALSE(jb["output"].contains("autoTrimDb"));
}

TEST_CASE("level match: OUTPUT is a persistent offset from -18 LUFS, not part of the trim or its hash", "[levelmatch][plugin]") {
  World w;
  json j = rigJson("offset");
  j["output"] = {{"gainDb", -6.0}};
  Preset p = parsePreset(j, kFixtures);
  Preset p0 = parsePreset(rigJson("offset"), kFixtures);
  REQUIRE(ensureAutoTrim(p));
  REQUIRE(ensureAutoTrim(p0));
  CHECK(p.autoTrim.db == Approx(p0.autoTrim.db).margin(1e-9));
  CHECK(p.autoTrim.hash == p0.autoTrim.hash);
  // Saved with OUTPUT at -6 and its (fresh) trim: reloads at -6 relative to the target, nothing re-measured.
  w.h.load(writeJson(w.tmp.dir, "saved", json::parse(presetToStateJson(p))));
  w.h.p.levelTick();
  CHECK_FALSE(w.h.p.status().levelPending);
  CHECK(w.h.p.waitForLevelWork());
  CHECK(w.h.p.levelWorker().trimJobsRun() == 0);
  CHECK(playReference(w.h, 0.2) == Approx(kAutoTrimTargetLufs - 6.0).margin(0.5));
  // Moving the knob from there is still relative: +6 dB lands on the target.
  w.h.setParam(kOutputGain, 0.0);
  CHECK(playReference(w.h) == Approx(kAutoTrimTargetLufs).margin(0.5));
}

TEST_CASE("level match: an empty rig (Init) gets trim 0", "[levelmatch][plugin]") {
  World w;
  w.h.p.loadPreset(makeInitPreset());
  REQUIRE(w.h.p.waitForLoader());
  w.h.p.levelTick();
  REQUIRE(w.h.p.waitForLevelWork());
  CHECK(w.h.p.status().trimDb == 0.0);
  CHECK_FALSE(w.h.p.status().levelPending);
  CHECK_FALSE(w.h.p.status().levelFailed);
}

// ---- Task B follow-ups (v0.3 Task C step 0) -------------------------------------------------------------------------------
TEST_CASE("level match: a capture swap keeps the old trim until the new measurement lands (no dip to 0)", "[levelmatch][plugin][swap]") {
  World w;
  w.load(rigJson("a", 6.0));
  REQUIRE(w.h.p.waitForLevelWork());
  const double t0 = w.h.p.status().trimDb;
  REQUIRE(std::fabs(t0) > 1.0);
  // A rig the stored trim does not cover (new hash), loaded the way the browser's swap does: with the old trim.
  w.h.p.loadPreset(parsePreset(rigJson("b", 6.0, "linear_05_025.nam"), kFixtures), false, t0);
  REQUIRE(w.h.p.waitForLoader());
  CHECK(w.h.p.status().trimDb == t0);  // immediately after: no jump
  CHECK(w.h.p.status().levelPending);
  w.h.p.setLevelDebounceMs(60000);     // the measurement stays pending
  w.h.p.levelTick();
  CHECK(w.h.p.status().trimDb == t0);
  // A plain user load of a new rig still starts at 0.
  w.h.p.loadPreset(parsePreset(rigJson("c", 9.0), kFixtures));
  REQUIRE(w.h.p.waitForLoader());
  CHECK(w.h.p.status().trimDb == 0.0);
}

TEST_CASE("level match: levelTick hashes the rig once, not on every tick", "[levelmatch][plugin]") {
  World w;
  w.load(rigJson("cache", 6.0));
  REQUIRE(w.h.p.waitForLevelWork());
  const auto n = w.h.p.levelHashComputes();
  for (int i = 0; i < 20; ++i) w.h.p.levelTick();
  CHECK(w.h.p.levelHashComputes() == n);
  w.h.setParam(kOutputGain, -3.0);  // a parameter change is noticed
  w.h.p.levelTick();
  CHECK(w.h.p.levelHashComputes() == n + 1);
  w.h.p.applyLiveEdit([](Preset& p) { p.inputGainDb += 1.0; });  // so is a live edit
  w.h.p.levelTick();
  CHECK(w.h.p.levelHashComputes() == n + 2);
}

TEST_CASE("level match: a failed measurement is retried after a user load and after a LEVEL MATCH toggle", "[levelmatch][plugin]") {
  World w;
  json j = rigJson("silent", 6.0);
  j["paths"]["a"]["enabled"] = false;
  j["blend"] = 0.0;  // only the disabled path: silent
  w.load(j);
  REQUIRE(w.h.p.waitForLevelWork());
  CHECK(w.h.p.status().levelFailed);
  CHECK(w.h.p.levelWorker().trimJobsRun() == 1);
  w.h.p.levelTick();
  REQUIRE(w.h.p.waitForLevelWork());
  CHECK(w.h.p.levelWorker().trimJobsRun() == 1);  // remembered while nothing changed
  settings::Settings::shared().setLevelMatch(false);
  w.h.p.levelTick();
  settings::Settings::shared().setLevelMatch(true);
  w.h.p.levelTick();
  REQUIRE(w.h.p.waitForLevelWork());
  CHECK(w.h.p.levelWorker().trimJobsRun() == 2);  // the toggle retried it
  w.load(j, "again");                              // a user load of the same rig retries too
  REQUIRE(w.h.p.waitForLevelWork());
  CHECK(w.h.p.levelWorker().trimJobsRun() == 3);
}

TEST_CASE("level match: destroying the level worker does not wait for the queued work", "[levelmatch][plugin]") {
  const auto t0 = std::chrono::steady_clock::now();
  {
    LevelWorker lw;
    const Preset p = parsePreset(rigJson("x"), kFixtures);
    for (int i = 0; i < 4; ++i) lw.submitMakeup(p, p, 0, [](const LevelWorker::MakeupResult&) {});
  }
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));
}
