#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "sawblade/adaa_clipper.h"
#include "sawblade/oversampler.h"
#include "sawblade/pedal_common.h"
#include "sawblade/pedal_params.h"
#include "sawblade/processor.h"

namespace sawblade {

// Every fixed constant of the pedal.rat model in one place (docs/specs/v0_9-vermin-A_design.md), SI units; the
// internal signal unit is the volt (float 1.0 at the block input = 1 V at the pedal input). A later capture fit
// changes this table only. Knob-controlled values are NOT here, only the circuit around them.
struct RatVoicing {
  // Op-amp gain stage: non-inverting, feedback Zf = Rd || Cf, ground leg Zg = (R1 + 1/sC1) || (R2 + 1/sC2).
  double rDistMax = 100e3, cf = 100e-12;
  double r1 = 47.0, c1 = 2.2e-6;    // corner 1.54 kHz
  double r2 = 560.0, c2 = 4.7e-6;   // corner 60.5 Hz (the leg the RUETZ mod removes)
  double distTaperA = 81.0;         // Rd = rDistMax * (a^t - 1) / (a - 1), t = dist / 10 (~10 % at mid travel)
  double rdMin = 0.01;              // numerical floor of Rd at DIST 0 (gain 1.0002 instead of exactly 1)
  // LM308-class op-amp: single pole, gain-bandwidth, slew rate, supply rails (9 V supply).
  // Extra oversampling of the op-amp stage and the diode clipper only, on top of the pedal's 4x (1, 2, 4 or 8): they
  // run at 4 * stageOversample * fs. The stage is a dynamic nonlinearity (slew edge, rails) that ADAA cannot
  // anti-alias. Measured in the pedal (DIST 10, FILTER 0, -6 dBFS, 48 kHz, silicon; worst existing pedal -82.1 dB):
  //   factor   latency   5 kHz     4.7 kHz   2.3 kHz   1.1 kHz   RTF (best of 5, range of runs)
  //   1        50        -57 dB    -51 dB    -58 dB    -69 dB    0.027-0.048
  //   2        52        -98 dB    -109 dB   -69 dB    -82 dB    0.051-0.094
  //   4        53        -129 dB   -134 dB   -93 dB    -105 dB   0.102-0.176
  //   8        54        -129 dB   -134 dB   -93 dB    -105 dB   0.217-0.358
  // Versus the other pedals at the same frequency (worst: pedal.hm v3 modded, asymmetric): 2.3 kHz -88.4 dB, 1.1 kHz
  // -100.5 dB; factor 2 is worse than every existing pedal there (-69 / -82 dB), factor 4 is below all of them
  // (-93 / -105 dB), so 4 is the default (main lead's rule). RTF varies with the load of a shared machine (the upper ends were measured on a loaded one; the ratio to factor 1 is steady: 2 = 2.0x, 4 = 3.7-3.9x, 8 = 7.4x).
  int stageOversample = 4;
  double gbwHz = 1.0e6;
  double slewVPerUs = 0.3;          // infinity = no slew limit (test seam)
  // Differential-pair input stage: the integrator input is Vd * tanh(v_diff / Vd), so the output rate is
  // SR * tanh(wt v_diff / SR) and saturates at exactly SR. Vd = SR / wt = 0.3 V/us / (2 pi 1 MHz) = 47.7 mV, which
  // is also what a bare bipolar pair gives (2 Vt = 52 mV at 300 K, within 9 %): the small-signal gain wt and the
  // maximum rate SR then follow from one tail current and one compensation capacitor (I_tail / Cc = SR,
  // gm / Cc = wt, Vd = I_tail / gm). Ignored when slewVPerUs is infinite.
  double diffPairVd = 0.0477465;
  double vRail = 3.8;               // supply rail (9 V supply, LM308 swing): the output is vRail * tanh(v / vRail)
  // Anti-windup: the integrator state v is clamped to +-railWindupLimit * vRail. The clamp is a slope kink of relative
  // size sech^2(limit), which aliases (-49 dB at 2.5-3.5, none seen from 6); the price of a large limit is a longer
  // recovery from a long saturation (up to (limit - 1) * vRail / SR = 63 us here, a few us in the real device),
  // which shifts the saturated edges in time without changing their shape. A voicing choice.
  double railWindupLimit = 6.0;
  // Filters.
  double inHpfHz = 20.0;            // input coupling, base rate
  double couplingR = 1.0e3, couplingC = 4.7e-6;  // after the op-amp: 1 k + 4.7 uF = 33.9 Hz first-order HPF
  double outHpfHz = 10.0;           // output coupling, base rate
  double outputTrimDb = 6.0;        // fixed output-buffer make-up on the wet path: VOLUME 8 (unity) is stock level
  double tightHz0 = 20.0, tightDecadeDiv = 10.0;  // TIGHT HPF: 20 * 10^(tightness / 10) Hz, as the other pedals
  // FILTER: fc = 1 / (2 pi (filterR + Rf) filterC), t = filter / 10. The taper is a VOICING CHOICE made by feel, not
  // verified against a schematic, so a later capture fit can revisit it (one switch): audio taper wired so FILTER 0
  // = brightest (Rf = 0, 32 kHz) and 10 = darkest (100 k, 475 Hz): Rf = filterPotR (a^t - 1) / (a - 1), about
  // 4.2 kHz at FILTER 5; or reverse log (filterReverseLog = true): Rf = filterPotR (1 - (a^(1-t) - 1) / (a - 1)),
  // 527 Hz at FILTER 5.
  double filterR = 1.5e3, filterPotR = 100e3, filterC = 3.3e-9, filterTaperA = 81.0;
  bool filterReverseLog = false;

