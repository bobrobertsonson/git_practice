#include "T3kRunner.h"

#include <algorithm>
#include <cctype>

#include "../settings/ToolEnv.h"

namespace sawblade::plugin {

std::string lastErrorLine(const std::string& output) {
  auto looksSecret = [](std::string l) {
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* k : {"token", "secret", "t3k_", "bearer", "password", "authorization"})
      if (l.find(k) != std::string::npos) return true;
    return false;
  };
  std::size_t end = output.size();
  while (end > 0) {
    const std::size_t nl = output.rfind('\n', end - 1);
    const std::size_t begin = nl == std::string::npos ? 0 : nl + 1;
    std::string line = output.substr(begin, end - begin);
    end = nl == std::string::npos ? 0 : nl;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line.front() == '{' || line.front() == '[' || looksSecret(line)) continue;
    if (line.size() > 300) line.resize(300);
    return line;
  }
  return {};
}

std::string lastErrorObjectLine(const std::string& output) {
  std::size_t end = output.size();
  while (end > 0) {
    const std::size_t nl = output.rfind('\n', end - 1);
    const std::size_t begin = nl == std::string::npos ? 0 : nl + 1;
    std::string line = output.substr(begin, end - begin);
    end = nl == std::string::npos ? 0 : nl;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty() && line.front() == '{' && line.find("\"error\"") != std::string::npos && line.find("t3k_cs_") == std::string::npos) return line;
  }
  return {};
}

namespace {
constexpr std::size_t kMaxOutput = 8u << 20;
using Clock = std::chrono::steady_clock;
}  // namespace

T3kRunner::T3kRunner(std::function<std::string()> executable) : executable_(std::move(executable)) {
  worker_ = std::thread([this] { run(); });
  dog_ = std::thread([this] { watchdog(); });
}

T3kRunner::~T3kRunner() {
  alive_->store(false);
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  {
    std::lock_guard<std::mutex> lk(procM_);
    cancel_ = true;
    dogStop_ = true;
    if (child_ != nullptr) child_->kill();
  }
  procCv_.notify_all();
  if (worker_.joinable()) worker_.join();
  if (dog_.joinable()) dog_.join();
}

void T3kRunner::submit(Job job) {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!job.supersedeKey.empty())
      queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Job& j) { return j.supersedeKey == job.supersedeKey; }), queue_.end());
    queue_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void T3kRunner::cancelAll() {
  {
    std::lock_guard<std::mutex> lk(m_);
    queue_.clear();
  }
  std::lock_guard<std::mutex> lk(procM_);
  cancel_ = true;
  if (child_ != nullptr) child_->kill();
}

void T3kRunner::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    execute(job);
  }
}

void T3kRunner::watchdog() {
  std::unique_lock<std::mutex> lk(procM_);
  while (!dogStop_) {
    if (child_ != nullptr && !timedOut_ && Clock::now() >= deadline_) {
      timedOut_ = true;
      child_->kill();
    }
    procCv_.wait_for(lk, std::chrono::milliseconds(25));
  }
}

void T3kRunner::execute(Job& job) {
  RunResult res;
  const std::string exe = executable_();
  const juce::File f(exe);
  if (exe.find('/') != std::string::npos && !f.existsAsFile()) {
    res.launchError = "not found: " + exe;
  } else if (const std::string problem = settings::toolPathProblem(exe); !problem.empty()) {
    res.launchError = problem;
  } else {
    juce::StringArray argv;
    // The same environment as every other tool launch (client id, cache dir): a DAW started from the Dock has no shell exports.
    for (const auto& a : settings::toolCommand(exe, job.args, settings::toolEnvironment())) argv.add(juce::String::fromUTF8(a.c_str()));
    juce::ChildProcess child;
    {
      std::lock_guard<std::mutex> lk(procM_);
      if (alive_->load()) cancel_ = false;
      timedOut_ = false;
    }
    if (!child.start(argv, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) {
      res.launchError = "could not start " + exe;
    } else {
      {
        std::lock_guard<std::mutex> lk(procM_);
        child_ = &child;
        deadline_ = Clock::now() + std::chrono::milliseconds(job.timeoutMs);
        if (cancel_ || !alive_->load()) child.kill();
      }
      procCv_.notify_all();
      res.launched = true;
      std::string pending, tailBuf;
      char buf[4096];
      for (;;) {
        const int n = child.readProcessOutput(buf, sizeof buf);
        if (n <= 0) break;
        if (job.keepOutput && res.output.size() < kMaxOutput) res.output.append(buf, static_cast<std::size_t>(n));
        tailBuf.append(buf, static_cast<std::size_t>(n));  // kept even when keepOutput is off: only a filtered last line leaves (lastErrorLine)
        if (tailBuf.size() > 4096) tailBuf.erase(0, tailBuf.size() - 2048);
        if (job.onWorkerLine || job.onLine) {
          pending.append(buf, static_cast<std::size_t>(n));
          std::size_t nl;
          while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (job.onWorkerLine) {
              if (const int t = job.onWorkerLine(line); t > 0) {
                std::lock_guard<std::mutex> lk(procM_);
                deadline_ = Clock::now() + std::chrono::milliseconds(t);
              }
            }
            if (job.onLine && alive_->load()) {
              juce::MessageManager::callAsync([alive = alive_, cb = job.onLine, line] {
                if (alive->load()) cb(line);
              });
            }
          }
        }
      }
      // Shutdown: the child is killed and not waited for (the destructor must not stall); otherwise the exit
      // code needs the wait (the pipe is at EOF, so the child is already gone or about to be).
      if (alive_->load()) child.waitForProcessToFinish(2000);
      else child.kill();
      {
        std::lock_guard<std::mutex> lk(procM_);
        child_ = nullptr;
        res.timedOut = timedOut_;
        res.cancelled = cancel_ && !timedOut_;
      }
      res.exitCode = static_cast<int>(child.getExitCode());
      res.lastLine = lastErrorLine(tailBuf);
      res.lastErrorObject = lastErrorObjectLine(tailBuf);
      if (child.isRunning()) child.kill();
    }
  }
  if (!alive_->load() || !job.onDone) return;
  juce::MessageManager::callAsync([alive = alive_, cb = std::move(job.onDone), res = std::move(res)]() mutable {
    if (alive->load()) cb(std::move(res));
  });
}

}  // namespace sawblade::plugin
