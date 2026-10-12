#include "LevelWorker.h"

#include <chrono>
#include <exception>

#include "sawblade/auto_trim.h"

namespace sawblade::plugin {

LevelWorker::LevelWorker() { thread_ = std::thread([this] { run(); }); }

LevelWorker::~LevelWorker() {
  cancel_.store(true);
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
    trim_.reset();
    makeups_.clear();
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void LevelWorker::submitTrim(Preset p, std::string hash, TrimDone done) {
  {
    std::lock_guard<std::mutex> lk(m_);
    trim_ = TrimJob{std::move(p), std::move(hash), std::move(done)};
  }
  cv_.notify_all();
}

void LevelWorker::submitMakeup(Preset before, Preset after, int path, MakeupDone done) {
  {
    std::lock_guard<std::mutex> lk(m_);
    makeups_.push_back(MakeupJob{std::move(before), std::move(after), path, std::move(done)});
  }
  cv_.notify_all();
}

bool LevelWorker::idle() const {
  std::lock_guard<std::mutex> lk(m_);
  return !busy_ && !trim_ && makeups_.empty();
}

bool LevelWorker::waitIdle(std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lk(m_);
  return cv_.wait_for(lk, timeout, [&] { return !busy_ && !trim_ && makeups_.empty(); });
}

std::uint64_t LevelWorker::trimJobsRun() const noexcept {
  std::lock_guard<std::mutex> lk(m_);
  return trimRuns_;
}
std::uint64_t LevelWorker::makeupJobsRun() const noexcept {
  std::lock_guard<std::mutex> lk(m_);
  return makeupRuns_;
}

void LevelWorker::run() {
  for (;;) {
    std::optional<TrimJob> trim;
    std::optional<MakeupJob> makeup;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [&] { return stop_ || trim_ || !makeups_.empty(); });
      if (stop_) return;
      busy_ = true;
      if (!makeups_.empty()) {
        makeup = std::move(makeups_.front());
        makeups_.pop_front();
      } else {
        trim = std::move(trim_);
        trim_.reset();
      }
    }
    if (makeup) {
      MakeupResult r;
      try {
        r.makeupDb = slotMakeupDb(makeup->before, makeup->after, makeup->path, &cache_, &cancel_);
        if (!r.makeupDb) r.error = "the slot's path is silent or disabled";
      } catch (const std::exception& e) {
        r.error = e.what();
      }
      if (makeup->done && !cancel_.load()) makeup->done(r);
    } else if (trim) {
      TrimResult r;
      r.hash = trim->hash;
      try {
        if (const auto t = computeAutoTrim(trim->preset, &cache_, &cancel_)) {
          r.trimDb = t->trimDb;
          r.lufs = t->lufs;
          r.hash = t->hash;
        } else {
          r.error = "the reference DI renders silent through this preset";
        }
      } catch (const std::exception& e) {
        r.error = e.what();
      }
      if (trim->done && !cancel_.load()) trim->done(r);
    }
    {
      std::lock_guard<std::mutex> lk(m_);
      busy_ = false;
      if (makeup) ++makeupRuns_;
      else ++trimRuns_;
    }
    cv_.notify_all();
  }
}

}  // namespace sawblade::plugin