  static const RatVoicing& stock() noexcept;

  double distOhms(double dist) const noexcept;
  double filterCornerHz(double filter) const noexcept;  // unclamped
  double tightHz(double tightness) const noexcept { return tightHz0 * std::pow(10.0, tightness / tightDecadeDiv); }
  double couplingHz() const noexcept { return 1.0 / (2.0 * 3.14159265358979323846 * couplingR * couplingC); }
  ClipShapeSpec clipShape(RatClip c) const noexcept;  // None: unused (clipper bypassed)
};

// tanh to < 1e-10 absolute error (tests/test_pedal_rat.cpp checks the maximum against std::tanh over [-30, 30]):
// an odd Taylor series below 0.25, above it 1 - 2 / (1 + e^(2|x|)) with e^y by range reduction and a degree-11 polynomial; ~3x faster than libm's tanh,
// which dominates the op-amp stage's cost (4 to 5 tanh per sample). Deterministic, no allocation.
inline double ratFastTanh(double x) noexcept {
  const double ax = std::fabs(x);
  if (ax < 0.25) {  // odd Taylor series to x^11: abs error < 1e-10 here (and much smaller below)
    const double x2 = x * x;
    return x * (1.0 + x2 * (-1.0 / 3.0 + x2 * (2.0 / 15.0 + x2 * (-17.0 / 315.0 + x2 * (62.0 / 2835.0 + x2 * (-1382.0 / 155925.0))))));
  }
  if (ax > 20.0) return std::copysign(1.0, x);
  const double y = -2.0 * ax;
  const int k = static_cast<int>(y * 1.4426950408889634 - 0.5);  // y <= 0: round to nearest
  const double r = y - static_cast<double>(k) * 0.6931471805599453;
  const double r2 = r * r, r4 = r2 * r2, r8 = r4 * r4;  // Estrin: short dependency chain
  const double p = ((1.0 + r) + (1.0 / 2.0 + r * (1.0 / 6.0)) * r2) +
                   ((1.0 / 24.0 + r * (1.0 / 120.0)) + (1.0 / 720.0 + r * (1.0 / 5040.0)) * r2) * r4 +
                   ((1.0 / 40320.0 + r * (1.0 / 362880.0)) + (1.0 / 3628800.0 + r * (1.0 / 39916800.0)) * r2) * r8;
  const double e = p * std::bit_cast<double>(static_cast<std::int64_t>(k + 1023) << 52);  // p * 2^k, k >= -58
  return std::copysign((1.0 - e) / (1.0 + e), x);
}

// The op-amp gain stage of the pedal.rat model, at the (oversampled) rate it runs at.
//
// Continuous model. vo is the op-amp output (the integrator state; the rails below make the output a saturated function of it), d(vo)/dt = wt * (vin - vm) with wt =
// 2 pi GBW (open loop A(s) = wt / s), vm the inverting-input node, vm = vo - vcf. Network (nothing flows into the
// op-amp input): the feedback current through Rd || Cf equals the current into the two ground legs,
//   (vo - vm) / Rd + Cf d(vo - vm)/dt = sum_k ik,   ik = (vm - vck) / Rk,   Ck d(vck)/dt = ik.
// The small-signal closed loop is H(s) = A / (1 + A beta), beta = Zg / (Zg + Zf), whose HF pole moves down as Rd
// (DIST) rises; with wt -> infinity it is the ideal gain 1 + Zf / Zg.
//
// Discretisation. Every energy-storing element (the three capacitors and the integrator) is trapezoidal
// (bilinear), solved implicitly and jointly each sample, so there is no explicit-Euler instability even though
// wt * T / 2 is about 16 at 192 kHz. Each capacitor is its companion model i = G v - Ih with G = 2C / T and
// history Ih' = 2 G v - Ih. The legs reduce to i_k = Y_k vm - J_k (Y_k = G_k / (1 + G_k R_k), J_k = Ih_k / (1 + G_k
// R_k)), the node equation gives vm = a vo + b (a = Yf / (Yf + sum Y), Yf = 1/Rd + Gf, b = (sum J - Ihf) / (Yf + sum
// Y)), and the integrator vo = vo' + h (e + e') with h = wt T / 2, e = vin - vm gives the linear solution
// vo = (vo' + h (vin - b + e')) / (1 + h a). This is the exact trapezoidal solution of the linear circuit.
//
// Nonlinear limits, both solved in the same implicit step. The integrator state is v; the stage output is
// y = Vrail tanh(v / Vrail) (a smooth rail that the feedback network sees):
//  1. slew: the real LM308 mechanism, a differential-pair input stage. The integrator input is
//     u(e) = Vd tanh(e / Vd), dv/dt = wt u(e), so the rate is SR tanh(wt e / SR) and approaches SR asymptotically.
//     In the trapezoidal step |dv| <= h (|u| + |u'|) < 2 h Vd = SR * T. The carried state is the limited input u'
//     (not the raw error), which the next trapezoid averages over, so no unlimited state is hidden;
//  2. anti-windup: v is clamped to +-railWindupLimit * Vrail (6), so the integrator cannot wind up past the rail.
//     Why 6: the clamp is a slope kink of relative size sech^2(limit) that aliases when it engages; 6 makes it
//     negligible, at the cost of a longer recovery from a long saturation (see RatVoicing).
// The step is solved implicitly with the network by Newton on F(v) = v - v' - h (u(e) + u'), e = vin - a y(v) - b:
// smooth and strictly increasing (F' = 1 + h a u'(e) y'(v) >= 1), started from the linear solution or, where
// |e| > Vd, from the hard-saturated root v' + h (+-Vd + u'), at most kMaxNewton = 8 steps, stopping when the step is
// below 1e-4 V (remaining error ~1e-6 V, under the alias floor). The final y and u come from the last evaluation plus a
// first-order correction (no further tanh). No allocation; it converges in 1-3 steps (newtonCapHits() counts the
// samples that hit the cap, 0 in all tests). The network states (vm, the capacitor histories) are then advanced with
// the final y.
//
// DIST (Rd) and the weight of the R2/C2 leg (RUETZ: 1 = in circuit, 0 = removed; in between it is a number of such
// legs in parallel, which keeps a live toggle click-free) change per sample along a linear ramp.
class RatOpAmpStage {
 public:
  void prepare(double fsOs, const RatVoicing& v = RatVoicing::stock());
  void reset() noexcept;
  // RT-safe. rampSamples <= 0: immediate.
  void setRd(double ohms, int rampSamples) noexcept;
  void setLegWeight(double w2, int rampSamples) noexcept;
  void process(float* io, int n) noexcept;

