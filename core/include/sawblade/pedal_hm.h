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
// 1.2), per model version: v2() is the phase 7 / 7b voicing, unchanged (versions 1 and 2 render
// bit-identically to before version 3 existed), v3() the voicing calibrated against real captures in
// phase 7.1 (docs/specs/phase7c_chainsaw_family.md, Part 3). Knob-controlled values (frequencies, Qs,
// gains that are parameters of HmParams) are NOT here; these are the constants around them. If a
// change here alters the sound, it needs a new version.
struct HmVoicing {
  // Per-mode constants: stage-1 gain slope (dB per knob unit), low-gyrator gain slope, the interstage
  // LPF corner and the post-clip 4th-order LPF corner.
  struct Mode {
    double s1;            // G1 dB = g1BaseDb + s1 * distortion + gain1Db
    double sLow;          // low peak dB = lowBaseDb + sLow * low
    double interLpfHz;    // interstage 1st-order LPF
    double postLpfHz;     // post-clip 4th-order Butterworth
  };
  int version = 2;
  std::array<Mode, 3> modes = {{
      {4.0, 3.0, 5000.0, 6500.0},  // stock
      {4.6, 3.6, 5000.0, 6500.0},  // custom: extended low and gain
      {4.0, 3.0, 9000.0, 9000.0},  // modded: brighter top
  }};

  // Input: the tightness HPF is tightHz0 * 10^(tightness / tightDecadeDiv) (20 Hz at tightness 0).
  double tightHz0 = 20.0, tightDecadeDiv = 10.0;
  // Stage 3 pre-filter (4x domain).
  double preHpfHz = 60.0, preLpfHz = 8000.0;
  // Gains.
  double g1BaseDb = 6.0;       // stage-1 gain at distortion 0
  double interstageDb = 20.0;  // stock interstage gain (gain2Db trims it)
  // Colour-mix EQ.
  double lowBaseDb = -12.0;    // low gyrator gain at low = 0
  double highBaseDb = -8.0, highSlope = 2.2;  // both high gyrators
  double highQ = 1.2, presenceQ = 2.0, rolloffQ = 0.707;
  // 4th-order Butterworth as two RBJ low-pass sections.
  double postQ1 = 0.5411961001461969, postQ2 = 1.3065629648763766;

  // Clip knees of the `silicon` clip type: k+ and the negative knee of stage 1 / stage 2. v2: the
  // symmetric 0.5 / 0.5 of clipShapeSpec. v3: k- fitted (see v3() in pedal_hm.cpp).
  double silKPos = 0.5, silKNeg1 = 0.5, silKNeg2 = 0.5;
  // Custom mode: output offset (dB), k- pulled this fraction of the way toward k+, and the two
  // static shelves (gain from HmParams::customLowDb / customHighDb). All zero / off in v2.
  double dcBlockHz = 0.0;  // output DC blocker (1st-order HPF) after the downsampler; 0 = none (v2)
  bool customTrims = false;
  double customOutDb = 0.0, customKNegPull = 0.0;
  double customLowHz = 100.0, customLowQ = 0.7, customHighHz = 6000.0, customHighQ = 0.7;
  // v3 fit bands, fixed (the free-cascade residual fit of phase 7.1): low shelf, mid peak, upper-mid cut.
  bool fitBands = false;
  double fitLowShelfHz = 0.0, fitLowShelfDb = 0.0, fitLowShelfQ = 0.7071067811865476;
  double fitMidHz = 0.0, fitMidDb = 0.0, fitMidQ = 1.0;
  double fitCutHz = 0.0, fitCutDb = 0.0, fitCutQ = 1.0;

  static const HmVoicing& v2() noexcept;
  static const HmVoicing& v3() noexcept;
  static const HmVoicing& forVersion(int version) noexcept { return version >= 3 ? v3() : v2(); }
  const Mode& mode(HmMode m) const noexcept { return modes[static_cast<std::size_t>(m)]; }
};

// Stage 10 of the HM model: the "color mix" dual-gyrator active EQ as fitted RBJ biquads
// (docs/specs/phase7b_chainsaw_pedal.md), at the base rate. Exposed on its own so tests can check
// it against the table without the nonlinear stages. Bands 0-4 are the phase 7 table (low gyrator,
// high A, high B, presence peak, roll-off); v3 appends the three fit bands and, in custom mode, the
// two trim shelves. The static two-knob forms are the phase 7 (v2) table.
class HmColorEq {
 public:
  static constexpr int kNumBands = 5;   // the v2 table
  static constexpr int kMaxBands = 10;  // v3 + custom
  struct Bands {
    std::array<EqBand, kMaxBands> b{};
    int n = 0;
  };
  // low, high are 0..10 knobs; everything else at the v2 HmParams defaults (the phase 7 stock EQ).
  static std::array<EqBand, kNumBands> bands(double low, double high);
  static Bands bands(const HmParams& p);
  void configure(double sampleRate, double low, double high);  // throws std::invalid_argument
  // RT-safe: clamps frequencies below 0.45 fs, keeps the filter state.
  void setParams(double sampleRate, const HmParams& p) noexcept;
  void reset() noexcept;
  void process(float* io, int n) noexcept;

 private:
  std::array<Biquad, kMaxBands> f_{};
  int n_ = kNumBands;
};

// "Swedish chainsaw distortion" (pedal.hm, model versions 1-3): HM-2-topology model, simplified. Input
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
  // `voicing` overrides the table of p.modelVersion for the clip knees, gains and filters (a test seam
  // for the k- fit scan); the colour EQ always follows the version's table.
  explicit HmPedal(const HmParams& p, PedalImplConfig cfg = {}, const HmVoicing* voicing = nullptr);
  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;
  int latencySamples() const noexcept override { return latency_; }
  void setLiveParams(const float* values, int count) noexcept override;

 private:
  void designFilters(const HmParams& p) noexcept;
  void setShapes(const HmParams& p) noexcept;
  ClipShapeSpec shapeFor(ClipType t, int stage, const HmParams& p) const noexcept;
  void retarget(bool immediate) noexcept;

  HmVoicing v_;
  HmParams target_, applied_;
  bool dirty_ = false;
  std::array<float, kHmNumLive> liveTarget_{};  // the last live values (float domain): identical values are ignored
  PedalImplConfig cfg_;
  double fs_ = 48000.0, fsOs_ = 192000.0;
  int latency_ = 0;
  int pad_ = 0;  // padding in the oversampled domain
  int rampOs_ = 0, rampBase_ = 0;
  GainRamp g1_, g2_, wet_, dry_;
  OnePole inHpf_, hpf60_, lpf8k_, lpfInter_, dcBlock_;
  std::array<Biquad, 2> post_{};
  AdaaClipper clip1_, clip2_;
  HmColorEq eq_;
  ShortDelay padDelay_;
  DryDelay dryDelay_;
  Oversampler4x os_;
  std::vector<float> osBuf_;
};

}  // namespace sawblade
