#pragma once

#include <vector>

#include "sawblade/adaa_clipper.h"
#include "sawblade/oversampler.h"
#include "sawblade/pedal_common.h"
#include "sawblade/pedal_params.h"
#include "sawblade/processor.h"

namespace sawblade {

// "Green overdrive" (pedal.ts): Tube-Screamer-style model. The clipped branch (720 Hz high-pass
// -> gain-dependent low-pass -> gain Rd/4.7k -> asymmetric soft clip, all at 4x) is summed with
// the clean signal (feedback-loop topology: the gain is never below unity), then tone (1st-order
// LPF), a 10 Hz DC-blocking high-pass (asymmetric clipping makes DC) and level at the base rate.
// Static, nonlinear, time-invariant: NAM-trainable.
// Latency: oversampler round trip + one ADAA2 sample, padded by one oversampled sample so the
// total is a whole number of base-rate samples. The clean signal is delayed by the ADAA sample so
// both branches stay aligned.
class TsPedal : public Processor {
 public:
  explicit TsPedal(const TsParams& p, PedalImplConfig cfg = {});
  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;
  int latencySamples() const noexcept override { return latency_; }

 private:
  TsParams p_;
  PedalImplConfig cfg_;
  int latency_ = 0;
  int pad_ = 0;
  float branchGain_ = 1.0f, levelGain_ = 1.0f;
  OnePole inHpf_, branchHpf_, branchLpf_, tone_, outHpf_;
  AdaaClipper clip_;
  ShortDelay cleanDelay_, padDelay_;
  Oversampler4x os_;
  std::vector<float> osBuf_, branchBuf_;
};

}  // namespace sawblade
