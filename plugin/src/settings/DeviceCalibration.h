#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "EngineCalibration.h"
#include "sawblade/chain.h"
#include "sawblade/drift.h"

// v0.8 I2 Part 2: the device calibration record. It lives in the Settings store (the plugin's app settings), NEVER in a preset or in
// the plugin state, and it is only ever written off the audio thread. JUCE-free.
//
// The record says what the user's audio interface does with an instrument input at 0 dBFS: dBu (the level that reads 0 dBFS), how
// it was found (preset / typed / measured), and the interface's setting at that figure. Every published figure holds only with the
// interface's instrument gain at MINIMUM, which is why the device step tells the user to set it there and let Sawblade supply all gain.
//
// Never compute the level from a gain readout such as the Focusrite Control 2 dB number. docs/specs/v0_8-input_calibration_REPORT.md
// A1b: at minimum Inst gain the readout already shows 7 dB (not 0), so "12 - N" is wrong, and no Focusrite document says the readout
// is the analogue gain, that a step is exactly 1 dB, or what the maximum input is above minimum gain. Use minimum gain, or measure.
namespace sawblade::plugin::settings {

enum class DeviceMethod { Preset, Manual, Measured };
const char* deviceMethodName(DeviceMethod m) noexcept;  // "preset" | "manual" | "measured"

struct DeviceCalibrationRecord {
  double dbu = 12.0;                 // interface level at 0 dBFS
  DeviceMethod method = DeviceMethod::Manual;
  std::string model;                 // preset label ("" for a typed or measured value)
  bool gainAtMinimum = true;         // the figure holds with the instrument gain at minimum
  bool pad = false;
  bool air = false;
  std::string date;                  // YYYY-MM-DD the record was made
  std::optional<double> liveGateFloorDbfs;  // Part 3: the live gate's learned noise floor, seeds the follower on prepare
  std::optional<double> driftBaselineDbfs;  // I3: the p95 of the played DI peaks in the first minutes after the record was made (the drift check's reference)
  bool operator==(const DeviceCalibrationRecord&) const = default;
};

// One interface preset. `source` is the citation (also in docs/PLUGIN.md); all figures are the manufacturer's, at minimum gain.
struct DevicePreset {
  const char* id;
  const char* label;
  double dbu;
  bool pad;
  const char* source;
};
// Scarlett 4i4 3rd gen Inst +12.5 dBu / +14 dBu with PAD (the user's interface): Focusrite user guide, "measured at minimum gain",
//   https://userguides.focusrite.com/hc/en-gb/articles/23031514701842 and
//   https://fael-downloads-prod.focusrite.com/customer/prod/downloads/Scarlett%204i4%203rd%20Gen%20User%20Guide%20V2.pdf
// Scarlett 4i4 4th gen Inst +12 dBu: Focusrite Scarlett 4i4 4th gen user guide, "at minimum gain",
//   https://fael-downloads-prod.focusrite.com/customer/prod/downloads/scarlett_4i4_4th_gen_user_guide_v2-pdf-en.pdf
// (docs/specs/v0_8-input_calibration_REPORT.md A1b: figures confirmed from guide extracts; the PDFs themselves were not opened, so the
// user should check them against their own unit's guide.)
const std::vector<DevicePreset>& devicePresets();
const DevicePreset* findDevicePreset(std::string_view id);

// Plausible range for a typed or measured value, and the practical range outside which the UI warns.
constexpr double kDbuMin = -60.0, kDbuMax = 60.0;
constexpr double kDbuWarnLow = 0.0, kDbuWarnHigh = 24.0;
struct DbuCheck {
  bool ok = false;
  std::string error;    // set when !ok (outside [-60, +60] or not a number)
  std::string warning;  // set when ok but outside the practical range [0, +24]
};
DbuCheck checkDeviceDbu(double dbu);
// Parses what a user typed ("12.5", "+14", "12,5 dBu"); nullopt when it is not a number.
std::optional<double> parseDbuText(std::string_view text);

// Builds a record. Presets take their dBu and PAD from the table; typed and measured values are validated by the caller (checkDeviceDbu).
DeviceCalibrationRecord recordFromPreset(const DevicePreset& p, std::string date);
DeviceCalibrationRecord recordFromValue(double dbu, DeviceMethod method, bool pad, bool air, std::string date);

// JSON of the record as stored under "deviceCalibration" in the settings file. fromJson returns nullopt for a malformed or out-of-range
// record (it is then treated as "no record"); unknown keys are ignored.
nlohmann::json recordToJson(const DeviceCalibrationRecord& r);
std::optional<DeviceCalibrationRecord> recordFromJson(const nlohmann::json& j);

// Today's date (local time) as YYYY-MM-DD.
std::string todayDate();

// The core setting for the engine, level measurements and the make-up: enabled iff the beta toggle is on; the device level is the
// record's (none = the +12 dBu assumption, flagged as such by the plan).
ChainCalibration chainCalibrationFor(bool calibratedInputLevels, const std::optional<DeviceCalibrationRecord>& record);

// What the engine is built with. The live-gate floor seed is used only with calibration on (with it off the engine is bit-identical to
// before this feature) and only from the device record: never from a preset or a matched reference DI.
EngineCalibration engineCalibrationFor(bool calibratedInputLevels, const std::optional<DeviceCalibrationRecord>& record);

// "Interface not calibrated: assuming +12 dBu" (the notice shown while calibrated input levels are on and no record exists).
std::string uncalibratedNotice();

}  // namespace sawblade::plugin::settings
