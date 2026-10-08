// v0.8 I4b part 4: "Calibrate all user presets..." (core/include/sawblade/preset_calibrate.h). Synthetic presets on the linear fixtures.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "sawblade/preset.h"
#include "sawblade/preset_calibrate.h"
#include "test_util.h"

using namespace sawblade;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kNam = fs::path(SAWBLADE_FIXTURES_DIR) / "nam";

struct TempDir {
  fs::path dir;
  TempDir() {
    static int counter = 0;
    dir = fs::temp_directory_path() / ("sawblade_bulkcal_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// version <= 4 or no `calibration` member = legacy; "calibrated" needs version 5.
json mk(const std::string& name, int version, const std::string& mode) {
  const auto blk = [](const char* id) { return json{{"id", id}, {"type", "nam"}, {"slot", "amp"}, {"model", {{"file", (kNam / "cal_amp_hi.nam").string()}}}}; };
  json j = {{"schema", "sawblade.preset"}, {"version", version}, {"name", name}, {"category", "Mine"}, {"notes", "keep me"},
            {"input", {{"gainDb", 3.5}}},
            {"paths", {{"a", {{"blocks", json::array({blk("a1")})}}}, {"b", {{"enabled", false}, {"blocks", json::array({blk("b1")})}}}}},
            {"align", {{"mode", "off"}}}, {"blend", 0.0},
            {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}},
            {"output", {{"gainDb", -2.0}, {"autoTrimDb", -1.5}, {"autoTrimHash", "abcd"}}}};
  if (!mode.empty()) j["calibration"] = {{"mode", mode}};
  return j;
}
void put(const fs::path& f, const json& j) { std::ofstream(f) << j.dump(2) << '\n'; }

}  // namespace

TEST_CASE("bulk calibrate: counts legacy user presets and ignores calibrated, resolved and foreign files", "[i4b][bulk]") {
  TempDir t;
  put(t.dir / "a_legacy_v4.json", mk("a", 4, ""));
  put(t.dir / "b_legacy_v5.json", mk("b", 5, "legacy"));
  put(t.dir / "c_calibrated.json", mk("c", 5, "calibrated"));
  put(t.dir / "d.resolved.json", mk("d", 4, ""));
  std::ofstream(t.dir / "e_broken.json") << "{ not json";
  std::ofstream(t.dir / "notes.txt") << "hello";
  fs::create_directories(t.dir / "sub");
  put(t.dir / "sub" / "f_legacy.json", mk("f", 4, ""));  // only the folder itself, like the preset library
  const auto legacy = legacyPresetFiles(t.dir);
  REQUIRE(legacy.size() == 2);
  CHECK(legacy[0].filename() == "a_legacy_v4.json");
  CHECK(legacy[1].filename() == "b_legacy_v5.json");
  CHECK(legacyPresetFiles(t.dir / "missing").empty());
}

TEST_CASE("bulk calibrate: legacy files become v5 calibrated, anything else about them is unchanged; calibrated files are byte-identical", "[i4b][bulk]") {
  TempDir t;
  put(t.dir / "a.json", mk("a", 4, ""));
  put(t.dir / "b.json", mk("b", 5, "legacy"));
  put(t.dir / "c.json", mk("c", 5, "calibrated"));
  std::ofstream(t.dir / "z_broken.json") << "{ not json";
  const std::string calBefore = slurp(t.dir / "c.json"), brokenBefore = slurp(t.dir / "z_broken.json");
  const Preset aBefore = loadPresetFile(t.dir / "a.json");
  REQUIRE(aBefore.calibrationMode == CalibrationMode::Legacy);

  std::vector<std::string> progress;
  const BulkCalibrateResult r = calibrateLegacyPresets(t.dir, atomicWriteText, [&](const BulkCalibrateEntry& e) { progress.push_back(e.file.filename().string()); });
  CHECK(r.converted() == 2);
  CHECK(r.count(BulkCalibrateStatus::AlreadyCalibrated) == 1);
  CHECK(r.count(BulkCalibrateStatus::Unreadable) == 1);
  CHECK(r.failed() == 0);
  CHECK(progress.size() == 4);

  for (const char* f : {"a.json", "b.json"}) {
    const Preset p = loadPresetFile(t.dir / f);
    CHECK(p.calibrationMode == CalibrationMode::Calibrated);
    CHECK(json::parse(slurp(t.dir / f))["version"] == 5);
    CHECK(json::parse(slurp(t.dir / f))["calibration"]["mode"] == "calibrated");
  }
  // Nothing but the mode changed.
  Preset aAfter = loadPresetFile(t.dir / "a.json");
  aAfter.calibrationMode = CalibrationMode::Legacy;
  CHECK(aAfter == aBefore);
  CHECK(loadPresetFile(t.dir / "a.json").name == "a");
  CHECK(loadPresetFile(t.dir / "a.json").notes == "keep me");
  CHECK(loadPresetFile(t.dir / "a.json").autoTrim.db == -1.5);  // the legacy stamp stays with the preset
  // Untouched files are byte-identical.
  CHECK(slurp(t.dir / "c.json") == calBefore);
  CHECK(slurp(t.dir / "z_broken.json") == brokenBefore);
  // No temporary file is left behind, and a second run finds nothing to do.
  for (const auto& e : fs::directory_iterator(t.dir)) CHECK(e.path().extension() != ".tmp");
  CHECK(legacyPresetFiles(t.dir).empty());
  const BulkCalibrateResult again = calibrateLegacyPresets(t.dir);
  CHECK(again.converted() == 0);
  CHECK(again.count(BulkCalibrateStatus::AlreadyCalibrated) == 3);
}

TEST_CASE("bulk calibrate: a file that cannot be written is reported and the others still convert", "[i4b][bulk]") {
  TempDir t;
  put(t.dir / "a.json", mk("a", 4, ""));
  put(t.dir / "b.json", mk("b", 4, ""));
  put(t.dir / "c.json", mk("c", 4, ""));
  const std::string bBefore = slurp(t.dir / "b.json");
  // An injected failure on b ...
  const AtomicWriteFn failB = [](const fs::path& f, const std::string& text) -> std::string {
    if (f.filename() == "b.json") return "cannot write " + f.string() + ": disk full";
    return atomicWriteText(f, text);
  };
  const BulkCalibrateResult r = calibrateLegacyPresets(t.dir, failB);
  CHECK(r.converted() == 2);
  REQUIRE(r.failed() == 1);
  const auto bad = std::find_if(r.files.begin(), r.files.end(), [](const BulkCalibrateEntry& e) { return e.status == BulkCalibrateStatus::WriteFailed; });
  REQUIRE(bad != r.files.end());
  CHECK(bad->file.filename() == "b.json");
  CHECK(bad->error.find("disk full") != std::string::npos);
  CHECK(slurp(t.dir / "b.json") == bBefore);  // left as it was
  CHECK(loadPresetFile(t.dir / "a.json").calibrationMode == CalibrationMode::Calibrated);
  CHECK(loadPresetFile(t.dir / "c.json").calibrationMode == CalibrationMode::Calibrated);
  CHECK(legacyPresetFiles(t.dir).size() == 1);

  // ... and the real thing: a directory squatting on the temp name makes the default atomic write fail for that file only (also as root).
  TempDir u;
  put(u.dir / "a.json", mk("a", 4, ""));
  put(u.dir / "b.json", mk("b", 4, ""));
  fs::create_directories(u.dir / "a.json.tmp");
  const BulkCalibrateResult r2 = calibrateLegacyPresets(u.dir);
  CHECK(r2.failed() == 1);
  CHECK(r2.converted() == 1);
  CHECK(loadPresetFile(u.dir / "b.json").calibrationMode == CalibrationMode::Calibrated);
  CHECK(loadPresetFile(u.dir / "a.json").calibrationMode == CalibrationMode::Legacy);
}

TEST_CASE("bulk calibrate: the factory folder is not the caller's business - only the folder it is given is touched", "[i4b][bulk]") {
  TempDir user, factory;
  put(user.dir / "u.json", mk("u", 4, ""));
  put(factory.dir / "f.json", mk("f", 4, ""));
  const std::string factoryBefore = slurp(factory.dir / "f.json");
  calibrateLegacyPresets(user.dir);
  CHECK(slurp(factory.dir / "f.json") == factoryBefore);
  CHECK(loadPresetFile(user.dir / "u.json").calibrationMode == CalibrationMode::Calibrated);
}

TEST_CASE("bulk calibrate: a cab-less rig written as an old (v4) file is legacy and converts", "[i4b][bulk]") {
  // The shape the plugin's tests use for a legacy user preset: the Init rig as an older version wrote it.
  Preset init;
  init.name = "Init";
  init.dynamicsMode = DynamicsMode::Live;
  init.cab.enabled = false;
  init.align.mode = AlignMode::Off;
  init.cab.ir.file = "(none)";  // plugin kNoCaptureFile
  init.cab.ir.resolvedPath = init.cab.ir.file;
  json j = toJson(init);
  j["version"] = 4;
  j.erase("calibration");
  TempDir t;
  put(t.dir / "init.json", j);
  REQUIRE(legacyPresetFiles(t.dir).size() == 1);
  const BulkCalibrateResult r = calibrateLegacyPresets(t.dir);
  CHECK(r.converted() == 1);
  CHECK(loadPresetFile(t.dir / "init.json").calibrationMode == CalibrationMode::Calibrated);
}
