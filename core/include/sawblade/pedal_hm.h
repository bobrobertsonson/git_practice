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

// Every fixed voicing constant of the pedal.hm model in one place (docs/specs/phase7b_chainsaw_pedal.md,
// 1.2), so a later fit against real captures (phase 7.1) is a one-file edit. Knob-controlled values
// (frequencies, Qs, gains that are parameters of HmParams) are NOT here; these are the constants
// around them. If a change here alters the sound, bump the model version.
struct HmVoicing {
  // Per-mode constants: stage-1 gain slope (dB per knob unit), low-gyrator gain slope, the interstage
  // LPF corner and the post-clip 4th-order LPF corner.
  struct Mode {
    double s1;            // G1 dB = g1BaseDb + s1 * distortion + gain1Db
    double sLow;          // low peak dB = lowBaseDb + sLow * low
    double interLpfHz;    // interstage 1st-order LPF
    double postLpfHz;     // post-clip 4th-order Butterworth
  };
  static constexpr std::array<Mode, 3> kModes = {{
      {4.0, 3.0, 5000.0, 6500.0},  // stock
      {4.6, 3.6, 5000.0, 6500.0},  // custom: extended low and gain
      {4.0, 3.0, 9000.0, 9000.0},  // modded: brighter top
  }};

  // Input: the tightness HPF is tightHz0 * 10^(tightness / tightDecadeDiv) (20 Hz at tightness 0).
  static constexpr double tightHz0 = 20.0, tightDecadeDiv = 10.0;
  // Stage 3 pre-filter (4x domain).
  static constexpr double preHpfHz = 60.0, preLpfHz = 8000.0;
  // Gains.
  static constexpr double g1BaseDb = 6.0;      // stage-1 gain at distortion 0
  static constexpr double interstageDb = 20.0;  // stock interstage gain (gain2Db trims it)
  // Colour-mix EQ.
  static constexpr double lowBaseDb = -12.0;   // low gyrator gain at low = 0
  static constexpr double highBaseDb = -8.0, highSlope = 2.2;  // both high gyrators
  static constexpr double highQ = 1.2, presenceQ = 2.0, rolloffQ = 0.707;
  // 4th-order Butterworth as two RBJ low-pass sections.
  static constexpr double postQ1 = 0.5411961001461969, postQ2 = 1.3065629648763766;
};

// Stage 10 of the HM model: the "color mix" dual-gyrator active EQ as five fitted RBJ biquads
// (docs/specs/phase7b_chainsaw_pedal.md), at the base rate. Exposed on its own so tests can check
// it against the table without the nonlinear stages.
class HmColorEq {
 public:
  static constexpr int kNumBands = 5;
  // low, high are 0..10 knobs; everything else at the HmParams defaults (the phase 7 stock EQ).
  static std::array<EqBand, kNumBands> bands(double low, double high);
  static std::array<EqBand, kNumBands> bands(const HmParams& p);
  void configure(double sampleRate, double low, double high);  // throws std::invalid_argument
  // RT-safe: clamps frequencies below 0.45 fs, keeps the filter state.
  void setParams(double sampleRate, const HmParams& p) noexcept;
  void reset() noexcept;
  void process(float* io, int n) noexcept;

 private:
  std::array<Biquad, kNumBands> f_{};
};

// "Swedish chainsaw distortion" (pedal.hm, model version 2): HM-2-topology model, simplified. Input
// buffer (tightness HPF) -> 4x oversampled [pre-filter -> gain -> clip -> interstage LPF + gain ->
// clip -> 4th-order LPF] -> colour-mix EQ -> level + clean mix. Static (live knobs aside),
// nonlinear: NAM-trainable.
// Latency: the oversampler round trip plus the two ADAA2 stages, padded to a whole number of
// base-rate samples (reported by latencySamples(); the IIR filters' group delay is not counted).
// The clean mix delays the dry input by exactly latencySamples().
//
// Live parameters (setLiveParams, same thread as process()): stored, applied at the start of the next
// process(). Gains (stage gains, level*mix, 1-mix) ramp linearly over kLiveRampMs; filter coefficients
// are redesigned once at that block start (a small step is possible); enums, the clip knees (bias)
// apply immediately. With no live change pending the code multiplies by the same constants as a static
// build.
class HmPedal : public Processor {
 public:
  explicit HmPedal(const HmParams& p, PedalImplConfig cfg = {});
  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;
  int latencySamples() const noexcept override { return latency_; }
  void setLiveParams(const float* values, int count) noexcept override;

 private:
  void designFilters(const HmParams& p) noexcept;
  void setShapes(const HmParams& p) noexcept;
  void retarget(bool immediate) noexcept;

  HmParams target_, applied_;
  bool dirty_ = false;
  std::array<float, kHmNumLive> liveTarget_{};  // the last live values (float domain): identical values are ignored
  PedalImplConfig cfg_;
  double fs_ = 48000.0, fsOs_ = 192000.0;
  int latency_ = 0;
  int pad_ = 0;  // padding in the oversampled domain
  int rampOs_ = 0, rampBase_ = 0;
  GainRamp g1_, g2_, wet_, dry_;
  OnePole inHpf_, hpf60_, lpf8k_, lpfInter_;
  std::array<Biquad, 2> post_{};
  AdaaClipper clip1_, clip2_;
  HmColorEq eq_;
  ShortDelay padDelay_;
  DryDelay dryDelay_;
  Oversampler4x os_;
  std::vector<float> osBuf_;
};

}  // namespace sawblade
