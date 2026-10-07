#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "sawblade/calibration.h"

using namespace sawblade::calibration;
using Catch::Approx;

namespace {
BlockLevelInfo nam(GearKind g, std::optional<double> in, std::optional<double> out) {
  return {LevelKind::Nam, g, in, out};
}
BlockLevelInfo eq() { return {LevelKind::Neutral, GearKind::Unknown, std::nullopt, std::nullopt}; }
constexpr double kDev = 10.0;  // deliberately != kAssumedDeviceDbu
DeviceCalibration dev(double dbu = kDev) { return {dbu}; }
DeviceCalibration noDev() { return {std::nullopt}; }
}  // namespace

TEST_CASE("calibration: dBu/dBFS conversion", "[calibration]") {
  REQUIRE(dbuToDbfs(-9.0, 9.0) == Approx(-18.0));
  REQUIRE(dbfsToDbu(-18.0, 9.0) == Approx(-9.0));
  for (double x : {-40.0, -18.0, 0.0, 3.5, 12.0}) {
    REQUIRE(dbuToDbfs(dbfsToDbu(x, 10.0), 10.0) == Approx(x).margin(1e-12));
  }
  REQUIRE(dbToLinear(0.0) == Approx(1.0));
  REQUIRE(dbToLinear(20.0) == Approx(10.0));
  REQUIRE(dbToLinear(-6.0206) == Approx(0.5).epsilon(1e-4));
}

TEST_CASE("calibration: single amp input gain", "[calibration]") {
  std::vector<BlockLevelInfo> p{nam(GearKind::Amp, 18.0, 4.0)};
  auto plan = planPath(dev(), p, defaultCalibrationDefaults());
  REQUIRE(plan.blocks.size() == 1);
  REQUIRE(plan.blocks[0].gainInDb == Approx(kDev - 18.0).margin(1e-12));
  REQUIRE(std::abs(plan.blocks[0].gainInLinear - std::pow(10.0, (kDev - 18.0) / 20.0)) < 1e-6);
  REQUIRE(plan.blocks[0].refAfterDbu == 4.0);
  REQUIRE(plan.refOutDbu == 4.0);
  REQUIRE_FALSE(plan.blocks[0].uncalibrated());
  REQUIRE_FALSE(plan.anyUncalibrated);
  REQUIRE_FALSE(plan.deviceUncalibrated);
}

TEST_CASE("calibration: pedal to amp hop", "[calibration]") {
  std::vector<BlockLevelInfo> p{nam(GearKind::Pedal, 5.0, -2.0), nam(GearKind::Amp, 8.0, 0.0)};
  auto plan = planPath(dev(), p, defaultCalibrationDefaults());
  REQUIRE(plan.blocks[0].gainInDb == Approx(kDev - 5.0));
  REQUIRE(plan.blocks[1].gainInDb == Approx(-2.0 - 8.0));

  std::vector<BlockLevelInfo> q{nam(GearKind::Pedal, 5.0, -2.0), eq(), nam(GearKind::Amp, 8.0, 0.0)};
  auto planEq = planPath(dev(), q, defaultCalibrationDefaults());
  REQUIRE(planEq.blocks[1].gainInDb == 0.0);
  REQUIRE(planEq.blocks[1].gainInLinear == 1.0f);
  REQUIRE(planEq.blocks[1].refAfterDbu == -2.0);
  REQUIRE(planEq.blocks[2].gainInDb == Approx(plan.blocks[1].gainInDb));
  REQUIRE(planEq.refOutDbu == plan.refOutDbu);
}

TEST_CASE("calibration: amp swap changes only the amp gain by -d", "[calibration]") {
  const double d = 3.25;
  auto build = [](double ampIn) {
    return std::vector<BlockLevelInfo>{nam(GearKind::Pedal, 5.0, -2.0), eq(),
                                       nam(GearKind::Amp, ampIn, 0.0), eq()};
  };
  auto a = build(8.0), b = build(8.0 + d);
  auto pa = planPath(dev(), a, defaultCalibrationDefaults());
  auto pb = planPath(dev(), b, defaultCalibrationDefaults());
  REQUIRE(pb.blocks[2].gainInDb - pa.blocks[2].gainInDb == Approx(-d).margin(1e-12));
  for (size_t i : {0u, 1u, 3u}) {
    REQUIRE(pa.blocks[i].gainInDb == pb.blocks[i].gainInDb);
    REQUIRE(pa.blocks[i].refAfterDbu == pb.blocks[i].refAfterDbu);
  }
  REQUIRE(pa.refOutDbu == pb.refOutDbu);
}

