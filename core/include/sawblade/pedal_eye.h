#pragma once

#include <memory>
#include <vector>

#include "sawblade/adaa_clipper.h"
#include "sawblade/block_registry.h"
#include "sawblade/oversampler.h"
#include "sawblade/pedal_common.h"
#include "sawblade/pedal_params.h"
#include "sawblade/pedal_hm.h"  // HmColorEq, by include only
#include "sawblade/pedal_saw_params.h"
#include "sawblade/processor.h"

namespace sawblade {

// "One-knob chainsaw" (pedal.eye; docs/specs/phase7c_chainsaw_family.md section 3): the HM core
// with the colour-mix EQ fixed at low = high = 10, one gain knob (10..52 dB first stage), a 100 Hz
// pre-clip high-pass (stock HM: 60 Hz) and a tightness low cut. No mix. Static, nonlinear,
// time-invariant: NAM-trainable.
// Latency: 50 samples at every rate (oversampler round trip + two ADAA2 samples = 200 at 4 fs).
class EyePedal : public Processor {
 public:
  explicit EyePedal(const EyeParams& p, PedalImplConfig cfg = {});
  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;
  int latencySamples() const noexcept override { return latency_; }

 private:
  EyeParams p_;
  PedalImplConfig cfg_;
  int latency_ = 0;
  int pad_ = 0;
  float g1_ = 1.0f, levelGain_ = 1.0f;
  OnePole inHpf_, hpf100_, lpf8k_, lpf5k_;
  std::array<Biquad, 2> post_{};
  AdaaClipper clip1_, clip2_;
  HmColorEq eq_;
  ShortDelay padDelay_;
  Oversampler4x os_;
  std::vector<float> osBuf_;
};

std::unique_ptr<Processor> createEye(const Block& b, const BlockBuildContext& ctx);

}  // namespace sawblade
