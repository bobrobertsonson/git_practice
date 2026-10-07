#include "sawblade/calibration.h"

#include <cmath>

namespace sawblade::calibration {

double dbfsToDbu(double dbfs, double deviceDbu) noexcept { return dbfs + deviceDbu; }
double dbuToDbfs(double dbu, double deviceDbu) noexcept { return dbu - deviceDbu; }
double dbToLinear(double db) noexcept { return std::pow(10.0, db / 20.0); }

bool DeviceCalibration::calibrated() const noexcept { return dbu && std::isfinite(*dbu); }

CalibrationDefaults defaultCalibrationDefaults() noexcept { return {}; }

namespace {
std::optional<double> finiteOnly(const std::optional<double>& v) noexcept {
  if (v && std::isfinite(*v)) return v;
  return std::nullopt;
}

const GearDefault& defaultFor(const CalibrationDefaults& d, GearKind g) noexcept {
  switch (g) {
    case GearKind::Amp: return d.amp;
    case GearKind::Pedal: return d.pedal;
    case GearKind::FullRig: return d.fullRig;
    case GearKind::Unknown: break;
  }
  return d.unknown;
}
}  // namespace

PathPlan planPath(const DeviceCalibration& device, std::span<const BlockLevelInfo> blocks,
                  const CalibrationDefaults& defaults) {
  PathPlan plan;
  plan.deviceUncalibrated = !device.calibrated();
  plan.blocks.reserve(blocks.size());
  std::optional<double> ref = device.calibrated() ? device.dbu : std::nullopt;

  for (const auto& b : blocks) {
    BlockGain g;
    const auto& gd = defaultFor(defaults, b.gear);
    if (b.kind == LevelKind::Nam) {
      auto in = finiteOnly(b.inputDbu);
      auto out = finiteOnly(b.outputDbu);
      g.inputMissing = !in;
      g.outputMissing = !out;
      if (!in) in = finiteOnly(gd.inputDbu);
      if (!out) out = finiteOnly(gd.outputDbu);
      if (ref && in) {
        g.gainInDb = *ref - *in;
      } else if (!ref && in) {
        ref = in;  // unknown reference resolves neutrally: gain 0
      }                // else no input known: neutral, gain 0
      if (out) ref = out;
    } else if (b.kind == LevelKind::NominalOutput) {
      auto out = finiteOnly(b.outputDbu);
      g.outputMissing = !out;
      if (!out) out = finiteOnly(gd.outputDbu);
      if (out) ref = out;
    }
    g.gainInLinear = static_cast<float>(dbToLinear(g.gainInDb));
    g.refAfterDbu = ref;
    if (g.uncalibrated()) plan.anyUncalibrated = true;
    plan.blocks.push_back(g);
  }
  plan.refOutDbu = ref;
  return plan;
}

}  // namespace sawblade::calibration
