#pragma once
// Input level calibration math (v0.8 Task B1). Pure functions; planPath() runs at load/swap time (it allocates
// its result vector), the audio thread only reads the precomputed linear gains later. All math in double.
#include <optional>
#include <span>
#include <vector>

namespace sawblade::calibration {

// Outside any real interface/capture level; treated as missing.
constexpr double kMinPlausibleDbu = -60.0, kMaxPlausibleDbu = 60.0;

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

// Interface max input in dBu (the level that corresponds to 0 dBFS). There is no numeric default: when absent the
// reference starts "unknown" and the first block that needs one resolves it neutrally (see planPath).
// Method + date strings live in the caller layer (Settings).
struct DeviceCalibration {
  std::optional<double> dbu;
  bool calibrated() const noexcept;  // true iff dbu is finite and within the plausible range
};

struct BlockGain {
  double gainInDb = 0.0;
  float gainInLinear = 1.0f;
  std::optional<double> refAfterDbu;  // nullopt while the reference is still unknown
  bool inputMissing = false;   // NAM / NominalOutput side metadata absent (default or neutral used)
  bool outputMissing = false;
  bool uncalibrated() const noexcept { return inputMissing || outputMissing; }
};

struct PathPlan {
  std::vector<BlockGain> blocks;
  std::optional<double> refOutDbu;  // nullopt while the reference is still unknown
  bool deviceUncalibrated = false;
  bool anyUncalibrated = false;
};

// Reference starts at device.dbu. If absent (unknown): the first NAM block with a known input (metadata, else gear
// default) sets ref := its input (gain 0 dB, today's behaviour); a NominalOutput block just sets ref; a NAM block
// with no input at all stays neutral (0 dB). deviceUncalibrated is set. Hops after that are computed normally.
PathPlan planPath(const DeviceCalibration& device, std::span<const BlockLevelInfo> blocks,
                  const CalibrationDefaults& defaults);

}  // namespace sawblade::calibration
