#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../AppPaths.h"  // appDataDir()

// Runs the Python `sawblade-t3k` tool as a child process (shared by the mic page's IR packs and the preset browser,
// docs/specs/phase9a_mic_page.md section 6). The message thread never blocks: the child runs on a background thread,
// progress comes from `--progress-json` stdout lines ({"done", "total", "name"}), cancel kills the child.
namespace sawblade::plugin {

// appDataDir() lives in AppPaths.h (SAWBLADE_APPDATA, SAWBLADE_DATA_DIR, platform default).
std::filesystem::path settingsFile();                // <appdata>/settings.json, shared with other features
std::filesystem::path packCacheDir();                // <appdata>/packs
std::filesystem::path packManifestPath(const std::string& toneId);  // <appdata>/packs/<toneId>.json

// settings.json: a JSON object shared by several features. Writes are read-modify-write and keep unknown keys; a
// file that is not a JSON object is left untouched (the write fails).
namespace settings {
std::filesystem::path defaultT3kExecutable();       // <repo>/match/.venv/bin/sawblade-t3k (compile definition)
std::filesystem::path t3kExecutable();              // key "t3kExecutable", else the default
std::filesystem::path factoryPresetDir();           // key "factoryPresetDir", else <repo>/presets (compile definition)
bool setT3kExecutable(const std::filesystem::path& exe, std::string* error = nullptr);
}  // namespace settings

class T3kTool {
 public:
  struct Progress {
    int done = 0, total = 0;
    std::string name;
  };
  enum class Status { Ok, NotLoggedIn, MissingExecutable, Failed, Cancelled };
  struct Result {
    Status status = Status::Failed;
    int exitCode = -1;
    std::string message;  // what to show the user ("" when Ok)
    std::string output;   // the child's non-progress output (stdout and stderr, merged)
  };
  using ProgressFn = std::function<void(const Progress&)>;
  using DoneFn = std::function<void(const Result&)>;

  static constexpr int kExitNotLoggedIn = 4;
  static std::string notLoggedInMessage();

  T3kTool();
  ~T3kTool();  // cancels a running child and joins
  T3kTool(const T3kTool&) = delete;
  T3kTool& operator=(const T3kTool&) = delete;

  // Runs `<executable> <args...>` on a background thread (the executable defaults to settings::t3kExecutable()).
  // `onProgress` and `onDone` are called on that thread (marshal to the message thread; do not call start() or destroy
  // the tool from them). False if a run is already active. stderr is merged into stdout (JUCE ChildProcess), so
  // Result::message for exit code 1 is the last non-progress line of the child's output.
  bool start(std::vector<std::string> args, ProgressFn onProgress, DoneFn onDone, std::filesystem::path executable = {});
  void cancel();  // kills the child; onDone then gets Status::Cancelled
  bool running() const { return running_.load(); }

  // `pack <toneId> -o <manifest> --progress-json`
  static std::vector<std::string> packArgs(const std::string& toneId, const std::filesystem::path& manifest);

 private:
  struct Job;
  static Result runBlocking(const std::filesystem::path& exe, const std::vector<std::string>& args, const ProgressFn& onProgress, Job& job);

  std::mutex m_;
  std::shared_ptr<Job> job_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace sawblade::plugin
