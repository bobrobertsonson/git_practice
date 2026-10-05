// Shared by the editor test executables: points the settings at a temp dir (see SettingsEnv).
#pragma once

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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
  std::optional<std::string> oldHome;
  bool homeIsolated = false;
  // isolateHome: HOME points into the temp dir too (for tests that read the fallback hooks, which consult Env::system()).
  explicit SettingsEnv(const char* json, bool isolateHome = false) {
    dir = std::filesystem::temp_directory_path() / ("sawblade_editor_settings_" + std::to_string(juce::Random::getSystemRandom().nextInt64() & 0xffffff));
    std::filesystem::create_directories(dir);
    ::setenv("SAWBLADE_SETTINGS_FILE", (dir / "settings.json").c_str(), 1);
    ::setenv("SAWBLADE_T3K_TOKEN_FILE", (dir / "tokens.json").c_str(), 1);
    ::setenv("SAWBLADE_CACHE_DIR", (dir / "cache").c_str(), 1);
    ::unsetenv("SAWBLADE_MATCH_VENV");
    if (isolateHome) {
      if (const char* h = std::getenv("HOME")) oldHome = h;
      homeIsolated = true;
      ::setenv("HOME", (dir / "home").c_str(), 1);
    }
    ::unsetenv("TONE3000_CLIENT_ID");
    if (json != nullptr) std::ofstream(dir / "settings.json") << json;
    sawblade::plugin::settings::Settings::resetSharedForTests();
    sawblade::plugin::settings::SettingsPanel::resetFirstRunShownForTests();
  }
  ~SettingsEnv() {
    sawblade::plugin::settings::Settings::resetSharedForTests();
    if (homeIsolated) {
      if (oldHome) ::setenv("HOME", oldHome->c_str(), 1);
      else ::unsetenv("HOME");
    }
    for (const char* v : {"SAWBLADE_SETTINGS_FILE", "SAWBLADE_T3K_TOKEN_FILE", "SAWBLADE_CACHE_DIR"}) ::unsetenv(v);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};
// For tests that call the fallback hooks (defaultT3kExecutable, MatchSettings::default*, BrowserSettings::defaultExecutable):
// HOME in a temp dir, SAWBLADE_MATCH_VENV unset, and a fresh Settings::shared() on both ends, so the result never depends on
// the developer's real settings or venv. Leaves SAWBLADE_SETTINGS_FILE and the app-data variables as the test set them.
struct HookIsolation {
  std::filesystem::path home;
  std::optional<std::string> oldHome, oldVenv;
  HookIsolation() {
    home = std::filesystem::temp_directory_path() / ("sawblade_hook_home_" + std::to_string(::getpid()));
    std::filesystem::create_directories(home);
    if (const char* h = std::getenv("HOME")) oldHome = h;
    if (const char* v = std::getenv("SAWBLADE_MATCH_VENV")) oldVenv = v;
    ::setenv("HOME", home.c_str(), 1);
    ::unsetenv("SAWBLADE_MATCH_VENV");
    sawblade::plugin::settings::Settings::resetSharedForTests();
  }
  ~HookIsolation() {
    sawblade::plugin::settings::Settings::resetSharedForTests();
    if (oldHome) ::setenv("HOME", oldHome->c_str(), 1);
    else ::unsetenv("HOME");
    if (oldVenv) ::setenv("SAWBLADE_MATCH_VENV", oldVenv->c_str(), 1);
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
  }
};
[[maybe_unused]] constexpr const char* kSettingsExist = "{\"version\": 1, \"firstRunCompleted\": true}";
}  // namespace
