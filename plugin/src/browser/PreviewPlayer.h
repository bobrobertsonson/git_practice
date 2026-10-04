#pragma once

// Audition player for the capture browser (docs/specs/phase8_capture_browser.md "Preview"). JUCE-free.
//
// A rendered preview (an immutable float buffer at the host rate) is handed to the audio thread through a
// SwapSlot; process() then cross-fades the rig output into the preview (10 ms linear), plays it dual-mono
// instead of the rig, and cross-fades back at the end or after stop(). The buffer ends up equal to the
// preview exactly once the fade-in is over. Retired buffers are freed on the producer side (start()/collect()),
// never on the audio thread. process() allocates nothing, takes no lock and does no I/O.
//
// Threads: start() / stop() / collect() from any non-audio threads (producers are serialised by an internal
// mutex the audio thread never touches); prepare() and process() from the host's threads as for any plugin.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "sawblade/swap_slot.h"

namespace sawblade::plugin {

class PreviewPlayer {
 public:
  static constexpr double kFadeSeconds = 0.010;

  PreviewPlayer() = default;
  PreviewPlayer(const PreviewPlayer&) = delete;
  PreviewPlayer& operator=(const PreviewPlayer&) = delete;

  // Not while process() runs (prepareToPlay).
  void prepare(double sampleRate) noexcept;

  // Plays `samples` (mono, at `sampleRate`; must equal the prepared rate or the buffer is ignored by the
  // audio thread), replacing a preview that is playing.
  void start(std::vector<float> samples, double sampleRate);
  // Fades the current preview out (no-op when idle); also cancels a start() the audio thread has not seen.
  void stop() noexcept;
  // Frees buffers the audio thread has replaced.
  void collect();

  bool playing() const noexcept { return playing_.load(std::memory_order_relaxed); }

  // Audio thread. Mixes the preview over `out` (which holds the rig output) in place.
  void process(float* const* out, int numChannels, int numSamples) noexcept;

 private:
  struct Buf {
    std::vector<float> samples;
    double rate = 0.0;
    std::uint64_t serial = 0;
    std::uint64_t stopGen = 0;  // value of stopGen_ when the buffer was published
  };

  std::mutex producerMutex_;  // start()/collect() only
  SwapSlot<Buf> slot_;
  std::uint64_t nextSerial_ = 1;       // producer side
  std::atomic<std::uint64_t> stopGen_{0};
  std::atomic<bool> playing_{false};

  // Audio-thread state.
  double rate_ = 48000.0;
  int fadeLen_ = 480;
  std::uint64_t curSerial_ = 0, seenStop_ = 0;
  std::size_t pos_ = 0;
  int fadeIn_ = 0;       // samples of fade-in done (fadeLen_ = finished)
  int fadeOut_ = -1;     // -1: not stopping; else samples of the stop fade done
  bool active_ = false;
};

}  // namespace sawblade::plugin
