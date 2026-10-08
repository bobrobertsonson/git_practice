#pragma once

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
  double gbwHz = 1.0e6;
  double slewVPerUs = 0.3;          // infinity = no slew limit (test seam)
  double vRail = 3.8, railKneeFrac = 0.75;  // smooth rail: linear below frac * vRail, tanh into vRail above
  // Filters.
  double inHpfHz = 20.0;            // input coupling, base rate
  double couplingR = 1.0e3, couplingC = 4.7e-6;  // after the op-amp: 1 k + 4.7 uF = 33.9 Hz first-order HPF
  double outHpfHz = 10.0;           // output coupling, base rate
  double outputTrimDb = 6.0;        // fixed output-buffer make-up on the wet path: VOLUME 8 (unity) is stock level
  double tightHz0 = 20.0, tightDecadeDiv = 10.0;  // TIGHT HPF: 20 * 10^(tightness / 10) Hz, as the other pedals
  // FILTER: fc = 1 / (2 pi (filterR + Rf) filterC), Rf = filterPotR * (1 - (a^(1-t) - 1) / (a - 1)) (reverse log).
  double filterR = 1.5e3, filterPotR = 100e3, filterC = 3.3e-9, filterTaperA = 81.0;

  static const RatVoicing& stock() noexcept;

  double distOhms(double dist) const noexcept;
  double filterCornerHz(double filter) const noexcept;  // unclamped
  double tightHz(double tightness) const noexcept { return tightHz0 * std::pow(10.0, tightness / tightDecadeDiv); }
  double couplingHz() const noexcept { return 1.0 / (2.0 * 3.14159265358979323846 * couplingR * couplingC); }
  ClipShapeSpec clipShape(RatClip c) const noexcept;  // None: unused (clipper bypassed)
};

// The op-amp gain stage of the pedal.rat model, at the (oversampled) rate it runs at.
//
// Continuous model. vo is the op-amp output and the integrator state, d(vo)/dt = wt * (vin - vm) with wt =
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
// Nonlinear limits, applied to that solution in this order:
//  1. slew: the integrator input e saturates at +-SR / wt (the op-amp's input stage cannot ask for more than SR),
//     d(vo)/dt = wt * clamp(e, +-SR / wt). In the trapezoidal step this is a clamp of the output step,
//     |vo - vo'| <= h (|e| + |e'|) <= 2 h SR / wt = SR * T, reached exactly while the stage slews. The saturated
//     value is also what the next step's trapezoid averages over (e'), so the carried state is the clamped one and
//     a slew onset / release is a sub-sample-consistent half step rather than a full or missing step. Where the
//     linear solution is saturated the step is solved in closed form (the equation is monotone in vo, so the
//     clamped root is unique);
//  2. rails: a smooth saturation (linear below knee, tanh into +-vRail) of that value.
// The final vo is the only integrator state, so the stage cannot wind up past a limit; the network states (vm, the
// capacitor histories, the clamped e') are then advanced with the final vo, so no hidden unclamped state exists.
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

  // Diagnostics since reset(): samples on which the slew saturation acted / the output was in the rail knee region.
  std::uint64_t slewClampCount() const noexcept { return slewHits_; }
  std::uint64_t railCount() const noexcept { return railHits_; }

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
  double eSat_ = 0.0, vRail_ = 3.8, knee_ = 2.85, kneeSpan_ = 0.95;
  bool slewOn_ = true;
  // per-sample coefficients (change only while a ramp runs)
  double a_ = 0.0, invD_ = 0.0, invK_ = 1.0;
  Ramp rd_, w2_;
  // state
  double vo_ = 0.0, e_ = 0.0, ihf_ = 0.0, ih1_ = 0.0, ih2_ = 0.0;
  std::uint64_t slewHits_ = 0, railHits_ = 0;
};

// "Rat-style distortion" (pedal.rat, display name VERMIN): input HPF (20 Hz) -> TIGHT HPF -> 4x oversampled
// [op-amp gain stage (GBW, slew, rails) -> 34 Hz coupling HPF -> diode clipper (CLIP)] -> FILTER (RC low-pass) ->
// 10 Hz output HPF -> VOLUME and clean MIX. Static (live knobs aside), nonlinear, time-invariant: NAM-trainable.
// Latency: the oversampler round trip plus the ADAA2 sample, padded to a whole number of base-rate samples = 50
// (the IIR filters' and the op-amp stage's group delay is not counted, as for the other modelled pedals). CLIP
// `none` keeps a one-sample clean delay in place of the clipper so the latency is the same in every mode. The
// clean mix is the input after the 20 Hz input HPF, delayed by exactly latencySamples().
//
// Stock: DIST 5, FILTER 5, VOLUME kRatStockVolume (8 = unity on the level map, plus RatVoicing::outputTrimDb), TIGHT 0, CLIP silicon, MIX 100, RUETZ off.
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
  std::vector<float> osBuf_;
};

}  // namespace sawblade
