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
#include "sawblade/pedal_hm.h"  // HmVoicing, HmParams defaults
#include "sawblade/processor.h"

namespace sawblade {

// "Modded chainsaw distortion" (pedal.hmx; docs/specs/phase7c_chainsaw_family.md section 2): the
// HM core of phase 7 with decoupled mids (the 1 kHz gyrator becomes the parametric HIGH-MID band,
// HIGH drives the 1.5 kHz gyrator alone), a low-mid band, a presence shelf, four clip types (7b's
// ClipType), a +9 dB boost stage before the clippers, a tightness low cut and a latency-matched
// clean blend. Every constant the stock HM shares comes from HmVoicing (stock mode) and the HmParams
// defaults; the constants below are the ones hmx adds.
// Input -> 4x [pre-filter -> gain (+boost) -> clip -> interstage LPF +20 dB -> clip -> 4th-order
// LPF] -> 10 Hz DC block -> 4-band EQ + presence -> roll-off -> level + mix.
// Static, nonlinear, time-invariant: NAM-trainable.
// Latency: oversampler round trip + two ADAA2 samples = 200 samples at 4 fs = 50 at the base rate,
// at every rate (IIR group delay is not counted). The dry branch of `mix` is delayed by the same
// number of samples; when mix == 100 it is skipped entirely.
//
// Live parameters (setLiveParams, same thread as process(); the 7b pattern of HmPedal): stored and
// applied at the start of the next process(). Gains (stage 1 incl. boost, level*mix, 1-mix) ramp
// over kLiveRampMs, filter coefficients are redesigned once at that block start, `clip` applies at
// once. Values equal to the current ones are ignored, so a static render stays bit-identical.
//
// Part 3 (phase 7.1): built on the v3 core (HmVoicing::v3(): 26 dB base drive, asymmetric silicon knees,
// the three fit bands, 9.5 kHz post-clip LPF, roll-off 16 kHz) with the fixed voicing deltas measured
// against a modded unit (+3.8 dB broad low peak at 110 Hz, -2.5 dB at 2.2 kHz, no extra presence) and a gain
// range that tops out at D 6.2: G1 = 26 + 1.24 * distortion (+ 9 boost) dB. `midVoice` selects the base
// centre and Q of the HIGH-MID band (HmxVoicing::kMidVoices); `highMidFreq` trims around the base.
struct HmxVoicing {
  struct MidVoiceRow {
    double baseHz, q;
  };
  static constexpr std::array<MidVoiceRow, kNumMidVoices> kMidVoices = {{{1000.0, 1.2}, {750.0, 1.4}, {2000.0, 1.2}}};
};

struct HmxConstants {
  static constexpr double dcBlockHz = 10.0;       // the asymmetric clip makes DC
  static constexpr double boostDb = 9.0;
  static constexpr double distDbPerUnit = 1.24;   // G1 = g1BaseDb + 1.24 * distortion
  static constexpr double lowShelfHz = 110.0, lowShelfDb = 3.8, lowShelfQ = 0.7;  // +3.8 dB at 110 Hz: a broad peak (Q 0.7), not a shelf: a shelf's +3.8 dB is its far-field gain and would read +1.9 dB at its corner, the measured delta is +3.8 AT 110 Hz
  static constexpr double dipHz = 2200.0, dipDb = -2.5, dipQ = 1.0;
  static constexpr double lowMidBaseHz = 200.0, lowMidOctaveRatio = 3.0, lowMidQ = 1.0, lowMidDbPerUnit = 2.0;
  static constexpr double highMidRatio = 1.6;  // fHM = base(midVoice) * ratio^((f - 5) / 5)
  static constexpr double presenceShelfHz = 3500.0, presenceShelfQ = 0.7071067811865476, presenceDbPerUnit = 1.2;
};

class HmxPedal : public Processor {
 public:
  static constexpr int kNumEqBands = 12;  // low, low-mid, high-mid, high, presence peak, presence shelf, roll-off, 3 fit bands, low shelf, dip

  explicit HmxPedal(const HmxParams& p, PedalImplConfig cfg = {});
  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;
  int latencySamples() const noexcept override { return latency_; }
  void setLiveParams(const float* values, int count) noexcept override;

  // Stages 11-17 as a table (also used by the tests' documentation of the design).
  static std::array<EqBand, kNumEqBands> eqBands(const HmxParams& p);

 private:
  void designFilters(const HmxParams& p) noexcept;
  void retarget(bool immediate) noexcept;
  ClipShapeSpec shapeFor(ClipType t, int stage) const noexcept;

  const HmVoicing& v_ = HmVoicing::v3();
  HmxParams target_, applied_;
  bool dirty_ = false;
  std::array<float, kHmxNumLive> liveTarget_{};
  PedalImplConfig cfg_;
  double fs_ = 48000.0, fsOs_ = 192000.0;
  int latency_ = 0;
  int pad_ = 0;
  int rampOs_ = 0, rampBase_ = 0;
  GainRamp g1_, wet_, dry_;
  float g2_ = 1.0f;
  OnePole inHpf_, hpf60_, lpf8k_, lpf5k_, dcBlock_;
  std::array<Biquad, 2> post_{};
  std::array<Biquad, kNumEqBands> eq_{};
  AdaaClipper clip1_, clip2_;
  ShortDelay padDelay_;
  DryDelay dryDelay_;
  Oversampler4x os_;
  std::vector<float> osBuf_;
};

std::unique_ptr<Processor> createHmx(const Block& b, const BlockBuildContext& ctx);

}  // namespace sawblade