TEST_CASE("calibration: missing metadata uses gear default or neutral, and is flagged", "[calibration]") {
  CalibrationDefaults defs;
  defs.amp = {10.0, 1.0};

  SECTION("gear default applied") {
    std::vector<BlockLevelInfo> p{nam(GearKind::Amp, std::nullopt, std::nullopt)};
    auto plan = planPath(dev(), p, defs);
    REQUIRE(plan.blocks[0].gainInDb == Approx(kDev - 10.0));
    REQUIRE(plan.blocks[0].refAfterDbu == 1.0);
    REQUIRE(plan.blocks[0].inputMissing);
    REQUIRE(plan.blocks[0].outputMissing);
    REQUIRE(plan.blocks[0].uncalibrated());
    REQUIRE(plan.anyUncalibrated);
  }
  SECTION("no default: neutral") {
    std::vector<BlockLevelInfo> p{nam(GearKind::Pedal, std::nullopt, std::nullopt)};
    auto plan = planPath(dev(), p, defs);
    REQUIRE(plan.blocks[0].gainInDb == 0.0);
    REQUIRE(plan.blocks[0].refAfterDbu == kDev);
    REQUIRE(plan.blocks[0].inputMissing);
    REQUIRE(plan.blocks[0].outputMissing);
    REQUIRE(plan.anyUncalibrated);
  }
  SECTION("only one side missing") {
    std::vector<BlockLevelInfo> p{nam(GearKind::Amp, 6.0, std::nullopt)};
    auto plan = planPath(dev(), p, defaultCalibrationDefaults());
    REQUIRE_FALSE(plan.blocks[0].inputMissing);
    REQUIRE(plan.blocks[0].outputMissing);
    REQUIRE(plan.blocks[0].gainInDb == Approx(kDev - 6.0));
    REQUIRE(plan.blocks[0].refAfterDbu == kDev);
  }
  SECTION("defaults helper is all neutral") {
    auto d = defaultCalibrationDefaults();
    REQUIRE_FALSE(d.amp.inputDbu);
    REQUIRE_FALSE(d.pedal.outputDbu);
    REQUIRE_FALSE(d.fullRig.inputDbu);
    REQUIRE_FALSE(d.unknown.outputDbu);
  }
}

TEST_CASE("calibration: non-finite metadata is missing; non-finite device is uncalibrated", "[calibration]") {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  std::vector<BlockLevelInfo> p{nam(GearKind::Amp, nan, inf)};
  auto plan = planPath(dev(), p, defaultCalibrationDefaults());
  REQUIRE(plan.blocks[0].inputMissing);
  REQUIRE(plan.blocks[0].outputMissing);
  REQUIRE(plan.blocks[0].gainInDb == 0.0);
  REQUIRE(std::isfinite(plan.blocks[0].gainInLinear));
  REQUIRE_FALSE(plan.deviceUncalibrated);
  REQUIRE(plan.refOutDbu == kDev);

  REQUIRE(dev().calibrated());
  REQUIRE_FALSE(noDev().calibrated());
  REQUIRE_FALSE(DeviceCalibration{nan}.calibrated());
  REQUIRE_FALSE(DeviceCalibration{inf}.calibrated());
  std::vector<BlockLevelInfo> ok{nam(GearKind::Amp, 7.0, 0.0)};
  REQUIRE(planPath(DeviceCalibration{nan}, ok, defaultCalibrationDefaults()).deviceUncalibrated);
}

