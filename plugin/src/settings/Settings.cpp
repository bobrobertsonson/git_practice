#include "Settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <system_error>

#include <juce_core/juce_core.h>

#include "sawblade/preset.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef SAWBLADE_SOURCE_DIR
#define SAWBLADE_SOURCE_DIR ""
#endif

namespace sawblade::plugin::settings {
namespace fs = std::filesystem;

const char* const kSecretKeyMessage =
    "That is a TONE3000 *secret* key (t3k_cs_\xe2\x80\xa6). Sawblade never stores it. Use the *publishable* key (t3k_pub_\xe2\x80\xa6) from "
    "tone3000.com \xe2\x80\xba Settings \xe2\x80\xba API keys.";

namespace {
std::string lower(std::string_view s) {
  std::string r(s);
  std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return r;
}
std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}
bool isSeparationModel(std::string_view s) { return s == "htdemucs_6s" || s == "htdemucs" || s == "htdemucs_ft"; }

// True if `p` is `dir` or lies inside it (lexical).
bool isInside(const fs::path& p, const fs::path& dir) {
  if (dir.empty() || p.empty()) return false;
  const fs::path a = p.lexically_normal(), b = dir.lexically_normal();
  auto ai = a.begin();
  for (auto bi = b.begin(); bi != b.end(); ++bi, ++ai) {
    if (bi->empty()) break;  // trailing separator
    if (ai == a.end() || *ai != *bi) return false;
  }
  return true;
}

bool validVenv(const Env& env, const fs::path& dir) { return env.exists && env.exists(dir / "bin" / "sawblade-t3k"); }
}  // namespace

bool containsSecretKey(std::string_view value) { return lower(value).find("t3k_cs_") != std::string::npos; }

namespace {
// Reads only the top-level "client_id" string of the sawblade-t3k token file. A SAX handler keeps that one
// value and drops everything else as it streams past, so no token field is ever held. Missing, unreadable
// or corrupt file, a non-string value, or a t3k_cs_ value: "".
struct ClientIdOnly final : nlohmann::json_sax<nlohmann::json> {
  using json = nlohmann::json;
  int depth = 0;
  bool wantNext = false;
  std::string id;
  bool null() override { wantNext = false; return true; }
  bool boolean(bool) override { wantNext = false; return true; }
  bool number_integer(json::number_integer_t) override { wantNext = false; return true; }
  bool number_unsigned(json::number_unsigned_t) override { wantNext = false; return true; }
  bool number_float(json::number_float_t, const std::string&) override { wantNext = false; return true; }
  bool string(std::string& v) override {
    if (wantNext && depth == 1) id = v;
    wantNext = false;
    return true;
  }
  bool binary(json::binary_t&) override { wantNext = false; return true; }
  bool start_object(std::size_t) override { ++depth; wantNext = false; return true; }
  bool end_object() override { --depth; return true; }
  bool start_array(std::size_t) override { ++depth; wantNext = false; return true; }
  bool end_array() override { --depth; return true; }
  bool key(std::string& k) override {
    wantNext = (depth == 1 && k == "client_id");
    return true;
  }
  bool parse_error(std::size_t, const std::string&, const json::exception&) override { return false; }
};

std::string readTokenFileClientId(const fs::path& file) {
  try {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size > (1u << 20)) return {};  // missing, or not a plausible token file
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    ClientIdOnly h;
    if (!nlohmann::json::sax_parse(in, &h)) return {};
    std::string id = trim(h.id);
    if (id.empty() || containsSecretKey(id)) return {};
    return id;
  } catch (...) {
    return {};
  }
}
}  // namespace

// ---------------------------------------------------------------------------------------------
Env Env::system() {
  Env e;
  e.getenv = [](std::string_view name) -> std::optional<std::string> {
    const std::string n(name);
    const char* v = std::getenv(n.c_str());
    if (v == nullptr) return std::nullopt;
    return std::string(v);
  };
  const char* h = std::getenv("HOME");
  e.home = (h != nullptr && *h != '\0') ? fs::path(h) : fs::path(juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName().toStdString());
  e.executable = fs::path(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName().toStdString());
  e.sourceDir = fs::path(SAWBLADE_SOURCE_DIR);
  e.exists = [](const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
  };
#if defined(__APPLE__)
  e.isMac = true;
#endif
  return e;
}

std::optional<std::string> Env::var(std::string_view name) const {
  if (!getenv) return std::nullopt;
  auto v = getenv(name);
  if (!v || v->empty()) return std::nullopt;
  return v;
}

