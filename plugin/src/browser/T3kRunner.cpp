#include "T3kRunner.h"

#include <algorithm>

namespace sawblade::plugin {
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
  } else {
    juce::StringArray argv;
    argv.add(juce::String(exe));
    for (const auto& a : job.args) argv.add(juce::String(a));
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
      std::string pending;
      char buf[4096];
      for (;;) {
        const int n = child.readProcessOutput(buf, sizeof buf);
        if (n <= 0) break;
        if (job.keepOutput && res.output.size() < kMaxOutput) res.output.append(buf, static_cast<std::size_t>(n));
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
      child.waitForProcessToFinish(2000);
      {
        std::lock_guard<std::mutex> lk(procM_);
        child_ = nullptr;
        res.timedOut = timedOut_;
        res.cancelled = cancel_ && !timedOut_;
      }
      res.exitCode = static_cast<int>(child.getExitCode());
      if (child.isRunning()) child.kill();
    }
  }
  if (!alive_->load() || !job.onDone) return;
  juce::MessageManager::callAsync([alive = alive_, cb = std::move(job.onDone), res = std::move(res)]() mutable {
    if (alive->load()) cb(std::move(res));
  });
}

}  // namespace sawblade::plugin
