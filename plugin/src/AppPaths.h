#pragma once

// Where Sawblade keeps its own files (JUCE-free). Linux: ~/.local/share/sawblade, macOS:
// ~/Library/Application Support/Sawblade ($XDG_DATA_HOME/sawblade when set on Linux). The environment
// variables SAWBLADE_APPDATA, then SAWBLADE_DATA_DIR, replace that root (tests, portable setups). This is the
// single definition: presets/T3kTool, the settings store and the take/job dirs all agree. Everything below it is created lazily by whoever writes there; reading
// a missing directory is never an error.

#include <cstdlib>
#include <filesystem>
#include <string>

namespace sawblade::plugin {

inline std::filesystem::path appDataDir() {
  for (const char* name : {"SAWBLADE_APPDATA", "SAWBLADE_DATA_DIR"})
    if (const char* e = std::getenv(name); e != nullptr && *e != '\0') return std::filesystem::path(e);
  const char* home = std::getenv("HOME");
  const std::filesystem::path h = (home != nullptr && *home != '\0') ? std::filesystem::path(home) : std::filesystem::path("/tmp");
#if defined(__APPLE__)
  return h / "Library" / "Application Support" / "Sawblade";
#else
  if (const char* x = std::getenv("XDG_DATA_HOME"); x != nullptr && *x != '\0') return std::filesystem::path(x) / "sawblade";
  return h / ".local" / "share" / "sawblade";
#endif
}

inline std::filesystem::path defaultTakesDir() { return appDataDir() / "takes"; }
inline std::filesystem::path defaultJobsDir() { return appDataDir() / "jobs"; }
inline std::filesystem::path defaultSettingsFile() { return appDataDir() / "settings.properties"; }

}  // namespace sawblade::plugin
