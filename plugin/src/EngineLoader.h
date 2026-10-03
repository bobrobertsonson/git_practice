#pragma once

// Background builder for Engines. One worker thread turns Requests (preset + host rate + block
// size) into prepared Engines and publishes them through a SwapSlot (lock-free hand-over to the
// audio thread); engines the audio thread has replaced are destroyed here, never on the audio
// thread. No audio-thread code ever touches the mutex below.
//
// Latest request wins: a request that arrives while another is being built supersedes it (the
// stale result is discarded). Every request is a complete specification, so this is always valid.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <memory>
#include <thread>
#include <vector>

#include "Engine.h"
#include "sawblade/swap_slot.h"

namespace sawblade::plugin {

// What travels through the SwapSlot. The audio thread copies the shared_ptr when it adopts an
// engine so it can keep the outgoing engine alive for the cross-fade; the loader keeps its own
// reference to every engine it published and only frees one once nothing else holds it, so the
// last reference is never dropped on the audio thread.
struct EngineRef {
  std::shared_ptr<Engine> engine;
};

class EngineLoader {
 public:
  struct Request {
    Preset preset;
    double hostRate = 48000.0;
    int maxBlock = 512;
    // If the build fails, publish a pass-through engine at this rate instead of keeping the
    // previous engine (used when the host rate changed: the old engine would be at the wrong rate).
    bool fallbackToInit = false;
    // Identifies the user-requested preset this build is for (null for rebuilds of the current
    // one); echoed in the Outcome.
    std::shared_ptr<const Preset> wanted;
    // Runs on the worker thread right before a successfully built engine is published (not for
    // the fallback engine): the processor commits the preset and its parameter values here, so
    // nothing changes if the build fails.
    std::function<void()> beforePublish;
  };

  struct Outcome {
    std::uint64_t id = 0;
    bool built = false;       // the requested preset was built and published
    bool published = false;   // an engine (requested or fallback) was published
    bool superseded = false;  // a newer request replaced this one; nothing published
    std::string error;        // why the build failed ("" if it did not)
    int latencySamples = 0;   // of the published engine (host-rate samples)
    double hostRate = 0.0, modelRate = 0.0;
    int maxBlock = 0;
    ChainInfo info;
    std::string presetName;
    std::shared_ptr<const Preset> wanted;
  };
  // Called on the worker thread after the outcome is known (and after publishing).
  using Callback = std::function<void(const Outcome&)>;

  EngineLoader(SwapSlot<EngineRef>& slot, Callback onOutcome);
  ~EngineLoader();
  EngineLoader(const EngineLoader&) = delete;
  EngineLoader& operator=(const EngineLoader&) = delete;

  // Any thread except the audio thread. Returns the request's id.
  std::uint64_t submit(Request r);

  // Blocks until request `id` (or a later one) has completed or the timeout expires.
  bool waitFor(std::uint64_t id, std::chrono::milliseconds timeout);
  // Blocks until nothing is queued or being built.
  bool waitIdle(std::chrono::milliseconds timeout);

  std::uint64_t engineBuilds() const noexcept { return builds_.load(); }  // engines published so far

 private:
  void run();
  static Request initRequest(const Request& like);

  SwapSlot<EngineRef>& slot_;
  Callback callback_;
  std::mutex m_;
  std::condition_variable cv_, doneCv_;
  std::optional<Request> pending_;
  std::uint64_t pendingId_ = 0, nextId_ = 0, doneId_ = 0;
  bool busy_ = false, stop_ = false;
  std::atomic<std::uint64_t> builds_{0};
  std::vector<std::shared_ptr<Engine>> owned_;  // worker thread only; see EngineRef
  void collect();
  std::thread thread_;  // last: started after everything above is constructed
};

}  // namespace sawblade::plugin