  // Diagnostics since reset(): samples on which the input stage was beyond Vd / the output was beyond half the rail.
  std::uint64_t slewClampCount() const noexcept { return slewHits_; }
  std::uint64_t railCount() const noexcept { return railHits_; }
  std::uint64_t newtonCapHits() const noexcept { return newtonCapHits_; }
  std::uint64_t newtonIterations() const noexcept { return newtonIters_; }
  static constexpr int kMaxNewton = 8;

 private:
  struct Ramp {
    double cur = 0.0, target = 0.0, step = 0.0;
    int left = 0;
    void set(double v, int n) noexcept {
      if (n <= 0) {
        cur = target = v;
        step = 0.0;
        left = 0;
      } else if (v != target || left > 0) {
        target = v;
        step = (v - cur) / n;
        left = n;
      }
    }
    void advance() noexcept {
      cur += step;
      if (--left == 0) cur = target;
    }
  };
  void updateCoeffs() noexcept;

  // constants
  double h_ = 0.0, gf_ = 0.0, g1_ = 0.0, g2_ = 0.0, y1_ = 0.0, y2_ = 0.0, c1_ = 1.0, c2_ = 1.0, r1_ = 0.0, r2_ = 0.0;
  double vd_ = 0.05, invVd_ = 20.0, vRail_ = 3.8, invRail_ = 0.26, vMax_ = 22.8;
  bool slewOn_ = true;
  // per-sample coefficients (change only while a ramp runs)
  double a_ = 0.0, invD_ = 0.0, invK_ = 1.0;
  Ramp rd_, w2_;
  // state
  double v_ = 0.0, e_ = 0.0, ihf_ = 0.0, ih1_ = 0.0, ih2_ = 0.0;
  std::uint64_t slewHits_ = 0, railHits_ = 0, newtonCapHits_ = 0, newtonIters_ = 0;
};

// "Rat-style distortion" (pedal.rat, display name VERMIN): input HPF (20 Hz) -> TIGHT HPF -> 4x oversampled
// [op-amp gain stage (GBW, slew, rails) -> 34 Hz coupling HPF -> diode clipper (CLIP)] -> FILTER (RC low-pass) ->
// 10 Hz output HPF -> VOLUME and clean MIX. Static (live knobs aside), nonlinear, time-invariant: NAM-trainable.
// Latency: the oversampler round trip, the stage-domain half-band chain (stageOversample > 1) and the ADAA2 sample,
// padded to a whole number of base-rate samples = 53 at the default stageOversample 4 (50 / 52 / 54 for 1 / 2 / 8)
// (the IIR filters' and the op-amp stage's group delay is not counted, as for the other modelled pedals). CLIP
// `none` keeps a one-sample clean delay in place of the clipper so the latency is the same in every mode. The
// clean mix is the input after the 20 Hz input HPF, delayed by exactly latencySamples().
//
// Stock: DIST 5, FILTER 5, VOLUME kRatStockVolume (5.8 on the level map, 8 = unity, plus RatVoicing::outputTrimDb), TIGHT 0, CLIP silicon, MIX 100, RUETZ off.
//
// Live parameters (setLiveParams, same thread as process()): applied at the start of the next process(). DIST,
// RUETZ ramp inside the op-amp stage and VOLUME / MIX as linear gain ramps over kLiveRampMs; FILTER and TIGHT
// redesign their filter once at that block start; CLIP applies immediately.
class RatPedal : public Processor {
 public:
  explicit RatPedal(const RatParams& p, PedalImplConfig cfg = {}, const RatVoicing* voicing = nullptr);
  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;
  int latencySamples() const noexcept override { return latency_; }
  void setLiveParams(const float* values, int count) noexcept override;

  const RatOpAmpStage& opAmpStage() const noexcept { return stage_; }

 private:
  void retarget(bool immediate) noexcept;

  RatVoicing v_;
  RatParams target_, applied_;
  bool dirty_ = false;
  std::array<float, kRatNumLive> liveTarget_{};
  PedalImplConfig cfg_;
  double fs_ = 48000.0, fsOs_ = 192000.0;
  int latency_ = 0, pad_ = 0;
  int rampOs_ = 0, rampBase_ = 0;
  bool tightOn_ = false, clipNone_ = false;
  float zPrev_ = 0.0f;  // the one-sample clean delay that stands in for the clipper's ADAA sample
  GainRamp wet_, dry_;
  OnePole inHpf_, tight_, coupling_, filter_, outHpf_;
  RatOpAmpStage stage_;
  AdaaClipper clip_;
  ShortDelay padDelay_;
  DryDelay dryDelay_;
  Oversampler4x os_;
  std::vector<float> osBuf_, hiBuf_;
  OversamplerNx osx_;
  ShortDelay hiPad_;  // pads the stage round trip to a whole number of 4 fs samples
  int stageOs_ = 1, hiPadSamples_ = 0, extra4_ = 0;
};

}  // namespace sawblade