// Same rule as appDataDir() in AppPaths.h, through the injectable Env.
fs::path Paths::appDataDir(const Env& env) {
  if (auto v = env.var("SAWBLADE_APPDATA")) return fs::path(*v);
  if (auto v = env.var("SAWBLADE_DATA_DIR")) return fs::path(*v);
  if (env.isMac) return env.home / "Library" / "Application Support" / "Sawblade";
  if (auto x = env.var("XDG_DATA_HOME")) return fs::path(*x) / "sawblade";
  return env.home / ".local" / "share" / "sawblade";
}
// The same file as presets/T3kTool's settingsFile(); both stores keep each other's keys.
fs::path Paths::settingsFile(const Env& env) {
  if (auto v = env.var("SAWBLADE_SETTINGS_FILE")) return fs::path(*v);
  return appDataDir(env) / "settings.json";
}
fs::path Paths::tokenFile(const Env& env) {
  if (auto v = env.var("SAWBLADE_T3K_TOKEN_FILE")) return fs::path(*v);
  return env.home / ".config" / "sawblade" / "t3k_tokens.json";
}
fs::path Paths::captureCacheDir(const Env& env) {
  if (auto v = env.var("SAWBLADE_CACHE_DIR")) return fs::path(*v);
  return env.home / ".cache" / "sawblade" / "captures";
}
fs::path Paths::takesDir(const Env& env) { return appDataDir(env) / "takes"; }  // = defaultTakesDir() (TakeRecorder)

std::optional<fs::path> detectMatchVenv(const Env& env) {
  if (auto v = env.var("SAWBLADE_MATCH_VENV")) {
    const fs::path p(*v);
    if (validVenv(env, p)) return p;
  }
  if (!env.sourceDir.empty() && isInside(env.executable, env.sourceDir)) {
    const fs::path p = env.sourceDir / "match" / ".venv";
    if (validVenv(env, p)) return p;
  }
  const fs::path p = env.home / "sawblade" / "match" / ".venv";
  if (validVenv(env, p)) return p;
  return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
Settings::Settings(fs::path file, Env env) : file_(std::move(file)), env_(std::move(env)) {}

namespace {
std::mutex gSharedMutex;
std::unique_ptr<Settings> gShared;
}  // namespace

Settings& Settings::shared() {
  std::lock_guard<std::mutex> lk(gSharedMutex);
  if (!gShared) {
    const Env env = Env::system();
    gShared = std::make_unique<Settings>(Paths::settingsFile(env), env);
    gShared->applyProcessEnv_ = true;
    gShared->load();
  }
  return *gShared;
}

bool Settings::sharedExistsForTests() {
  std::lock_guard<std::mutex> lk(gSharedMutex);
  return gShared != nullptr;
}

void Settings::resetSharedForTests() {
  std::lock_guard<std::mutex> lk(gSharedMutex);
  gShared.reset();
}

bool Settings::fileExists() const {
  std::error_code ec;
  return fs::exists(file_, ec);
}

bool Settings::isFirstRun() const {
  std::lock_guard<std::mutex> lk(m_);
  return firstRun_;
}

void Settings::markFirstRunCompleted() {
  {
    std::lock_guard<std::mutex> lk(m_);
    doc_["firstRunCompleted"] = true;
    firstRun_ = false;
    saveLocked();
  }
  notify();
}

std::string Settings::load() {
  std::lock_guard<std::mutex> lk(m_);
  const bool existed = fileExists();
  if (!loadedOnce_) {
    loadedOnce_ = true;
    firstRun_ = !existed;
  }
  doc_ = nlohmann::json::object();
  loadError_.clear();
  if (!existed) {
    applyCacheEnv();
    return {};
  }
  std::string err;
  auto finish = [this](std::string e) {  // records the error text and applies the cache-dir environment on every exit path
    loadError_ = e;
    applyCacheEnv();
    return e;
  };
  std::ifstream in(file_, std::ios::binary);
  if (!in) return finish("Cannot read " + file_.string() + "; using defaults.");
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object())
    return finish("Settings file " + file_.string() + " is not valid JSON; using defaults (the next change overwrites it).");
  doc_ = std::move(j);
  if (auto it = doc_.find("tone3000ClientId"); it != doc_.end()) {
    if (!it->is_string() || containsSecretKey(it->get<std::string>())) {
      err = "The settings file contained a TONE3000 secret key (t3k_cs_...); it was ignored and will not be kept.";
      doc_.erase(it);
    }
  }
  return finish(err);
}

