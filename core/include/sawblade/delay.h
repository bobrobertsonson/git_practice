#pragma once

#include <vector>

#include "sawblade/processor.h"

namespace sawblade {

// Static gain. Applied instantly (no smoothing) so output is independent of block size.
class Gain : public Processor {
 public:
  void setGainDb(double db) noexcept;
  void setGainLinear(float g) noexcept { gain_ = g; }
  float gainLinear() const noexcept { return gain_; }

  void prepare(const ProcessSpec&) override {}
  void reset() noexcept override {}
  void process(float* io, int numSamples) noexcept override {
    const float g = gain_;
    for (int i = 0; i < numSamples; ++i) io[i] *= g;
  }

 private:
  float gain_ = 1.0f;
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
  void reset() noexcept override;
  void process(float* io, int numSamples) noexcept override;

 private:
  std::vector<float> buf_;  // size max_ + 1 after prepare()
  int max_ = 0;
  int delay_ = 0;
  int write_ = 0;
};

}  // namespace sawblade
