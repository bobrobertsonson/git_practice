#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Settings.h"

namespace juce {
class ChildProcess;
}

// Phase 11 (docs/specs/phase11_settings.md section 2): the one child-process helper for every
// feature that runs a `sawblade-*` tool. Not real-time; never used from the audio thread.
namespace sawblade::plugin::settings {

struct ToolRequest {
  std::string tool;  // "sawblade-t3k", "sawblade-match", ...
  std::vector<std::string> args;
  std::map<std::string, std::string> env;           // added to the child's environment (wins over ToolRunner's own)
  std::optional<std::filesystem::path> executable;  // bypass resolution (tests, "Locate...")
  std::chrono::milliseconds timeout{0};             // 0 = none
  bool callbacksOnMessageThread = true;             // false: callbacks run on the worker thread (headless tests)
};

struct ToolResult {
  enum class Outcome { Ok, NonZeroExit, StartFailed, Cancelled, TimedOut };
  Outcome outcome = Outcome::StartFailed;
  int exitCode = -1;
  std::string error;                  // human text for StartFailed / TimedOut
  std::vector<std::string> lines;     // every complete line, stdout and stderr merged, redacted
  std::string text;                   // lines joined with '\n'
  std::optional<nlohmann::json> json;  // the last line that is a JSON object/array, else the whole text
  std::filesystem::path executable;   // what was run
};

// Replaces every t3k_cs_[A-Za-z0-9_-]+ with t3k_cs_[redacted].
std::string redactSecrets(const std::string& line);
// See ToolResult::json. Never throws.
std::optional<nlohmann::json> extractJson(const std::vector<std::string>& lines);

class ToolRunner {
 public:
  explicit ToolRunner(Settings& settings);
  ~ToolRunner();  // cancels and joins every job it started
  ToolRunner(const ToolRunner&) = delete;
  ToolRunner& operator=(const ToolRunner&) = delete;

  // request.executable is not considered here. Resolution: settings.toolPath(tool) if it exists,
  // else the first match on PATH (read through Settings::env()), else "" and *error.
  std::filesystem::path resolve(std::string_view tool, std::string* error) const;

  class Job {
   public:
    void cancel();  // any thread; kills the child, result.outcome = Cancelled
    bool isRunning() const;
    bool wait(std::chrono::milliseconds);  // true once finished
    const ToolResult& result() const;      // valid once !isRunning()

   private:
    friend class ToolRunner;
    void runWorker();
    void runWatchdog();
    void finishLine(std::string line);
    void deliver(std::function<void()> fn);

    mutable std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false, reaped_ = false;  // reaped_: the child has been waited for; its pid must not be signalled any more
    std::atomic<bool> cancelled_{false}, timedOut_{false};
    std::unique_ptr<juce::ChildProcess> child_;
    ToolResult result_;
    std::chrono::milliseconds timeout_{0};
    bool onMessageThread_ = true;
    std::vector<std::string> command_;  // empty -> StartFailed with result_.error already set
    std::function<void(const std::string&)> onLine_;
    std::function<void(const ToolResult&)> onDone_;
    std::shared_ptr<std::atomic<bool>> runnerAlive_;
  };

  std::shared_ptr<Job> run(ToolRequest, std::function<void(const std::string& line)> onLine, std::function<void(const ToolResult&)> onDone);

 private:
  Settings& settings_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::mutex m_;
  std::vector<std::weak_ptr<Job>> jobs_;
};

}  // namespace sawblade::plugin::settings
