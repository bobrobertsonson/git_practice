#pragma once

// The export panel's last settings (docs/PLUGIN.md "NAM export"): UI state, saved in the plugin state as the optional
// `export` object beside `playAlong`, never in the preset. JUCE-free so the processor can hold it.

#include <string>

#include <nlohmann/json.hpp>

namespace sawblade::plugin {

struct ExportSettings {
  std::string mode;                  // "" = follow the rig (shared cab: nocab, per-path cabs: withcab), "nocab", "withcab"
  std::string size = "standard";     // feather / lite / standard
  std::string diSource = "take";     // take (the newest take if one exists, else the built-in signal) / builtin
  std::string compChoice = "drop";   // drop (exact) / keep (inexact: --allow-inexact); only used with a no-cab export of a rig with the comp on
  std::string outputFolder;          // "" = <app data>/exports

  bool operator==(const ExportSettings&) const = default;
  bool isDefault() const { return *this == ExportSettings{}; }
};

nlohmann::json exportSettingsToJson(const ExportSettings& s);
// Tolerant: never throws; wrong-typed or unknown values keep the defaults.
ExportSettings exportSettingsFromJson(const nlohmann::json& j);

}  // namespace sawblade::plugin
