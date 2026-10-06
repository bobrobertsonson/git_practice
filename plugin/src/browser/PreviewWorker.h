#pragma once

// The preview render worker of the capture browser: one thread, latest job wins. It lives in a shared_ptr that
// outlives the BrowserController: closing the browser never waits for a render (renderPreset has no cancel
// hook). The controller cancels, retire()s the worker, and a cleanup thread joins it once the render in flight
// is over. The worker never touches the controller: a finished result is delivered with callAsync, guarded by
// the job's `alive` flag, and dropped if the worker was cancelled.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "sawblade/capture_cache.h"
#include "sawblade/preset.h"
#include "sawblade/wav_io.h"

namespace sawblade::plugin {

class PreviewWorker {
 public:
  using RenderFn = std::function<std::vector<float>(const Preset&, double hostRate, std::string& error)>;
  // v0.3 LEVEL MATCH: the worker computes the slot's make-up (loudness of the slot's path alone on the reference DI before the swap
  // minus after it) and the candidate's auto trim, and renders the preview at that level (PreviewRender.h). Only for the default render.
  struct LevelMatch {
    bool on = false;
    Preset before;      // the rig as it is now (old capture, its make-up)
    int path = -1;      // the swapped nam block (path 0 = a, 1 = b; -1 = an IR: no make-up, the trim still applies)
    int block = -1;
  };
  struct Job {
    LevelMatch levelMatch;
    Preset preset;
    double hostRate = 48000.0;
    std::shared_ptr<std::atomic<bool>> alive;  // cleared by the owner on destruction (message thread)
    RenderFn render;                           // empty: renderPreview with the embedded riff
    std::function<void(std::vector<float>, std::string)> onDone;  // message thread
  };

  PreviewWorker();
  ~PreviewWorker();  // joins: call only from retire()'s cleanup thread or when idle
  PreviewWorker(const PreviewWorker&) = delete;
  PreviewWorker& operator=(const PreviewWorker&) = delete;

  void submit(Job job);
  // Drops the queued job and discards the result of the one in flight. Never blocks.
  void cancel();

  // Cancels and hands the worker to a cleanup thread (never blocks the caller); static cleanup joins any still
  // running at process exit.
  static void retire(std::shared_ptr<PreviewWorker> w);

 private:
  void run();

  CaptureCache cache_;
  AudioFile riff_;
  std::mutex m_;
  std::condition_variable cv_;
  std::optional<Job> pending_;
  bool stop_ = false;
  std::atomic<bool> cancelled_{false};
  std::thread thread_;
};

}  // namespace sawblade::plugin
