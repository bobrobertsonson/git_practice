#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// v0.8 I4b part 4: "Calibrate all user presets..." (docs/specs/v0_8-I4b-plugin.md section 4). Switches every LEGACY preset file of one
// folder to schema v5 "calibrated", one atomic write per file. JUCE-free; the plugin runs it off the message thread on the user bank and
// never passes the factory folder.
//
// A file is edited minimally: its JSON gets `"version": 5` and `"calibration": {"mode": "calibrated"}` and nothing else, and the result is
// only written when it parses to exactly the preset the file held, with the mode changed (so nothing else can change meaning with the
// version). Files that are already calibrated are not touched (not even rewritten). A failure on one file is reported and does not stop the
// rest.
namespace sawblade {

enum class BulkCalibrateStatus {
  Converted,          // rewritten as v5 "calibrated"
  AlreadyCalibrated,  // left byte for byte as it was
  Unreadable,         // not a preset this build can parse: left alone
  WriteFailed         // the converted text could not be written (or did not verify): left as it was
};

struct BulkCalibrateEntry {
  std::filesystem::path file;
  BulkCalibrateStatus status = BulkCalibrateStatus::AlreadyCalibrated;
  std::string error;  // set for Unreadable and WriteFailed
};

struct BulkCalibrateResult {
  std::vector<BulkCalibrateEntry> files;
  int count(BulkCalibrateStatus s) const;
  int converted() const { return count(BulkCalibrateStatus::Converted); }
  int failed() const { return count(BulkCalibrateStatus::WriteFailed); }
};

// The preset files directly in `dir` (*.json, not *.resolved.json: the preset library's rule) that parse and are legacy: what a bulk run
// would convert. Its size is the count the confirmation shows. Sorted by file name.
std::vector<std::filesystem::path> legacyPresetFiles(const std::filesystem::path& dir);

// Writes `text` to `file` (replacing it); returns "" or an error. The default writes `<file>.tmp` and renames it over `file`.
using AtomicWriteFn = std::function<std::string(const std::filesystem::path& file, const std::string& text)>;
std::string atomicWriteText(const std::filesystem::path& file, const std::string& text);

// Converts every legacy file of `dir`. `write` replaces the default atomic write (tests inject failures). `onFile` (optional) is called after
// each file, for progress.
BulkCalibrateResult calibrateLegacyPresets(const std::filesystem::path& dir, const AtomicWriteFn& write = atomicWriteText,
                                           const std::function<void(const BulkCalibrateEntry&)>& onFile = {});

}  // namespace sawblade
