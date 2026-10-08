#pragma once

#include <optional>

#include "sawblade/chain.h"

namespace sawblade::plugin {

// v0.8 I2: what an Engine is built with besides the preset. Default = calibration off and the stock gate seed, which builds the very
// engine that was built before I2 (bit-identical). The device record and the Settings toggle produce it (settings/DeviceCalibration.h).
struct EngineCalibration {
  ChainCalibration chain;                   // enabled only with the "Calibrated input levels (beta)" toggle
  std::optional<double> gateFloorSeedDb;    // the device record's learned live-gate floor; used only when chain.enabled
};

}  // namespace sawblade::plugin