std::string Settings::saveLocked() {
  doc_["version"] = 1;
  {
    // Read-modify-write: another store (presets/T3kTool: t3kExecutable, factoryPresetDir) may have written keys since
    // we loaded. The file wins for keys we do not own; for our own keys the in-memory document wins (including removal).
    static const char* const kOwned[] = {"version",    "matchVenvDir", "captureCacheDir", "tone3000ClientId", "separationModel",
                                         "takesDir",   "theme",        "uiScale",         "firstRunCompleted"};
    std::ifstream in(file_, std::ios::binary);
    if (in) {
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      auto disk = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
      if (disk.is_object()) {
        for (const char* k : kOwned) {
          disk.erase(k);
          if (auto it = doc_.find(k); it != doc_.end()) disk[k] = *it;
        }
        for (auto it = doc_.begin(); it != doc_.end(); ++it)
          if (!disk.contains(it.key()) && std::find(std::begin(kOwned), std::end(kOwned), it.key()) == std::end(kOwned)) disk[it.key()] = it.value();
        doc_ = std::move(disk);
      }
    }
  }
  std::error_code ec;
  const fs::path dir = file_.parent_path();
  if (!dir.empty() && !fs::exists(dir, ec)) {
    fs::create_directories(dir, ec);
    if (ec) return "Cannot create " + dir.string() + ": " + ec.message();
#ifndef _WIN32
    ::chmod(dir.c_str(), 0700);
#endif
  }
  static std::atomic<unsigned> tmpCounter{0};  // unique per write: concurrent writers never share a tmp file
#ifndef _WIN32
  const fs::path tmp = file_.string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(tmpCounter.fetch_add(1));
#else
  const fs::path tmp = file_.string() + ".tmp." + std::to_string(tmpCounter.fetch_add(1));
#endif
  const std::string text = doc_.dump(2) + "\n";
#ifndef _WIN32
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return "Cannot write " + tmp.string();
  ::fchmod(fd, 0600);
  size_t off = 0;
  while (off < text.size()) {
    const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
    if (n <= 0) {
      ::close(fd);
      fs::remove(tmp, ec);
      return "Cannot write " + tmp.string();
    }
    off += static_cast<size_t>(n);
  }
  ::close(fd);
#else
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return "Cannot write " + tmp.string();
    out << text;
  }
#endif
  fs::rename(tmp, file_, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return "Cannot replace " + file_.string();
  }
  return {};
}

std::string Settings::loadError() const {
  std::lock_guard<std::mutex> lk(m_);
  return loadError_;
}

std::string Settings::save() {
  std::string e;
  {
    std::lock_guard<std::mutex> lk(m_);
    e = saveLocked();
  }
  notify();
  return e;
}

void Settings::addListener(Listener* l) {
  std::lock_guard<std::mutex> lk(m_);
  if (std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end()) listeners_.push_back(l);
}
void Settings::removeListener(Listener* l) {
  std::lock_guard<std::mutex> lk(m_);
  listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
}
void Settings::notify() {
  std::vector<Listener*> copy;
  {
    std::lock_guard<std::mutex> lk(m_);
    copy = listeners_;
  }
  for (auto* l : copy) l->settingsChanged();
}

Result Settings::finish(Result r) {
  {
    std::lock_guard<std::mutex> lk(m_);
    const std::string e = saveLocked();
    if (!e.empty()) {
      r.ok = false;
      r.error = e;
    }
  }
  notify();
  return r;
}

// --- getters ---------------------------------------------------------------------------------
std::optional<fs::path> Settings::pathKey(const char* key) const {
  auto it = doc_.find(key);
  if (it == doc_.end() || !it->is_string() || it->get<std::string>().empty()) return std::nullopt;
  return fs::path(it->get<std::string>());
}
std::optional<fs::path> Settings::matchVenvDir() const {
  std::lock_guard<std::mutex> lk(m_);
  return pathKey("matchVenvDir");
}
std::optional<fs::path> Settings::effectiveMatchVenvDir() const {
  std::lock_guard<std::mutex> lk(m_);
  if (auto p = pathKey("matchVenvDir")) return p;
  return detectMatchVenv(env_);
}
std::optional<fs::path> Settings::captureCacheDir() const {
  std::lock_guard<std::mutex> lk(m_);
  return pathKey("captureCacheDir");
}
fs::path Settings::effectiveCaptureCacheDir() const {
  std::lock_guard<std::mutex> lk(m_);
  if (auto p = pathKey("captureCacheDir")) return *p;
  return Paths::captureCacheDir(env_);
}
std::string Settings::tone3000ClientId() const {
  std::lock_guard<std::mutex> lk(m_);
  auto it = doc_.find("tone3000ClientId");
  return (it != doc_.end() && it->is_string()) ? it->get<std::string>() : std::string();
}
ClientIdResolution Settings::resolveTone3000ClientId() const {
  if (std::string s = tone3000ClientId(); !s.empty() && !containsSecretKey(s)) return {s, ClientIdSource::Stored};
  if (auto v = env_.var("TONE3000_CLIENT_ID")) {
    const std::string t = trim(*v);
    if (!t.empty() && !containsSecretKey(t)) return {t, ClientIdSource::Environment};
  }
  const std::string f = readTokenFileClientId(Paths::tokenFile(env_));
  if (!f.empty()) return {f, ClientIdSource::TokenFile};
  return {};
}
std::string Settings::effectiveTone3000ClientId() const { return resolveTone3000ClientId().id; }
std::string Settings::separationModel() const {
  std::lock_guard<std::mutex> lk(m_);
  auto it = doc_.find("separationModel");
  if (it != doc_.end() && it->is_string() && isSeparationModel(it->get<std::string>())) return it->get<std::string>();
  return kDefaultSeparationModel;
}
std::optional<fs::path> Settings::takesDir() const {
  std::lock_guard<std::mutex> lk(m_);
  return pathKey("takesDir");
}
fs::path Settings::effectiveTakesDir() const {
  std::lock_guard<std::mutex> lk(m_);
  if (auto p = pathKey("takesDir")) return *p;
  return Paths::takesDir(env_);
}
std::string Settings::theme() const { return "dark"; }
double Settings::uiScale() const {
  std::lock_guard<std::mutex> lk(m_);
  auto it = doc_.find("uiScale");
  if (it != doc_.end() && it->is_number()) {
    const double v = it->get<double>();
    if (std::isfinite(v)) return std::clamp(v, 0.5, 2.0);
  }
  return 1.0;
}
bool Settings::firstRunCompleted() const {
  std::lock_guard<std::mutex> lk(m_);
  auto it = doc_.find("firstRunCompleted");
  return it != doc_.end() && it->is_boolean() && it->get<bool>();
}

