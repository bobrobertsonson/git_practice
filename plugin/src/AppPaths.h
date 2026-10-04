#pragma once

// Where Sawblade keeps its own files (JUCE-free). Linux: ~/.local/share/sawblade, macOS:
// ~/Library/Application Support/Sawblade. The environment variable SAWBLADE_DATA_DIR replaces that
// root (tests, portable setups). Everything below it is created lazily by whoever writes there; reading
// a missing directory is never an error.

#include <cstdlib>
#include <filesystem>
#include <string>

namespace sawblade::plugin {

inline std::filesystem::path appDataDir() {
  if (const char* e = std::getenv("SAWBLADE_DATA_DIR"); e != nullptr && *e != '\0') return std::filesystem::path(e);
  const char* home = std::getenv("HOME");
  const std::filesystem::path h = (home != nullptr && *home != '\0') ? std::filesystem::path(home) : std::filesystem::path("/tmp");
#if defined(__APPLE__)
  return h / "Library" / "Application Support" / "Sawblade";
#else
  return h / ".local" / "share" / "sawblade";
#endif
}

inline std::filesystem::path defaultTakesDir() { return appDataDir() / "takes"; }
inline std::filesystem::path defaultJobsDir() { return appDataDir() / "jobs"; }
inline std::filesystem::path defaultSettingsFile() { return appDataDir() / "settings.properties"; }

}  // namespace sawblade::plugin