TEST_CASE("calibration: uncalibrated device assumes kAssumedDeviceDbu", "[calibration]") {
  const auto defs = defaultCalibrationDefaults();
  REQUIRE(kAssumedDeviceDbu == 12.0);

  SECTION("lone amp: gain = assumed - in, device flagged") {
    std::vector<BlockLevelInfo> p{nam(GearKind::Amp, 7.0, 0.0)};
    auto plan = planPath(noDev(), p, defs);
    REQUIRE(plan.deviceUncalibrated);
    REQUIRE(plan.blocks[0].gainInDb == Approx(kAssumedDeviceDbu - 7.0));
    REQUIRE_FALSE(plan.blocks[0].uncalibrated());  // block metadata is fine; the device is what is assumed
    REQUIRE_FALSE(plan.anyUncalibrated);
    REQUIRE(plan.refOutDbu == 0.0);
  }
  SECTION("amp swap changes the gain by exactly -d even when uncalibrated") {
    const double d = 2.5;
    std::vector<BlockLevelInfo> a{nam(GearKind::Amp, 7.0, 0.0)}, b{nam(GearKind::Amp, 7.0 + d, 0.0)};
    auto pa = planPath(noDev(), a, defs), pb = planPath(noDev(), b, defs);
    REQUIRE(pb.blocks[0].gainInDb - pa.blocks[0].gainInDb == Approx(-d).margin(1e-12));
  }
  SECTION("pedal then amp: first gain from the assumed level, hop exact") {
    std::vector<BlockLevelInfo> p{nam(GearKind::Pedal, 5.0, -2.0), eq(), nam(GearKind::Amp, 8.0, 0.0)};
    auto plan = planPath(noDev(), p, defs);
    REQUIRE(plan.deviceUncalibrated);
    REQUIRE(plan.blocks[0].gainInDb == Approx(kAssumedDeviceDbu - 5.0));
    REQUIRE(plan.blocks[1].gainInDb == 0.0);
    REQUIRE(plan.blocks[2].gainInDb == Approx(-2.0 - 8.0));
  }
  SECTION("known input, missing output, no default: flagged, ref unchanged") {
    std::vector<BlockLevelInfo> p{nam(GearKind::Amp, 7.0, std::nullopt)};
    auto plan = planPath(noDev(), p, defs);
    REQUIRE(plan.blocks[0].gainInDb == Approx(kAssumedDeviceDbu - 7.0));
    REQUIRE(plan.blocks[0].outputMissing);
    REQUIRE_FALSE(plan.blocks[0].inputMissing);
    REQUIRE(plan.refOutDbu == kAssumedDeviceDbu);
  }
  SECTION("block with no input is neutral and flagged; the next amp hops from the unchanged ref") {
    std::vector<BlockLevelInfo> p{nam(GearKind::Pedal, std::nullopt, std::nullopt), nam(GearKind::Amp, 9.0, 1.0)};
    auto plan = planPath(noDev(), p, defs);
    REQUIRE(plan.blocks[0].gainInDb == 0.0);
    REQUIRE(plan.blocks[0].uncalibrated());
    REQUIRE(plan.blocks[0].refAfterDbu == kAssumedDeviceDbu);
    REQUIRE(plan.blocks[1].gainInDb == Approx(kAssumedDeviceDbu - 9.0));
    REQUIRE(plan.refOutDbu == 1.0);
  }
}

TEST_CASE("calibration: nominal-output DSP pedal sets the next reference", "[calibration]") {
  std::vector<BlockLevelInfo> p{
      {LevelKind::NominalOutput, GearKind::Pedal, std::nullopt, -3.0}, nam(GearKind::Amp, 5.0, 0.0)};
  auto plan = planPath(dev(), p, defaultCalibrationDefaults());
  REQUIRE(plan.blocks[0].gainInDb == 0.0);
  REQUIRE(plan.blocks[0].refAfterDbu == -3.0);
  REQUIRE_FALSE(plan.blocks[0].uncalibrated());
  REQUIRE(plan.blocks[1].gainInDb == Approx(-3.0 - 5.0));

  std::vector<BlockLevelInfo> m{{LevelKind::NominalOutput, GearKind::Pedal, std::nullopt, std::nullopt}};
  auto pm = planPath(dev(), m, defaultCalibrationDefaults());
  REQUIRE(pm.blocks[0].outputMissing);
  REQUIRE(pm.blocks[0].refAfterDbu == kDev);

  auto pu = planPath(noDev(), p, defaultCalibrationDefaults());  // NominalOutput just sets the ref
  REQUIRE(pu.deviceUncalibrated);
  REQUIRE(pu.blocks[0].gainInDb == 0.0);
  REQUIRE(pu.blocks[1].gainInDb == Approx(-3.0 - 5.0));
}

TEST_CASE("calibration: implausible dBu values are treated as missing", "[calibration]") {
  std::vector<BlockLevelInfo> p{nam(GearKind::Amp, 1e6, -1e6)};
  auto plan = planPath(dev(), p, defaultCalibrationDefaults());
  REQUIRE(plan.blocks[0].inputMissing);
  REQUIRE(plan.blocks[0].outputMissing);
  REQUIRE(plan.blocks[0].gainInDb == 0.0);
  REQUIRE(std::isfinite(plan.blocks[0].gainInLinear));
  REQUIRE(plan.refOutDbu == kDev);

  // Bounds are inclusive.
  std::vector<BlockLevelInfo> edge{nam(GearKind::Amp, kMaxPlausibleDbu, kMinPlausibleDbu)};
  auto pe = planPath(dev(), edge, defaultCalibrationDefaults());
  REQUIRE_FALSE(pe.blocks[0].uncalibrated());
  REQUIRE(std::isfinite(pe.blocks[0].gainInLinear));

  // Implausible device value counts as uncalibrated.
  REQUIRE_FALSE(DeviceCalibration{1e6}.calibrated());
  REQUIRE_FALSE(DeviceCalibration{-1e6}.calibrated());
  std::vector<BlockLevelInfo> ok{nam(GearKind::Amp, 7.0, 0.0)};
  auto pd = planPath(DeviceCalibration{1e6}, ok, defaultCalibrationDefaults());
  REQUIRE(pd.deviceUncalibrated);
  REQUIRE(pd.blocks[0].gainInDb == Approx(kAssumedDeviceDbu - 7.0));
}
