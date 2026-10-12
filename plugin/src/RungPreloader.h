#pragma once

// Background loader of gain-ladder rung models (v0.2 Task B). One worker thread runs Engine::refreshRungs for the
// engine it was last asked about: the cached rung models nearest the knob's rung are loaded (the capture cache is the
// worker's own), handed to the audio thread through the block's SwapSlot, and the others are evicted. Requests coalesce
// (the latest engine wins). JUCE-free; never touches the audio thread.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "Engine.h"
#include "sawblade/capture_cache.h"

namespace sawblade::plugin {

class RungPreloader {
 public:
  RungPreloader();
  ~RungPreloader();
  RungPreloader(const RungPreloader&) = delete;
  RungPreloader& operator=(const RungPreloader&) = delete;

  void request(std::shared_ptr<Engine> engine);  // any non-audio thread
  bool waitIdle(std::chrono::milliseconds timeout);
  // Rungs wanted but not cached after the last refresh of the last engine (what Task D's UI calls "pending").
  int missingRungs() const { return missing_.load(); }

 private:
  void run();
  std::mutex m_;
  std::condition_variable cv_, idleCv_;
  std::shared_ptr<Engine> pending_;
  bool busy_ = false, stop_ = false;
  std::atomic<int> missing_{0};
  CaptureCache cache_;
  std::thread thread_;
};

}  // namespace sawblade::plugin
