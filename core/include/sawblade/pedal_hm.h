#pragma once

#include <array>
#include <vector>

#include "sawblade/adaa_clipper.h"
#include "sawblade/eq.h"
#include "sawblade/oversampler.h"
#include "sawblade/pedal_common.h"
#include "sawblade/pedal_params.h"
#include "sawblade/processor.h"

namespace sawblade {

// Stage 10 of the HM model: the "color mix" dual-gyrator active EQ as five fitted RBJ biquads
// (docs/specs/phase7_modeled_pedals.md), at the base rate. Exposed on its own so tests can check
// it against the table without the nonlinear stages.
class HmColorEq {
 public:
  static constexpr int kNumBands = 5;
  // low, high are 0..10 knobs.
  static std::array<EqBand, kNumBands> bands(double low, double high);
  void configure(double sampleRate, double low, double high);  // throws std::invalid_argument
  void reset() noexcept;
  void process(float* io, int n) noexcept;

 private:
  std::array<Biquad, kNumBands> f_{};
};

// "Swedish chainsaw distortion" (pedal.hm): HM-2-topology model, simplified. Input buffer ->
// 4x oversampled [pre-filter -> gain -> diode clip -> interstage LPF + 20 dB -> diode clip ->
// 4th-order LPF] -> colour-mix EQ -> level. Static, nonlinear, time-invariant: NAM-trainable.
// Latency: the oversampler round trip plus the two ADAA2 stages, padded to a whole number of
// base-rate samples (reported by latencySamples(); the IIR filters' group delay is not counted).
class HmPedal : public Processor {
 public:
  explicit HmPedal(const HmParams& p, PedalImplConfig cfg = {});
  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;
  int latencySamples() const noexcept override { return latency_; }

 private:
  HmParams p_;
  PedalImplConfig cfg_;
  int latency_ = 0;
  int pad_ = 0;  // padding in the oversampled domain
  float g1_ = 1.0f, levelGain_ = 1.0f;
  OnePole inHpf_, hpf60_, lpf8k_, lpf5k_;
  std::array<Biquad, 2> post_{};
  AdaaClipper clip1_, clip2_;
  HmColorEq eq_;
  ShortDelay padDelay_;
  Oversampler4x os_;
  std::vector<float> osBuf_;
};

}  // namespace sawblade
