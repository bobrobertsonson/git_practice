#include "sawblade/calibration.h"

#include <cmath>

namespace sawblade::calibration {

double dbfsToDbu(double dbfs, double deviceDbu) noexcept { return dbfs + deviceDbu; }
double dbuToDbfs(double dbu, double deviceDbu) noexcept { return dbu - deviceDbu; }
double dbToLinear(double db) noexcept { return std::pow(10.0, db / 20.0); }

namespace {
bool plausible(double v) noexcept { return std::isfinite(v) && v >= kMinPlausibleDbu && v <= kMaxPlausibleDbu; }
}  // namespace

bool DeviceCalibration::calibrated() const noexcept { return dbu && plausible(*dbu); }

CalibrationDefaults defaultCalibrationDefaults() noexcept { return {}; }

namespace {
std::optional<double> finiteOnly(const std::optional<double>& v) noexcept {
  if (v && plausible(*v)) return v;
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

BlockGain planBlock(double ref, const BlockLevelInfo& b, const CalibrationDefaults& defaults) noexcept {
  BlockGain g;
  const auto& gd = defaultFor(defaults, b.gear);
  if (b.kind == LevelKind::Nam) {
    auto in = finiteOnly(b.inputDbu);
    auto out = finiteOnly(b.outputDbu);
    g.inputMissing = !in;
    g.outputMissing = !out;
    if (!in) in = finiteOnly(gd.inputDbu);
    if (!out) out = finiteOnly(gd.outputDbu);
    g.inputUnknown = !in;
    if (in) g.gainInDb = ref - *in;  // else no input known: neutral, gain 0
    if (out) ref = *out;
  } else if (b.kind == LevelKind::NominalOutput) {
    auto out = finiteOnly(b.outputDbu);
    g.outputMissing = !out;
    if (!out) out = finiteOnly(gd.outputDbu);
    if (out) ref = *out;
  }
  g.gainInLinear = static_cast<float>(dbToLinear(g.gainInDb));
  g.refAfterDbu = ref;
  return g;
}

PathPlan planPath(const DeviceCalibration& device, std::span<const BlockLevelInfo> blocks,
                  const CalibrationDefaults& defaults) {
  PathPlan plan;
  plan.deviceUncalibrated = !device.calibrated();
  plan.blocks.reserve(blocks.size());
  double ref = device.calibrated() ? *device.dbu : kAssumedDeviceDbu;
  for (const auto& b : blocks) {
    const BlockGain g = planBlock(ref, b, defaults);
    ref = g.refAfterDbu;
    if (g.uncalibrated()) plan.anyUncalibrated = true;
    plan.blocks.push_back(g);
  }
  plan.refOutDbu = ref;
  return plan;
}

}  // namespace sawblade::calibration
