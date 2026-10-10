#include "ToolRunner.h"
#include "ToolEnv.h"

#include <algorithm>
#include <cctype>
#include <thread>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace sawblade::plugin::settings {
namespace fs = std::filesystem;

// A manual scan (no std::regex: its recursion can blow the stack on a very long token line). Case-insensitive.
std::string redactSecrets(const std::string& line) {
  static const char kMarker[] = "t3k_cs_";
  constexpr size_t kLen = sizeof(kMarker) - 1;
  auto isTokenChar = [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; };
  auto markerAt = [&](size_t i) {
    if (i + kLen > line.size()) return false;
    for (size_t k = 0; k < kLen; ++k)
      if (std::tolower(static_cast<unsigned char>(line[i + k])) != kMarker[k]) return false;
    return true;
  };
  std::string out;
  size_t i = 0;
  while (i < line.size()) {
    if (markerAt(i) && i + kLen < line.size() && isTokenChar(static_cast<unsigned char>(line[i + kLen]))) {
      size_t j = i + kLen;
      while (j < line.size() && isTokenChar(static_cast<unsigned char>(line[j]))) ++j;
      out += "t3k_cs_[redacted]";
      i = j;
    } else {
      out.push_back(line[i++]);
    }
  }
  return out;
}

std::optional<nlohmann::json> extractJson(const std::vector<std::string>& lines) {
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    if (it->empty()) continue;
    const char c = (*it)[0];
    if (c != '{' && c != '[') continue;
    auto j = nlohmann::json::parse(*it, nullptr, false);
    if (!j.is_discarded() && (j.is_object() || j.is_array())) return j;
  }
  std::string all;
  for (size_t i = 0; i < lines.size(); ++i) all += (i ? "\n" : "") + lines[i];
  auto j = nlohmann::json::parse(all, nullptr, false);
  if (!j.is_discarded() && (j.is_object() || j.is_array())) return j;
  return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
ToolRunner::ToolRunner(Settings& settings) : settings_(settings) {}

ToolRunner::~ToolRunner() {
  alive_->store(false);
  std::vector<std::shared_ptr<Job>> jobs;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& w : jobs_)
      if (auto j = w.lock()) jobs.push_back(std::move(j));
  }
  for (auto& j : jobs) j->cancel();
  for (auto& j : jobs) j->wait(std::chrono::milliseconds(2000));
}

fs::path ToolRunner::resolve(std::string_view tool, std::string* error) const {
  const Env& env = settings_.env();
  const fs::path inVenv = settings_.toolPath(tool);
  if (!inVenv.empty() && env.exists && env.exists(inVenv)) return inVenv;
  if (auto path = env.getenv ? env.getenv("PATH") : std::nullopt) {
    size_t pos = 0;
    const std::string& s = *path;
    while (pos <= s.size()) {
      size_t e = s.find(':', pos);
      if (e == std::string::npos) e = s.size();
      const std::string dir = s.substr(pos, e - pos);
      if (!dir.empty()) {
        const fs::path cand = fs::path(dir) / std::string(tool);
        if (env.exists && env.exists(cand)) return cand;
      }
      pos = e + 1;
    }
  }
  if (error != nullptr) *error = std::string(tool) + " not found. Set the match venv in Settings (gear icon).";
  return {};
}

std::shared_ptr<ToolRunner::Job> ToolRunner::run(ToolRequest req, std::function<void(const std::string&)> onLine,
                                                 std::function<void(const ToolResult&)> onDone) {
  auto job = std::make_shared<Job>();
  job->timeout_ = req.timeout;
  job->onMessageThread_ = req.callbacksOnMessageThread;
  job->onLine_ = std::move(onLine);
  job->onDone_ = std::move(onDone);
  job->runnerAlive_ = alive_;

  std::string err;
  fs::path exe;
  if (req.executable) {
    exe = *req.executable;
    const Env& env = settings_.env();
    if (!(env.exists && env.exists(exe))) err = "cannot run " + exe.string() + ": file not found. Check the path in Settings (gear icon).";
  } else {
    exe = resolve(req.tool, &err);
  }
  job->result_.executable = exe;
  if (!err.empty()) {
    job->result_.error = err;
  } else {
    ToolEnvMap env = toolEnvironment(settings_);  // the one shared rule (ToolEnv.h)
    for (auto& kv : req.env) env[kv.first] = kv.second;
    if (const std::string problem = toolPathProblem(exe); !problem.empty()) job->result_.error = problem;
    else job->command_ = toolCommand(exe, req.args, env);
  }

  {
    std::lock_guard<std::mutex> lk(m_);
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const std::weak_ptr<Job>& w) { return w.expired(); }), jobs_.end());
    jobs_.push_back(job);
  }
  std::thread([job] { job->runWorker(); }).detach();
  if (job->timeout_.count() > 0 && !job->command_.empty()) std::thread([job] { job->runWatchdog(); }).detach();
  return job;
}

