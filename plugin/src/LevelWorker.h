#pragma once

// Background thread of level-matched auditioning (v0.3 Task B; docs/PRESET_SCHEMA.md "Level matching"): the renders of the
// reference DI that give a preset's auto trim and a capture swap's make-up. JUCE-free. Nothing here runs on the audio thread;
// the results are handed to the audio thread by the processor (an atomic trim target).
//
// Two kinds of work share one thread and one capture cache: trim jobs (latest wins: a new rig supersedes a queued one, the one
// being measured finishes and its result is dropped by the caller when stale) and make-up jobs (first in, first out, always
// before a trim job). Callbacks run on the worker thread.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "sawblade/capture_cache.h"
#include "sawblade/preset.h"

namespace sawblade::plugin {

class LevelWorker {
 public:
  struct TrimResult {
    std::string hash;               // autoTrimHash of the preset that was measured
    std::optional<double> trimDb;   // none: could not be measured (silent, capture missing, ...)
    double lufs = 0.0;              // measured without the trim
    std::string error;              // why not ("" when measured)
  };
  struct MakeupResult {
    std::optional<double> makeupDb;
    std::string error;
  };
  using TrimDone = std::function<void(const TrimResult&)>;
  using MakeupDone = std::function<void(const MakeupResult&)>;

  LevelWorker();
  ~LevelWorker();  // joins; callbacks of work still queued are not called
  LevelWorker(const LevelWorker&) = delete;
  LevelWorker& operator=(const LevelWorker&) = delete;

  // The auto trim of `p` (its measurement preset: see SawbladeProcessor::levelMeasurementPreset). `hash` is echoed.
  void submitTrim(Preset p, std::string hash, TrimDone done);
  // The make-up for block `block` of path `path` (0 = a, 1 = b) when `before` becomes `after` (core auto_trim.h slotMakeupDb).
  void submitMakeup(Preset before, Preset after, int path, MakeupDone done);

  // Nothing queued and nothing running.
  bool idle() const;
  bool waitIdle(std::chrono::milliseconds timeout) const;
  std::uint64_t trimJobsRun() const noexcept;     // finished trim measurements (tests)
  std::uint64_t makeupJobsRun() const noexcept;

 private:
  struct TrimJob {
    Preset preset;
    std::string hash;
    TrimDone done;
  };
  struct MakeupJob {
    Preset before, after;
    int path = 0;
    MakeupDone done;
  };
  void run();

  mutable std::mutex m_;
  mutable std::condition_variable cv_;
  std::optional<TrimJob> trim_;
  std::deque<MakeupJob> makeups_;
  bool busy_ = false, stop_ = false;
  std::uint64_t trimRuns_ = 0, makeupRuns_ = 0;
  CaptureCache cache_;  // worker thread only
  std::thread thread_;  // last
};

}  // namespace sawblade::plugin
