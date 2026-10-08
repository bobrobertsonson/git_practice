// v0.8 I2 Part 2: the device calibration record (docs/PLUGIN.md "Device step"): the interface presets and their figures, the dBu
// validation, the JSON of the record, and the Settings store (default none, round trip through the file, the learned gate floor
// keyed on the record, the beta toggle default off). Headless: temp dirs only. The engine / processor cases are in
// test_device_calibration_engine.cpp.
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "settings/DeviceCalibration.h"
#include "settings/Settings.h"

using namespace sawblade::plugin::settings;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;

namespace {

struct Tmp {
  fs::path dir;
  Tmp() {
    static std::atomic<int> n{0};
    dir = fs::temp_directory_path() / ("sawblade_devcal_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~Tmp() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

}  // namespace

TEST_CASE("device calibration: the interface presets carry the manufacturer's figures at minimum gain, each with its source", "[devicecal][settings]") {
  const auto& t = devicePresets();
  REQUIRE(t.size() == 3);
  const DevicePreset* g3 = findDevicePreset("scarlett-4i4-3g");
  const DevicePreset* g3pad = findDevicePreset("scarlett-4i4-3g-pad");
  const DevicePreset* g4 = findDevicePreset("scarlett-4i4-4g");
  REQUIRE((g3 && g3pad && g4));
  CHECK(g3->dbu == 12.5);   // Scarlett 4i4 3rd gen, Inst, no PAD (the user's interface)
  CHECK_FALSE(g3->pad);
  CHECK(g3pad->dbu == 14.0);  // the same with PAD
  CHECK(g3pad->pad);
  CHECK(g4->dbu == 12.0);   // Scarlett 4i4 4th gen, Inst
  CHECK(findDevicePreset("nope") == nullptr);
  for (const auto& p : t) {
    INFO(p.id);
    CHECK_THAT(std::string(p.label), ContainsSubstring("Scarlett 4i4"));
    CHECK_THAT(std::string(p.source), ContainsSubstring("at minimum gain"));
    CHECK_THAT(std::string(p.source), ContainsSubstring("https://"));
    CHECK(checkDeviceDbu(p.dbu).ok);
    CHECK(checkDeviceDbu(p.dbu).warning.empty());
  }
  const auto r = recordFromPreset(*g3pad, "2026-10-08");
  CHECK(r.dbu == 14.0);
  CHECK(r.method == DeviceMethod::Preset);
  CHECK(r.pad);
  CHECK_FALSE(r.air);  // Air off is part of the assumption
  CHECK(r.gainAtMinimum);
  CHECK(r.model == g3pad->label);
  CHECK(r.date == "2026-10-08");
  CHECK_FALSE(r.liveGateFloorDbfs.has_value());
}

TEST_CASE("device calibration: a typed dBu is valid in [-60, +60] and warns outside the practical [0, +24]", "[devicecal]") {
  for (double v : {-60.0, -0.5, 0.0, 12.0, 24.0, 60.0}) {
    INFO(v);
    CHECK(checkDeviceDbu(v).ok);
  }
  for (double v : {-60.01, -61.0, 60.01, 100.0, std::nan(""), HUGE_VAL}) {
    INFO(v);
    const auto c = checkDeviceDbu(v);
    CHECK_FALSE(c.ok);
    CHECK_FALSE(c.error.empty());
  }
  for (double v : {0.0, 12.5, 24.0}) CHECK(checkDeviceDbu(v).warning.empty());
  for (double v : {-0.1, -30.0, 24.1, 60.0}) {
    INFO(v);
    const auto c = checkDeviceDbu(v);
    CHECK(c.ok);
    CHECK_FALSE(c.warning.empty());
  }
  CHECK(parseDbuText("12.5") == 12.5);
  CHECK(parseDbuText(" +14 ") == 14.0);
  CHECK(parseDbuText("12,5 dBu") == 12.5);
  CHECK(parseDbuText("-3dbu") == -3.0);
  CHECK_FALSE(parseDbuText("").has_value());
  CHECK_FALSE(parseDbuText("abc").has_value());
  CHECK_FALSE(parseDbuText("12 dB gain").has_value());
  CHECK_FALSE(parseDbuText("nan").has_value());
}

TEST_CASE("device calibration: the record round-trips through JSON; a malformed or out-of-range one reads as none", "[devicecal]") {
  DeviceCalibrationRecord r = recordFromValue(13.25, DeviceMethod::Measured, true, true, "2026-10-08");
  r.model = "my interface";
  r.liveGateFloorDbfs = -43.5;
  const auto back = recordFromJson(recordToJson(r));
  REQUIRE(back.has_value());
  CHECK(*back == r);
  const DeviceCalibrationRecord plain = recordFromPreset(*findDevicePreset("scarlett-4i4-4g"), "2026-01-02");
  CHECK(recordFromJson(recordToJson(plain)) == plain);
  CHECK_FALSE(recordToJson(plain).contains("liveGateFloorDbfs"));
  CHECK(std::string(deviceMethodName(DeviceMethod::Preset)) == "preset");
  CHECK_FALSE(recordFromJson(json::array()).has_value());
  CHECK_FALSE(recordFromJson(json::object()).has_value());
  CHECK_FALSE(recordFromJson(json{{"dbu", "12"}}).has_value());
  CHECK_FALSE(recordFromJson(json{{"dbu", 99.0}}).has_value());
  CHECK(recordFromJson(json{{"dbu", 12.0}}).has_value());  // everything else defaults
  CHECK(recordFromJson(json{{"dbu", 12.0}})->method == DeviceMethod::Manual);
  CHECK_FALSE(recordFromJson(json{{"dbu", 12.0}, {"liveGateFloorDbfs", -10.0}})->liveGateFloorDbfs.has_value());  // outside the follower's range
  CHECK_FALSE(recordFromJson(json{{"dbu", 12.0}, {"liveGateFloorDbfs", -120.0}})->liveGateFloorDbfs.has_value());
}

TEST_CASE("device calibration: the date is YYYY-MM-DD", "[devicecal]") {
  const std::string d = todayDate();
  REQUIRE(d.size() == 10);
  CHECK(d[4] == '-');
  CHECK(d[7] == '-');
}

TEST_CASE("device calibration: the Settings store has no record and the beta toggle off by default", "[devicecal][settings]") {
  Tmp t;
  Settings s(t.dir / "settings.json");
  CHECK(s.load().empty());
  CHECK_FALSE(s.deviceCalibration().has_value());
  CHECK_FALSE(s.calibratedInputLevels());
}

TEST_CASE("device calibration: the record and the toggle live in the settings file and survive a reload", "[devicecal][settings]") {
  Tmp t;
  const fs::path file = t.dir / "settings.json";
  DeviceCalibrationRecord r = recordFromPreset(*findDevicePreset("scarlett-4i4-3g"), "2026-10-08");
  {
    Settings s(file);
    REQUIRE(s.load().empty());
    CHECK(s.setDeviceCalibration(r).ok);
    CHECK(s.setCalibratedInputLevels(true).ok);
    CHECK(s.deviceCalibration() == r);
  }
  {
    Settings again(file);
    REQUIRE(again.load().empty());
    REQUIRE(again.deviceCalibration().has_value());
    CHECK(*again.deviceCalibration() == r);
    CHECK(again.calibratedInputLevels());
    CHECK(again.setCalibratedInputLevels(false).ok);
    CHECK(again.setDeviceCalibration(std::nullopt).ok);
  }
  Settings third(file);
  REQUIRE(third.load().empty());
  CHECK_FALSE(third.deviceCalibration().has_value());
  CHECK_FALSE(third.calibratedInputLevels());
  // Other settings are untouched by all of it.
  CHECK(third.levelMatch());
  // The keys are owned by the store: a stale instance does not resurrect a removed record.
  std::ifstream in(file);
  const json doc = json::parse(in);
  CHECK_FALSE(doc.contains("deviceCalibration"));
  CHECK_FALSE(doc.contains("calibratedInputLevels"));
}

TEST_CASE("device calibration: setDeviceCalibration validates the dBu and warns outside the practical range", "[devicecal][settings]") {
  Tmp t;
  Settings s(t.dir / "settings.json");
  REQUIRE(s.load().empty());
  DeviceCalibrationRecord bad = recordFromValue(75.0, DeviceMethod::Manual, false, false, "2026-10-08");
  const Result rb = s.setDeviceCalibration(bad);
  CHECK_FALSE(rb.ok);
  CHECK_FALSE(rb.error.empty());
  CHECK_FALSE(s.deviceCalibration().has_value());
  const Result rw = s.setDeviceCalibration(recordFromValue(30.0, DeviceMethod::Manual, false, false, "2026-10-08"));
  CHECK(rw.ok);
  CHECK_FALSE(rw.warning.empty());
  CHECK(s.deviceCalibration()->dbu == 30.0);
  const Result rn = s.setDeviceCalibration(recordFromValue(-60.0, DeviceMethod::Measured, false, false, "2026-10-08"));
  CHECK(rn.ok);
  CHECK(s.deviceCalibration()->method == DeviceMethod::Measured);
}

TEST_CASE("device calibration: the learned live-gate floor is keyed on the record and stored with it", "[devicecal][settings][gatefloor]") {
  Tmp t;
  const fs::path file = t.dir / "settings.json";
  Settings s(file);
  REQUIRE(s.load().empty());
  // No record, no key: nothing is stored.
  CHECK_FALSE(s.setLiveGateFloor(-43.0).ok);
  CHECK_FALSE(s.deviceCalibration().has_value());
  REQUIRE(s.setDeviceCalibration(recordFromPreset(*findDevicePreset("scarlett-4i4-3g"), "2026-10-08")).ok);
  CHECK_FALSE(s.deviceCalibration()->liveGateFloorDbfs.has_value());
  CHECK(s.setLiveGateFloor(-43.0).ok);
  CHECK(s.deviceCalibration()->liveGateFloorDbfs == -43.0);
  CHECK(s.deviceCalibration()->dbu == 12.5);  // the rest of the record is kept
  // Out of the follower's range [-96, -40]: refused, the stored value stays.
  CHECK_FALSE(s.setLiveGateFloor(-39.0).ok);
  CHECK_FALSE(s.setLiveGateFloor(-97.0).ok);
  CHECK_FALSE(s.setLiveGateFloor(std::nan("")).ok);
  CHECK(s.deviceCalibration()->liveGateFloorDbfs == -43.0);
  {
    Settings again(file);
    REQUIRE(again.load().empty());
    CHECK(again.deviceCalibration()->liveGateFloorDbfs == -43.0);
  }
  // A new device record is a different interface: its learned floor starts empty. Removing the record removes the floor.
  REQUIRE(s.setDeviceCalibration(recordFromValue(12.0, DeviceMethod::Manual, false, false, "2026-10-09")).ok);
  CHECK_FALSE(s.deviceCalibration()->liveGateFloorDbfs.has_value());
  REQUIRE(s.setLiveGateFloor(-50.0).ok);
  REQUIRE(s.setDeviceCalibration(std::nullopt).ok);
  CHECK_FALSE(s.setLiveGateFloor(-50.0).ok);
}

TEST_CASE("device calibration: the engine settings come from the toggle and the record only", "[devicecal]") {
  const auto rec = recordFromPreset(*findDevicePreset("scarlett-4i4-3g-pad"), "2026-10-08");
  DeviceCalibrationRecord withFloor = rec;
  withFloor.liveGateFloorDbfs = -44.0;
  // Toggle off: calibration off, no seed, whatever the record holds (the engine is the pre-I2 engine).
  const auto off = engineCalibrationFor(false, withFloor);
  CHECK_FALSE(off.chain.enabled);
  CHECK_FALSE(off.gateFloorSeedDb.has_value());
  CHECK_FALSE(engineCalibrationFor(false, std::nullopt).chain.enabled);
  // Toggle on, no record: enabled with no device level (the plan assumes +12 and flags it), no seed.
  const auto assumed = engineCalibrationFor(true, std::nullopt);
  CHECK(assumed.chain.enabled);
  CHECK_FALSE(assumed.chain.device.dbu.has_value());
  CHECK_FALSE(assumed.gateFloorSeedDb.has_value());
  // Toggle on with a record: its dBu, and its learned floor as the seed.
  const auto on = engineCalibrationFor(true, withFloor);
  CHECK(on.chain.enabled);
  CHECK(on.chain.device.dbu == 14.0);
  CHECK(on.gateFloorSeedDb == -44.0);
  CHECK_FALSE(engineCalibrationFor(true, rec).gateFloorSeedDb.has_value());  // nothing learned yet: today's -70 seed
  CHECK(uncalibratedNotice() == "Interface not calibrated: assuming +12 dBu");
}

// ---- v0.8 I3: the drift baseline ---------------------------------------------------------------------------------------------------
TEST_CASE("drift baseline: stored in the device record, keyed on it, and cleared by a new record", "[devicecal][settings][drift]") {
  Tmp t;
  const fs::path file = t.dir / "settings.json";
  Settings s(file);
  REQUIRE(s.load().empty());
  CHECK_FALSE(s.setDriftBaseline(-20.0).ok);  // no record, no key
  REQUIRE(s.setDeviceCalibration(recordFromPreset(*findDevicePreset("scarlett-4i4-3g"), "2026-10-08")).ok);
  REQUIRE(s.setLiveGateFloor(-45.0).ok);
  CHECK_FALSE(s.deviceCalibration()->driftBaselineDbfs.has_value());  // a record with no baseline yet learns one
  CHECK(s.setDriftBaseline(-21.5).ok);
  CHECK(s.deviceCalibration()->driftBaselineDbfs == -21.5);
  CHECK(s.deviceCalibration()->liveGateFloorDbfs == -45.0);  // the rest of the record is kept
  CHECK(s.deviceCalibration()->dbu == 12.5);
  // The learned gate floor is written later: it must not drop the baseline.
  REQUIRE(s.setLiveGateFloor(-46.0).ok);
  CHECK(s.deviceCalibration()->driftBaselineDbfs == -21.5);
  // Out of [-80, 0] or not a number: refused, the stored value stays.
  CHECK_FALSE(s.setDriftBaseline(1.0).ok);
  CHECK_FALSE(s.setDriftBaseline(-81.0).ok);
  CHECK_FALSE(s.setDriftBaseline(std::nan("")).ok);
  CHECK(s.deviceCalibration()->driftBaselineDbfs == -21.5);
  {
    Settings again(file);  // survives a reload
    REQUIRE(again.load().empty());
    CHECK(again.deviceCalibration()->driftBaselineDbfs == -21.5);
  }
  CHECK(s.setDriftBaseline(std::nullopt).ok);  // cleared
  CHECK_FALSE(s.deviceCalibration()->driftBaselineDbfs.has_value());
  // Re-picking a device (a new record) clears the baseline, as it does the floor; so does removing the record.
  REQUIRE(s.setDriftBaseline(-20.0).ok);
  REQUIRE(s.setDeviceCalibration(recordFromPreset(*findDevicePreset("scarlett-4i4-3g"), "2026-10-09")).ok);
  CHECK_FALSE(s.deviceCalibration()->driftBaselineDbfs.has_value());
  REQUIRE(s.setDriftBaseline(-20.0).ok);
  REQUIRE(s.setDeviceCalibration(std::nullopt).ok);
  CHECK_FALSE(s.setDriftBaseline(-20.0).ok);
  // A record handed in with an out-of-range baseline is stored without it.
  DeviceCalibrationRecord bad = recordFromValue(12.0, DeviceMethod::Manual, false, false, "2026-10-09");
  bad.driftBaselineDbfs = -120.0;
  REQUIRE(s.setDeviceCalibration(bad).ok);
  CHECK_FALSE(s.deviceCalibration()->driftBaselineDbfs.has_value());
}

TEST_CASE("drift baseline: the record JSON round-trips it and drops an out-of-range one", "[devicecal][drift]") {
  DeviceCalibrationRecord r = recordFromValue(12.0, DeviceMethod::Manual, false, false, "2026-10-09");
  r.driftBaselineDbfs = -18.25;
  const auto back = recordFromJson(recordToJson(r));
  REQUIRE(back.has_value());
  CHECK(*back == r);
  json j = recordToJson(r);
  j["driftBaselineDbfs"] = 3.0;
  CHECK_FALSE(recordFromJson(j)->driftBaselineDbfs.has_value());
  CHECK_FALSE(recordToJson(recordFromValue(12.0, DeviceMethod::Manual, false, false, "x")).contains("driftBaselineDbfs"));
}

TEST_CASE("drift check: the engine measures only with calibrated input levels on AND a device record", "[devicecal][drift]") {
  const auto rec = recordFromPreset(*findDevicePreset("scarlett-4i4-3g"), "2026-10-08");
  CHECK_FALSE(engineCalibrationFor(false, rec).driftCheck);
  CHECK_FALSE(engineCalibrationFor(false, std::nullopt).driftCheck);
  CHECK_FALSE(engineCalibrationFor(true, std::nullopt).driftCheck);
  CHECK(engineCalibrationFor(true, rec).driftCheck);
}