// ---------------------------------------------------------------------------------------------
void ToolRunner::Job::cancel() {
  cancelled_.store(true);
  std::lock_guard<std::mutex> lk(m_);
  if (child_ && !done_ && !reaped_) child_->kill();  // never signal a reaped (possibly reused) pid
}

bool ToolRunner::Job::isRunning() const {
  std::lock_guard<std::mutex> lk(m_);
  return !done_;
}

bool ToolRunner::Job::wait(std::chrono::milliseconds t) {
  std::unique_lock<std::mutex> lk(m_);
  return cv_.wait_for(lk, t, [this] { return done_; });
}

const ToolResult& ToolRunner::Job::result() const { return result_; }

void ToolRunner::Job::deliver(std::function<void()> fn) {
  if (!onMessageThread_) {
    fn();
    return;
  }
  auto alive = runnerAlive_;
  juce::MessageManager::callAsync([alive, fn = std::move(fn)] {
    if (alive->load()) fn();
  });
}

void ToolRunner::Job::finishLine(std::string line) {
  while (!line.empty() && line.back() == '\r') line.pop_back();
  line = redactSecrets(line);
  result_.lines.push_back(line);
  if (onLine_) {
    auto cb = onLine_;
    deliver([cb, line] { cb(line); });
  }
}

void ToolRunner::Job::runWatchdog() {
  std::unique_lock<std::mutex> lk(m_);
  if (cv_.wait_for(lk, timeout_, [this] { return done_; })) return;
  timedOut_.store(true);
  if (child_ && !reaped_) child_->kill();
}

void ToolRunner::Job::runWorker() {
  if (command_.empty()) {
    result_.outcome = ToolResult::Outcome::StartFailed;
  } else {
    juce::StringArray args;
    for (auto& a : command_) args.add(juce::String::fromUTF8(a.c_str()));
    auto proc = std::make_unique<juce::ChildProcess>();
    const bool started = proc->start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr);
    if (!started) {
      result_.outcome = ToolResult::Outcome::StartFailed;
      result_.error = "could not start " + result_.executable.string();
    } else {
      juce::ChildProcess* p = proc.get();
      {
        std::lock_guard<std::mutex> lk(m_);
        child_ = std::move(proc);
        if (cancelled_.load() || timedOut_.load()) p->kill();
      }
      // JUCE reads through a FILE*: a block read would wait for the whole block, so read byte by byte
      // (stdio buffers underneath) to deliver each line as soon as it is complete.
      std::string cur;
      char c;
      while (p->readProcessOutput(&c, 1) == 1) {
        if (c == '\n') {
          finishLine(std::move(cur));
          cur.clear();
        } else {
          cur.push_back(c);
        }
      }
      if (!cur.empty()) finishLine(std::move(cur));
      // JUCE quirk: isRunning() after the child has been reaped reports exit code 0 (waitpid fails and the
      // zeroed status reads as "exited 0"), so it must not be called again once it has returned false.
      if (!p->waitForProcessToFinish(5000)) {
        p->kill();
        p->waitForProcessToFinish(2000);
      }
      {
        std::lock_guard<std::mutex> lk(m_);
        reaped_ = true;  // from here cancel() and the watchdog must not kill()
      }
      result_.exitCode = static_cast<int>(p->getExitCode());
      if (cancelled_.load()) result_.outcome = ToolResult::Outcome::Cancelled;
      else if (timedOut_.load()) {
        result_.outcome = ToolResult::Outcome::TimedOut;
        result_.error = "timed out after " + std::to_string(timeout_.count()) + " ms";
      } else result_.outcome = result_.exitCode == 0 ? ToolResult::Outcome::Ok : ToolResult::Outcome::NonZeroExit;
    }
  }
  for (size_t i = 0; i < result_.lines.size(); ++i) result_.text += (i ? "\n" : "") + result_.lines[i];
  result_.json = extractJson(result_.lines);

  // Deliver onDone (after the last onLine), then publish "done". Callbacks are copied out: the Job may
  // be released by the owner as soon as done_ is set.
  const ToolResult copy = result_;
  auto cb = onDone_;
  if (cb) {
    if (onMessageThread_) {
      auto alive = runnerAlive_;
      juce::MessageManager::callAsync([alive, cb, copy] {
        if (alive->load()) cb(copy);
      });
    } else {
      cb(copy);
    }
  }
  std::unique_ptr<juce::ChildProcess> dead;
  {
    std::lock_guard<std::mutex> lk(m_);
    dead = std::move(child_);
    done_ = true;
    // Notify while holding the lock: a waiter may release the Job (destroying cv_ and m_) the moment it sees done_.
    cv_.notify_all();
  }
}

}  // namespace sawblade::plugin::settings
