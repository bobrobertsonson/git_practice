#include "T3kTool.h"
#include "../AppPaths.h"
#include "../settings/Settings.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sawblade::plugin {
namespace fs = std::filesystem;
using nlohmann::json;

// --- app data and settings ---------------------------------------------------------------------------------------
fs::path settingsFile() {
  if (const char* o = std::getenv("SAWBLADE_SETTINGS_FILE"); o != nullptr && *o != '\0') return fs::path(o);  // as settings::Paths::settingsFile
  return appDataDir() / "settings.json";
}
fs::path packCacheDir() { return appDataDir() / "packs"; }
fs::path packManifestPath(const std::string& toneId) {
  std::string safe;
  for (char c : toneId) safe.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_');
  return packCacheDir() / (safe + ".json");
}

namespace settings {
namespace {
// Reads settings.json: a missing file is an empty object; false if the file exists but is not a JSON object.
bool readSettings(json& out) {
  out = json::object();
  std::ifstream f(settingsFile(), std::ios::binary);
  if (!f) return true;
  std::ostringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  if (text.find_first_not_of(" \t\r\n") == std::string::npos) return true;
  json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return false;
  out = std::move(j);
  return true;
}
}  // namespace

fs::path defaultT3kExecutable() {
  if (auto& s = Settings::shared(); s.effectiveMatchVenvDir()) return s.toolPath("sawblade-t3k");  // Settings panel venv (phase 11)
#ifdef SAWBLADE_REPO_DIR
  return fs::path(SAWBLADE_REPO_DIR) / "match" / ".venv" / "bin" / "sawblade-t3k";
#else
  return fs::path("sawblade-t3k");
#endif
}

fs::path t3kExecutable() {
  json j;
  if (readSettings(j)) {
    const auto it = j.find("t3kExecutable");
    if (it != j.end() && it->is_string() && !it->get<std::string>().empty()) return fs::path(it->get<std::string>());
  }
  return defaultT3kExecutable();
}

fs::path factoryPresetDir() {
  json j;
  if (readSettings(j)) {
    const auto it = j.find("factoryPresetDir");
    if (it != j.end() && it->is_string() && !it->get<std::string>().empty()) return fs::path(it->get<std::string>());
  }
#ifdef SAWBLADE_REPO_DIR
  return fs::path(SAWBLADE_REPO_DIR) / "presets";
#else
  return fs::path("presets");
#endif
}

bool setT3kExecutable(const fs::path& exe, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  json j;
  if (!readSettings(j)) return fail("settings.json is not a JSON object; not overwritten: " + settingsFile().string());
  j["t3kExecutable"] = exe.string();
  std::error_code ec;
  const fs::path dir = settingsFile().parent_path();
  if (!dir.empty()) fs::create_directories(dir, ec);
  if (ec) return fail("cannot create " + dir.string() + ": " + ec.message());
  static std::atomic<unsigned> counter{0};
  const fs::path tmp = settingsFile().string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(counter.fetch_add(1));
  const std::string text = j.dump(2) + "\n";
#if !defined(_WIN32)
  {  // created 0600 from the start: the shared settings file stays private (the phase 11 store writes it 0600 too)
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return fail("cannot write " + tmp.string());
    size_t off = 0;
    while (off < text.size()) {
      const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
      if (n <= 0) {
        ::close(fd);
        fs::remove(tmp, ec);
        return fail("cannot write " + tmp.string());
      }
      off += static_cast<size_t>(n);
    }
    ::close(fd);
  }
#else
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return fail("cannot write " + tmp.string());
    o << text;
  }
#endif
  fs::rename(tmp, settingsFile(), ec);
  if (ec) return fail("cannot replace " + settingsFile().string() + ": " + ec.message());
  return true;
}
}  // namespace settings

// --- the runner ---------------------------------------------------------------------------------------------------
struct T3kTool::Job {
  std::mutex m;
  juce::ChildProcess* child = nullptr;  // set while the child runs; guarded by m
  std::atomic<bool> cancelled{false};
};

std::string T3kTool::notLoggedInMessage() {
  return "Not logged in to TONE3000. Run `sawblade-t3k login` in a terminal, then try again.";
}

std::vector<std::string> T3kTool::packArgs(const std::string& toneId, const fs::path& manifest) {
  return {"pack", toneId, "-o", manifest.string(), "--progress-json"};
}

namespace {

bool looksExecutable(const fs::path& p) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return false;
#if !defined(_WIN32)
  return ::access(p.c_str(), X_OK) == 0;
#else
  return true;
#endif
}

bool hasDirectory(const fs::path& p) { return p.has_parent_path(); }

