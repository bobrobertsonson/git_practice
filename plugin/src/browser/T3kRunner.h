#pragma once

// Runs `sawblade-t3k` as a juce::ChildProcess on its own worker thread (one thread, a FIFO of jobs).
// Never used from the audio thread; never blocks the message thread. Results are delivered on the message
// thread through MessageManager::callAsync, guarded by a flag the destructor clears, so a closed browser
// never gets a callback. stdout and stderr are merged (the JSON parser skips stderr notes; on a failure the
// tail is the error text).

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <juce_events/juce_events.h>

namespace sawblade::plugin {

struct RunResult {
  bool launched = false;   // the child could be started
  bool timedOut = false;
  bool cancelled = false;
  int exitCode = -1;
  std::string output;      // merged stdout + stderr (capped)
  std::string launchError;
  std::string lastErrorObject;  // the last {"error", "code"} line of the output, kept even when keepOutput is off
  std::string lastLine;   // the tool's last plain output line (stderr text such as "TONE3000_CLIENT_ID is not set"); never JSON, never secret-looking
};

// The last output line that is not JSON and does not look like a credential ("" if none). Bounded to 300 characters.
std::string lastErrorLine(const std::string& output);
// The last line that is a {"error": ...} JSON object ("" if none): the CLI's own error, preferred over the stderr line.
std::string lastErrorObjectLine(const std::string& output);

class T3kRunner {
 public:
  struct Job {
    std::vector<std::string> args;  // after the executable
    int timeoutMs = 30000;
    // Jobs with the same non-empty key supersede a queued (not yet running) older one: it is dropped, and its
    // callbacks never run.
    std::string supersedeKey;
    // Worker thread, per complete output line; returns a new timeout in ms from now (0 = unchanged).
    std::function<int(const std::string& line)> onWorkerLine;
    // Message thread, per complete output line (streaming jobs only, e.g. login).
    std::function<void(const std::string& line)> onLine;
    // Message thread, once. Not called for a superseded job or after the runner is destroyed.
    std::function<void(RunResult)> onDone;
    bool keepOutput = true;  // false: the full output is not kept (login: it may hold secrets)
  };

  explicit T3kRunner(std::function<std::string()> executable);
  ~T3kRunner();
  T3kRunner(const T3kRunner&) = delete;
  T3kRunner& operator=(const T3kRunner&) = delete;

  void submit(Job job);
  // Kills the running child (its job completes as cancelled) and drops everything queued.
  void cancelAll();

 private:
  void run();
  void watchdog();
  void execute(Job& job);

  std::function<std::string()> executable_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

  std::mutex m_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  bool stop_ = false;

  // The running child (guarded by procM_); the watchdog kills it at the deadline.
  std::mutex procM_;
  juce::ChildProcess* child_ = nullptr;
  std::chrono::steady_clock::time_point deadline_{};
  bool timedOut_ = false, cancel_ = false, dogStop_ = false;
  std::condition_variable procCv_;

  std::thread worker_, dog_;
};

}  // namespace sawblade::plugin
