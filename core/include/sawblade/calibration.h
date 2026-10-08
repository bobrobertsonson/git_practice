#pragma once
// Input level calibration math (v0.8 Task B1). Pure functions; planPath() runs at load/swap time (it allocates
// its result vector), the audio thread only reads the precomputed linear gains later. All math in double.
#include <optional>
#include <span>
#include <vector>

namespace sawblade::calibration {

// Outside any real interface/capture level; treated as missing.
constexpr double kMinPlausibleDbu = -60.0, kMaxPlausibleDbu = 60.0;

// Level assumed when the device has no in-range dbu (deviceUncalibrated stays true). Sources:
//  - NeuralAmpModelerPlugin InputCalibrationLevel default +12.0 dBu (NeuralAmpModeler.cpp:75, tag v0.7.15)
//  - Focusrite Scarlett 4i4 4th gen, instrument input at minimum gain: +12 dBu
//  - Audient iD4 MKII: +12 dBu
constexpr double kAssumedDeviceDbu = 12.0;

double dbfsToDbu(double dbfs, double deviceDbu) noexcept;  // dbfs + deviceDbu
double dbuToDbfs(double dbu, double deviceDbu) noexcept;   // dbu - deviceDbu
double dbToLinear(double db) noexcept;                     // 10^(db/20)

enum class GearKind { Amp, Pedal, FullRig, Unknown };  // mirrors TONE3000 gear types
enum class LevelKind {
  Nam,            // capture with (possibly missing) input/output dBu metadata
  Neutral,        // no level conversion (EQ, gain-neutral DSP): gain 0, reference unchanged
  NominalOutput   // modelled DSP pedal: gain 0, reference becomes outputDbu (its declared nominal output)
};

struct BlockLevelInfo {
  LevelKind kind = LevelKind::Neutral;
  GearKind gear = GearKind::Unknown;
  std::optional<double> inputDbu, outputDbu;
};

struct GearDefault {
  std::optional<double> inputDbu, outputDbu;
};

// Per-gear-type fallbacks for missing metadata. An unset entry means "neutral" (today's behaviour).
struct CalibrationDefaults {
  GearDefault amp, pedal, fullRig, unknown;
};

// PROVISIONAL: all entries unset (neutral) until the v0.8 Task A audit supplies documented values.
CalibrationDefaults defaultCalibrationDefaults() noexcept;

// Interface max input in dBu (the level that corresponds to 0 dBFS). When absent or out of range, planPath assumes
// kAssumedDeviceDbu and sets deviceUncalibrated. Method + date strings live in the caller layer (Settings).
struct DeviceCalibration {
  std::optional<double> dbu;
  bool calibrated() const noexcept;  // true iff dbu is finite and within the plausible range
};

struct BlockGain {
  double gainInDb = 0.0;
  float gainInLinear = 1.0f;
  double refAfterDbu = 0.0;
  bool inputMissing = false;   // NAM / NominalOutput side metadata absent (default or neutral used)
  bool outputMissing = false;
  bool inputUnknown = false;   // no usable input level at all (metadata and gear default both absent): gain is the neutral 0
  bool uncalibrated() const noexcept { return inputMissing || outputMissing; }
};

// What a block needs to apply its own calibration (v0.8 I1). The Chain fills it from planPath(); every Nam block (each
// gain-ladder rung included) then derives ITS planned gain from its own capture metadata with planBlock(), so a rung swap
// uses the rung's levels. Plain data: handing it to a block on the audio thread allocates nothing.
struct BlockCalibration {
  bool active = false;         // false: the block runs exactly as if calibration were off
  double refBeforeDbu = 0.0;   // dBu at 0 dBFS of the signal arriving at the block
  bool feedsNam = false;       // another Nam block follows in the same path: the hop is planned (no normalise / make-up)
  CalibrationDefaults defaults{};
};

struct PathPlan {
  std::vector<BlockGain> blocks;
  double refOutDbu = 0.0;
  bool deviceUncalibrated = false;
  bool anyUncalibrated = false;
};

// One step of planPath(): the block's gain given the reference arriving at it. Pure arithmetic, no allocation.
BlockGain planBlock(double refBeforeDbu, const BlockLevelInfo& block, const CalibrationDefaults& defaults) noexcept;

// Reference starts at device.dbu, or kAssumedDeviceDbu (with deviceUncalibrated set) when it is absent/out of range,
// so it is always known. NAM blocks with no usable input are neutral (0 dB) and flagged.
PathPlan planPath(const DeviceCalibration& device, std::span<const BlockLevelInfo> blocks,
                  const CalibrationDefaults& defaults);

}  // namespace sawblade::calibration
