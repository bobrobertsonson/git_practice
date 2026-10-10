#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// Phase 11 (docs/specs/phase11_settings.md section 1): the plugin's settings store. Nothing here
// touches the audio thread. JUCE-free apart from the special-location lookups in Env::system().
namespace sawblade::plugin::settings {

// Everything the store reads from the machine, injectable so tests never touch the real home.
struct Env {
  std::function<std::optional<std::string>(std::string_view name)> getenv;
  std::filesystem::path home;        // ~
  std::filesystem::path executable;  // the running binary
  std::filesystem::path sourceDir;   // compile-time SAWBLADE_SOURCE_DIR ("" when unknown)
  std::function<bool(const std::filesystem::path&)> exists;
  bool isMac = false;

  // The real process. getenv is evaluated at call time (tests set variables after startup).
  static Env system();
  // An unset or empty variable reads as nullopt.
  std::optional<std::string> var(std::string_view name) const;
};

struct Result {
  bool ok = true;
  std::string error;    // set when !ok
  std::string warning;  // set when ok but suspicious
};

// Where things live (all overridable by environment variables, see the table in docs/PLUGIN.md).
struct Paths {
  static std::filesystem::path appDataDir(const Env&);  // SAWBLADE_APPDATA, SAWBLADE_DATA_DIR, platform default (= AppPaths.h)
  static std::filesystem::path settingsFile(const Env&);
  static std::filesystem::path tokenFile(const Env&);
  static std::filesystem::path captureCacheDir(const Env&);
  static std::filesystem::path takesDir(const Env&);
};

// Auto-detect of the match venv (first dir with bin/sawblade-t3k wins): SAWBLADE_MATCH_VENV, the
// build tree's match/.venv, ~/sawblade/match/.venv.
std::optional<std::filesystem::path> detectMatchVenv(const Env&);

// True if `value` contains a TONE3000 secret key marker (t3k_cs_, any case).
bool containsSecretKey(std::string_view value);
extern const char* const kSecretKeyMessage;

// Where the effective TONE3000 client id came from.
enum class ClientIdSource { None, Stored, Environment, TokenFile };
struct ClientIdResolution {
  std::string id;
  ClientIdSource source = ClientIdSource::None;
};

class Settings {
 public:
  struct Listener {
    virtual ~Listener() = default;
    virtual void settingsChanged() = 0;
  };

  static constexpr const char* kDefaultSeparationModel = "htdemucs_6s";

  explicit Settings(std::filesystem::path file, Env env = Env::system());  // does not touch disk
  ~Settings();
  // The process-wide instance at Paths::settingsFile(Env::system()), load()ed on first use.
  static Settings& shared();
  // Test hook: forget the shared instance so the next shared() re-reads SAWBLADE_SETTINGS_FILE.
  static void resetSharedForTests();
  static bool sharedExistsForTests();  // true once shared() has been called since the last reset

  const std::filesystem::path& file() const { return file_; }
  const Env& env() const { return env_; }

  bool fileExists() const;
  bool isFirstRun() const;  // no file existed when this instance first load()ed
  void markFirstRunCompleted();
  std::string load();  // "" or an error text
  // The text load() returned last time (malformed file, dropped secret key); the panel shows it.
  std::string loadError() const;
  std::string save();  // "" or an error text

  std::optional<std::filesystem::path> matchVenvDir() const;
  std::optional<std::filesystem::path> effectiveMatchVenvDir() const;
  std::optional<std::filesystem::path> captureCacheDir() const;
  std::filesystem::path effectiveCaptureCacheDir() const;
  std::string tone3000ClientId() const;
  // stored, else TONE3000_CLIENT_ID, else client_id from the token file (never a t3k_cs_ value; only that one
  // key of the token file is ever read).
  std::string effectiveTone3000ClientId() const;
  ClientIdResolution resolveTone3000ClientId() const;  // the same, with the source
  std::string separationModel() const;
  std::optional<std::filesystem::path> takesDir() const;
  std::filesystem::path effectiveTakesDir() const;
  std::string theme() const;
  double uiScale() const;
  bool firstRunCompleted() const;
  // v0.3 level matching: apply each preset's auto trim (docs/PRESET_SCHEMA.md "Level matching"). A plugin setting, never part of the
  // preset. Default true.
  bool levelMatch() const;

  Result setMatchVenvDir(std::optional<std::filesystem::path>);
  Result setCaptureCacheDir(std::optional<std::filesystem::path>);
  Result setTone3000ClientId(std::string);
  Result setSeparationModel(std::string);
  Result setTakesDir(std::optional<std::filesystem::path>);
  Result setTheme(std::string);
  Result setUiScale(double);
  Result setLevelMatch(bool);

  std::filesystem::path tokenFile() const;
  // <effective venv>/bin/<tool>, "" when there is no venv.
  std::filesystem::path toolPath(std::string_view tool) const;

  void addListener(Listener*);
  void removeListener(Listener*);

 private:
  std::string saveLocked();
  Result setPathKey(const char* key, std::optional<std::filesystem::path> v);
  Result finish(Result r);  // save + notify
  void notify();
  void applyCacheEnv();
  std::optional<std::filesystem::path> pathKey(const char* key) const;

  std::filesystem::path file_;
  Env env_;
  mutable std::mutex m_;
  nlohmann::json doc_ = nlohmann::json::object();
  bool loadedOnce_ = false, firstRun_ = false;
  std::string loadError_;
  bool applyProcessEnv_ = false;  // only the shared() instance sets the core cache-root override
  std::vector<Listener*> listeners_;
};

}  // namespace sawblade::plugin::settings