// One complete output line: progress JSON is reported, anything else is kept as text.
void handleLine(const std::string& raw, const T3kTool::ProgressFn& onProgress, std::string& text, std::string& lastText) {
  std::string line = raw;
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  if (line.empty()) return;
  if (line.front() == '{') {
    const json j = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (j.is_object() && j.contains("done") && j.contains("total") && j["done"].is_number() && j["total"].is_number()) {
      T3kTool::Progress p;
      p.done = j["done"].get<int>();
      p.total = j["total"].get<int>();
      // `pack` lines carry "name", `resolve` lines carry "title" (and the JSON path in "capture").
      for (const char* k : {"name", "title"})
        if (p.name.empty() && j.contains(k) && j[k].is_string()) p.name = j[k].get<std::string>();
      if (onProgress) onProgress(p);
      return;
    }
  }
  text += line;
  text += '\n';
  lastText = line;
}

}  // namespace

T3kTool::Result T3kTool::runBlocking(const fs::path& exe, const std::vector<std::string>& args, const ProgressFn& onProgress, Job& job) {
  Result r;
  if (hasDirectory(exe) && !looksExecutable(exe)) {
    r.status = Status::MissingExecutable;
    r.message = "Cannot find the sawblade-t3k tool at " + exe.string() + ". Locate it, or set it up (match/README.md).";
    return r;
  }
  juce::StringArray cmd;
  cmd.add(juce::String(exe.string()));
  for (const auto& a : args) cmd.add(juce::String(juce::CharPointer_UTF8(a.c_str())));

  juce::ChildProcess child;
  if (!child.start(cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) {
    r.status = Status::MissingExecutable;
    r.message = "Cannot start " + exe.string() + ". Locate the sawblade-t3k tool, or set it up (match/README.md).";
    return r;
  }
  {
    std::lock_guard<std::mutex> lk(job.m);
    job.child = &child;
    if (job.cancelled.load()) child.kill();
  }
  std::string pending, text, lastText;
  // JUCE's ChildProcess::readProcessOutput is fread() on the pipe, which blocks until the whole request is filled (or EOF):
  // a larger buffer would hold every progress line back until the child exits. Read a byte at a time (stdio buffers the pipe).
  for (;;) {
    char c;
    if (child.readProcessOutput(&c, 1) <= 0) break;
    if (c == '\n') {
      handleLine(pending, onProgress, text, lastText);
      pending.clear();
    } else {
      pending.push_back(c);
    }
  }
  handleLine(pending, onProgress, text, lastText);
  child.waitForProcessToFinish(10000);
  const int code = static_cast<int>(child.getExitCode());
  {
    std::lock_guard<std::mutex> lk(job.m);
    job.child = nullptr;
  }
  r.exitCode = code;
  r.output = std::move(text);
  if (job.cancelled.load()) {
    r.status = Status::Cancelled;
    r.message = "Cancelled.";
  } else if (code == 0) {
    r.status = Status::Ok;
  } else if (code == kExitNotLoggedIn) {
    r.status = Status::NotLoggedIn;
    r.message = notLoggedInMessage();
  } else {
    r.status = Status::Failed;
    r.message = !lastText.empty() ? lastText : "sawblade-t3k failed (exit code " + std::to_string(code) + ").";
  }
  return r;
}

T3kTool::T3kTool() = default;

T3kTool::~T3kTool() {
  cancel();
  if (thread_.joinable()) thread_.join();
}

bool T3kTool::start(std::vector<std::string> args, ProgressFn onProgress, DoneFn onDone, fs::path executable) {
  std::lock_guard<std::mutex> lk(m_);
  if (running_.load()) return false;
  if (thread_.joinable()) thread_.join();  // the previous run has finished
  if (executable.empty()) executable = settings::t3kExecutable();
  auto job = std::make_shared<Job>();
  job_ = job;
  running_ = true;
  thread_ = std::thread([this, job, exe = std::move(executable), args = std::move(args), onProgress = std::move(onProgress),
                         onDone = std::move(onDone)]() {
    Result r;
    try {
      r = runBlocking(exe, args, onProgress, *job);
    } catch (const std::exception& e) {
      r.status = Status::Failed;
      r.message = e.what();
    }
    running_ = false;
    if (onDone) onDone(r);
  });
  return true;
}

void T3kTool::cancel() {
  std::shared_ptr<Job> job;
  {
    std::lock_guard<std::mutex> lk(m_);
    job = job_;
  }
  if (!job) return;
  job->cancelled = true;
  std::lock_guard<std::mutex> lk(job->m);
  if (job->child != nullptr) job->child->kill();
}

}  // namespace sawblade::plugin
