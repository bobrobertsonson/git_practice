#pragma once

// The export panel's last settings (docs/PLUGIN.md "NAM export"): UI state, saved in the plugin state as the optional
// `export` object beside `playAlong`, never in the preset. JUCE-free so the processor can hold it.

#include <string>

#include <nlohmann/json.hpp>

namespace sawblade::plugin {

struct ExportSettings {
  std::string mode;                  // "" = follow the rig (shared cab: nocab, per-path cabs: withcab), "nocab", "withcab"
  std::string arch = "a2";           // a2 (default: A2-capable loaders such as the Anagram) / a1 (older loaders)
  std::string size = "full";         // a2: full / lite; a1: feather / lite / standard (see exportSizeValid)
  std::string diSource = "take";     // take (the newest take if one exists, else the built-in signal) / builtin
  std::string compChoice = "drop";   // drop (exact) / keep (inexact: --allow-inexact); only used with a no-cab export of a rig with the comp on
  std::string outputFolder;          // "" = <app data>/exports

  bool operator==(const ExportSettings&) const = default;
  bool isDefault() const { return *this == ExportSettings{}; }
};

// The sizes each architecture offers (sawblade-export --arch/--size): a2 = full | lite, a1 = feather | lite | standard.
bool exportSizeValid(const std::string& arch, const std::string& size);
// The default size of an architecture (a2: full, a1: standard).
std::string defaultExportSize(const std::string& arch);
// Wall-time history key: "a2.full", "a2.lite", "a1.standard", ...
std::string exportHistoryKey(const std::string& arch, const std::string& size);

nlohmann::json exportSettingsToJson(const ExportSettings& s);
// Tolerant: never throws; wrong-typed or unknown values keep the defaults. Migration (v0.6): a saved object WITHOUT `arch` is
// from before A2 and its `size` (feather / lite / standard) was an A1 size: it becomes arch "a1" with that size, so an
// existing user keeps what they had. Only a new install (nothing saved) starts on A2 Full.
ExportSettings exportSettingsFromJson(const nlohmann::json& j);

}  // namespace sawblade::plugin
