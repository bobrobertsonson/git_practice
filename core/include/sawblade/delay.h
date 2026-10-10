#pragma once

#include <vector>

#include "sawblade/processor.h"

namespace sawblade {

// Gain. Static by default: applied instantly (no smoothing) so output is independent of block
// size. rampToLinear() starts a per-sample linear ramp (used for live parameter changes); the
// ramp is sample-accurate, so the output does not depend on block size.
class Gain : public Processor {
 public:
  void setGainDb(double db) noexcept;
  void setGainLinear(float g) noexcept {
    gain_ = g;
    remaining_ = 0;
  }
  float gainLinear() const noexcept { return gain_; }  // the target (== current when not ramping)

  // Ramps from the current value to `target` over `samples` samples (RT-safe; <= 0 jumps).
  void rampToLinear(float target, int samples) noexcept {
    if (samples <= 0) {
      setGainLinear(target);
      return;
    }
    if (remaining_ <= 0) cur_ = gain_;
    gain_ = target;
    step_ = (target - cur_) / static_cast<float>(samples);
    remaining_ = samples;
  }
  bool ramping() const noexcept { return remaining_ > 0; }

  void prepare(const ProcessSpec&) override {}
  void reset() override { remaining_ = 0; }
  void process(float* io, int numSamples) noexcept override {
    int i = 0;
    if (remaining_ > 0) {
      const int r = remaining_ < numSamples ? remaining_ : numSamples;
      float c = cur_;
      for (; i < r; ++i) {
        c += step_;
        io[i] *= c;
      }
      remaining_ -= r;
      cur_ = remaining_ == 0 ? gain_ : c;
    }
    const float g = gain_;
    for (; i < numSamples; ++i) io[i] *= g;
  }

 private:
  float gain_ = 1.0f;  // target
  float cur_ = 1.0f, step_ = 0.0f;
  int remaining_ = 0;
};

// Integer-sample delay. Set the maximum before prepare() (it sizes the buffer there); the
// delay itself can be changed at any time without allocation (clamped to [0, max]).
// Changing the delay while audio runs is not click-free; the caller owns that policy.
// latencySamples() stays 0: this block is deliberate delay, not processing latency.
class DelayLine : public Processor {
 public:
  explicit DelayLine(int maxDelaySamples = 0) noexcept : max_(maxDelaySamples < 0 ? 0 : maxDelaySamples) {}

  void setMaxDelaySamples(int maxDelay) noexcept { max_ = maxDelay < 0 ? 0 : maxDelay; }
  int maxDelaySamples() const noexcept { return max_; }

  void setDelaySamples(int d) noexcept { delay_ = d < 0 ? 0 : (d > max_ ? max_ : d); }
  int delaySamples() const noexcept { return delay_; }

  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;

 private:
  std::vector<float> buf_;  // size max_ + 1 after prepare()
  int max_ = 0;
  int delay_ = 0;
  int write_ = 0;
};

}  // namespace sawblade