// --- setters ---------------------------------------------------------------------------------
Result Settings::setPathKey(const char* key, std::optional<fs::path> v) {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (v && !v->empty()) doc_[key] = v->string();
    else doc_.erase(key);
  }
  return finish({});
}
Result Settings::setMatchVenvDir(std::optional<fs::path> v) { return setPathKey("matchVenvDir", std::move(v)); }
Result Settings::setCaptureCacheDir(std::optional<fs::path> v) {
  Result r = setPathKey("captureCacheDir", std::move(v));
  {
    std::lock_guard<std::mutex> lk(m_);
    applyCacheEnv();
  }
  return r;
}

// The shared instance points the core's captureCacheRoot() at a stored captureCacheDir (an in-process override, no
// environment mutation), so the engine's cache fallback and the child tools (ToolRunner passes the effective dir) use
// one folder. Clearing the override returns to the environment / default. Caller holds m_.
void Settings::applyCacheEnv() {
  if (!applyProcessEnv_) return;
  auto it = doc_.find("captureCacheDir");
  if (it != doc_.end() && it->is_string() && !it->get<std::string>().empty()) sawblade::setCaptureCacheRootOverride(fs::path(it->get<std::string>()));
  else sawblade::setCaptureCacheRootOverride(std::nullopt);
}

Settings::~Settings() {
  std::lock_guard<std::mutex> lk(m_);
  doc_.erase("captureCacheDir");  // drops the core override
  applyCacheEnv();
}
Result Settings::setTakesDir(std::optional<fs::path> v) { return setPathKey("takesDir", std::move(v)); }

Result Settings::setTone3000ClientId(std::string v) {
  if (containsSecretKey(v)) return {false, kSecretKeyMessage, ""};  // never stored, written or logged
  v = trim(v);
  Result r;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (v.empty()) doc_.erase("tone3000ClientId");
    else doc_["tone3000ClientId"] = v;
  }
  if (!v.empty() && v.rfind("t3k_pub_", 0) != 0) r.warning = "does not look like a publishable key (t3k_pub_\xe2\x80\xa6)";
  return finish(r);
}

Result Settings::setSeparationModel(std::string v) {
  if (!isSeparationModel(v)) return {false, "separation model must be htdemucs_6s, htdemucs or htdemucs_ft", ""};
  {
    std::lock_guard<std::mutex> lk(m_);
    doc_["separationModel"] = v;
  }
  return finish({});
}

Result Settings::setTheme(std::string v) {
  if (v != "dark") return {false, "only the \"dark\" theme exists", ""};
  {
    std::lock_guard<std::mutex> lk(m_);
    doc_["theme"] = v;
  }
  return finish({});
}

Result Settings::setUiScale(double v) {
  if (!std::isfinite(v)) return {false, "uiScale must be a number", ""};
  {
    std::lock_guard<std::mutex> lk(m_);
    doc_["uiScale"] = std::clamp(v, 0.5, 2.0);
  }
  return finish({});
}

fs::path Settings::tokenFile() const { return Paths::tokenFile(env_); }

fs::path Settings::toolPath(std::string_view tool) const {
  auto v = effectiveMatchVenvDir();
  if (!v) return {};
  return *v / "bin" / std::string(tool);
}

}  // namespace sawblade::plugin::settings
