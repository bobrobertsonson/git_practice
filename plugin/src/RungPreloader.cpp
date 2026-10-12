#include "RungPreloader.h"

namespace sawblade::plugin {

RungPreloader::RungPreloader() { thread_ = std::thread([this] { run(); }); }

RungPreloader::~RungPreloader() {
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void RungPreloader::request(std::shared_ptr<Engine> engine) {
  {
    std::lock_guard<std::mutex> lk(m_);
    pending_ = std::move(engine);
  }
  cv_.notify_all();
}

bool RungPreloader::waitIdle(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(m_);
  return idleCv_.wait_for(lk, timeout, [&] { return !pending_ && !busy_; });
}

void RungPreloader::run() {
  for (;;) {
    std::shared_ptr<Engine> e;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [&] { return stop_ || pending_ != nullptr; });
      if (stop_) return;
      e = std::move(pending_);
      busy_ = true;
    }
    try {
      missing_.store(e->refreshRungs(&cache_));
    } catch (...) {
    }
    e.reset();
    {
      std::lock_guard<std::mutex> lk(m_);
      busy_ = false;
    }
    idleCv_.notify_all();
  }
}

}  // namespace sawblade::plugin
