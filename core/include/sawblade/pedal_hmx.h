#pragma once

#include <array>
#include <memory>
#include <vector>

#include "sawblade/adaa_clipper.h"
#include "sawblade/block_registry.h"
#include "sawblade/eq.h"
#include "sawblade/oversampler.h"
#include "sawblade/pedal_common.h"
#include "sawblade/pedal_params.h"
#include "sawblade/pedal_saw_params.h"
#include "sawblade/pedal_stages.h"
#include "sawblade/processor.h"

namespace sawblade {

// "Modded chainsaw distortion" (pedal.hmx; docs/specs/phase7c_chainsaw_family.md section 2): the
// HM core of phase 7 with decoupled mids (the 1 kHz gyrator becomes the parametric HIGH-MID band,
// HIGH drives the 1.5 kHz gyrator alone), a low-mid band, a presence shelf, three clip types, a
// +9 dB boost stage before the clippers, a tightness low cut and a latency-matched clean blend.
// Input -> 4x [pre-filter -> gain (+boost) -> clip -> interstage LPF +20 dB -> clip -> 4th-order
// LPF] -> 10 Hz DC block -> 4-band EQ + presence -> roll-off -> level + mix.
// Static, nonlinear, time-invariant: NAM-trainable.
// Latency: oversampler round trip + two ADAA2 samples = 200 samples at 4 fs = 50 at the base rate,
// at every rate (IIR group delay is not counted). The dry branch of `mix` is delayed by the same
// number of samples; when mix == 100 it is skipped entirely.
class HmxPedal : public Processor {
 public:
  static constexpr int kNumEqBands = 7;  // low, low-mid, high-mid, high, presence peak, presence shelf, roll-off

  explicit HmxPedal(const HmxParams& p, PedalImplConfig cfg = {});
  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;
  int latencySamples() const noexcept override { return latency_; }

  // Stages 11-17 as a table (also used by the tests' documentation of the design).
  static std::array<EqBand, kNumEqBands> eqBands(const HmxParams& p);

 private:
  HmxParams p_;
  PedalImplConfig cfg_;
  int latency_ = 0;
  int pad_ = 0;
  bool mixDry_ = false;  // mix < 100: the dry branch is live
  float g1_ = 1.0f, wetGain_ = 1.0f, dryGain_ = 0.0f;
  OnePole inHpf_, hpf60_, lpf8k_, lpf5k_, dcBlock_;
  std::array<Biquad, 2> post_{};
  std::array<Biquad, kNumEqBands> eq_{};
  AdaaClipper clip1_, clip2_;
  ShortDelay padDelay_;
  stages::DryDelay dryDelay_;
  Oversampler4x os_;
  std::vector<float> osBuf_, dryBuf_;
};

std::unique_ptr<Processor> createHmx(const Block& b, const BlockBuildContext& ctx);

}  // namespace sawblade
