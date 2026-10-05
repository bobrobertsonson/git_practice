// Shared by the editor test executables: points the settings at a temp dir (see SettingsEnv).
#pragma once

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <juce_core/juce_core.h>

#include "settings/Settings.h"
#include "settings/SettingsPanel.h"

namespace {
// Each editor test points the settings at its own temp file (the editor reads Settings::shared() when it is
// constructed) and forgets the once-per-process first-run flag. `json == nullptr`: no settings file, so
// this is a first run.
struct SettingsEnv {
  std::filesystem::path dir;
  explicit SettingsEnv(const char* json) {
    dir = std::filesystem::temp_directory_path() / ("sawblade_editor_settings_" + std::to_string(juce::Random::getSystemRandom().nextInt64() & 0xffffff));
    std::filesystem::create_directories(dir);
    ::setenv("SAWBLADE_SETTINGS_FILE", (dir / "settings.json").c_str(), 1);
    ::setenv("SAWBLADE_T3K_TOKEN_FILE", (dir / "tokens.json").c_str(), 1);
    ::setenv("SAWBLADE_CACHE_DIR", (dir / "cache").c_str(), 1);
    ::unsetenv("SAWBLADE_MATCH_VENV");
    ::unsetenv("TONE3000_CLIENT_ID");
    if (json != nullptr) std::ofstream(dir / "settings.json") << json;
    sawblade::plugin::settings::Settings::resetSharedForTests();
    sawblade::plugin::settings::SettingsPanel::resetFirstRunShownForTests();
  }
  ~SettingsEnv() {
    sawblade::plugin::settings::Settings::resetSharedForTests();
    for (const char* v : {"SAWBLADE_SETTINGS_FILE", "SAWBLADE_T3K_TOKEN_FILE", "SAWBLADE_CACHE_DIR"}) ::unsetenv(v);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};
constexpr const char* kSettingsExist = "{\"version\": 1, \"firstRunCompleted\": true}";
}  // namespace
