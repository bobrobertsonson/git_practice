#include "sawblade/pedal_rat.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace sawblade {

const RatVoicing& RatVoicing::stock() noexcept {
  static const RatVoicing v;
  return v;
}

double RatVoicing::distOhms(double dist) const noexcept {
  const double t = std::clamp(dist, 0.0, 10.0) / 10.0;
  return std::max(rDistMax * (std::pow(distTaperA, t) - 1.0) / (distTaperA - 1.0), rdMin);
}

double RatVoicing::filterCornerHz(double filter) const noexcept {
  const double t = std::clamp(filter, 0.0, 10.0) / 10.0;
  const double rf = filterPotR * (1.0 - (std::pow(filterTaperA, 1.0 - t) - 1.0) / (filterTaperA - 1.0));
  return 1.0 / (2.0 * std::numbers::pi * (filterR + rf) * filterC);
}

ClipShapeSpec RatVoicing::clipShape(RatClip c) const noexcept {
  switch (c) {
    case RatClip::Silicon: return {0.55, 0.55, 1};      // 1N914 pair
    case RatClip::Led: return {1.7, 1.7, 2};            // red LED pair
    case RatClip::Asymmetric: return {0.55, 1.1, 1};    // one diode one way, two the other
    case RatClip::None: break;
  }
  return {0.55, 0.55, 1};
}

// ---- op-amp stage ---------------------------------------------------------------------------------------

void RatOpAmpStage::prepare(double fsOs, const RatVoicing& v) {
  if (!(fsOs >= 8000.0)) throw std::invalid_argument("pedal.rat: unsupported sample rate");
  const double T = 1.0 / fsOs;
  h_ = std::numbers::pi * v.gbwHz * T;  // wt * T / 2
  gf_ = 2.0 * v.cf / T;
  g1_ = 2.0 * v.c1 / T;
  g2_ = 2.0 * v.c2 / T;
  r1_ = v.r1;
  r2_ = v.r2;
  c1_ = 1.0 / (1.0 + g1_ * v.r1);
  c2_ = 1.0 / (1.0 + g2_ * v.r2);
  y1_ = g1_ * c1_;
  y2_ = g2_ * c2_;
  slewOn_ = std::isfinite(v.slewVPerUs);
  eSat_ = slewOn_ ? v.slewVPerUs * 1e6 / (2.0 * std::numbers::pi * v.gbwHz) : 0.0;  // SR / wt
  vRail_ = v.vRail;
  knee_ = v.railKneeFrac * v.vRail;
  kneeSpan_ = v.vRail - knee_;
  rd_.set(v.distOhms(5.0), 0);
  w2_.set(1.0, 0);
  updateCoeffs();
  reset();
}

void RatOpAmpStage::reset() noexcept {
  vo_ = e_ = ihf_ = ih1_ = ih2_ = 0.0;
  slewHits_ = railHits_ = 0;
}

void RatOpAmpStage::setRd(double ohms, int rampSamples) noexcept {
  rd_.set(ohms, rampSamples);
  updateCoeffs();
}

void RatOpAmpStage::setLegWeight(double w2, int rampSamples) noexcept {
  w2_.set(w2, rampSamples);
  updateCoeffs();
}

void RatOpAmpStage::updateCoeffs() noexcept {
  const double yf = 1.0 / rd_.cur + gf_;
  const double d = yf + y1_ + w2_.cur * y2_;
  a_ = yf / d;
  invD_ = 1.0 / d;
  invK_ = 1.0 / (1.0 + h_ * a_);
}

void RatOpAmpStage::process(float* io, int n) noexcept {
  for (int i = 0; i < n; ++i) {
    if (rd_.left > 0 || w2_.left > 0) {
      if (rd_.left > 0) rd_.advance();
      if (w2_.left > 0) w2_.advance();
      updateCoeffs();
    }
    const double vin = io[i];
    const double w2 = w2_.cur;
    const double b = (c1_ * ih1_ + w2 * c2_ * ih2_ - ihf_) * invD_;
    // Linear (unlimited) trapezoidal solution; accepted when the input stage is not saturated.
    double vo = (vo_ + h_ * (vin - b + e_)) * invK_;
    const double eLin = vin - (a_ * vo + b);
    if (slewOn_ && std::fabs(eLin) > eSat_) {
      // Slew: the integrator input saturates at +-eSat = SR / wt, so |vo - vo'| <= h (|e| + |e'|) <= SR * T.
      vo = vo_ + h_ * (std::copysign(eSat_, eLin) + e_);
      ++slewHits_;
    }
    const double av = std::fabs(vo);
    if (av > knee_) {
      ++railHits_;
      vo = std::copysign(knee_ + kneeSpan_ * std::tanh((av - knee_) / kneeSpan_), vo);
    }
    // Advance the network with the final vo. e_ is the saturated integrator input, so no unclamped state is kept.
    const double vm = a_ * vo + b;
    e_ = slewOn_ ? std::clamp(vin - vm, -eSat_, eSat_) : vin - vm;
    ihf_ = 2.0 * gf_ * (vo - vm) - ihf_;
    const double i1 = y1_ * vm - c1_ * ih1_;
    ih1_ = 2.0 * g1_ * (vm - r1_ * i1) - ih1_;
    const double i2 = y2_ * vm - c2_ * ih2_;
    ih2_ = 2.0 * g2_ * (vm - r2_ * i2) - ih2_;
    vo_ = vo;
    io[i] = static_cast<float>(vo);
  }
}

// ---- pedal ----------------------------------------------------------------------------------------------

RatPedal::RatPedal(const RatParams& p, PedalImplConfig cfg, const RatVoicing* voicing)
    : v_(voicing ? *voicing : RatVoicing::stock()), target_(p), applied_(p), cfg_(cfg) {
  ratLiveFromParams(p, liveTarget_.data());
  const int nAdaa = cfg_.adaa ? AdaaClipper::kLatency : 0;
  if (cfg_.oversample) {
    const int osTotal = Oversampler4x::roundTripLatencyOs() + nAdaa;
    pad_ = osPadding(osTotal);
    latency_ = (osTotal + pad_) / 4;
  } else {
    pad_ = 0;
    latency_ = nAdaa;
  }
}

void RatPedal::retarget(bool immediate) noexcept {
  const RatParams& n = target_;
  const RatParams& a = applied_;
  if (immediate || n.tightness != a.tightness) {
    const bool on = n.tightness > 0.0;
    if (on) tight_.setHighPass(std::min(v_.tightHz(n.tightness), 0.45 * fs_), fs_);
    if (on != tightOn_) tight_.reset();
    tightOn_ = on;
  }
  if (immediate || n.filter != a.filter) filter_.setLowPass(std::min(v_.filterCornerHz(n.filter), 0.45 * fs_), fs_);
  if (immediate || n.clip != a.clip) {
    const bool none = n.clip == RatClip::None;
    if (!none) {
      const ClipShapeSpec s = v_.clipShape(n.clip);
      clip_.setShape(s.kPos, s.kNeg, s.order);
      if (clipNone_) clip_.reset();  // leaving `none`: start the clipper clean
    }
    clipNone_ = none;
  }
  const double m = n.mix / 100.0;
  const double wet = static_cast<double>(dbToGain(pedalLevelDb(n.volume) + v_.outputTrimDb)) * m;
  const double rd = v_.distOhms(n.distortion), w2 = n.ruetz ? 0.0 : 1.0;
  if (immediate) {
    stage_.setRd(rd, 0);
    stage_.setLegWeight(w2, 0);
    wet_.setImmediate(wet);
    dry_.setImmediate(1.0 - m);
  } else {
    stage_.setRd(rd, rampOs_);
    stage_.setLegWeight(w2, rampOs_);
    wet_.setTarget(wet, rampBase_);
    dry_.setTarget(1.0 - m, rampBase_);
  }
  applied_ = n;
  dirty_ = false;
}

void RatPedal::setLiveParams(const float* values, int count) noexcept {
  const RatParams np = ratParamsFromLive(values, count);
  std::array<float, kRatNumLive> nv;
  ratLiveFromParams(np, nv.data());
  if (nv == liveTarget_) return;  // same values (in float, as the host sends them): nothing to do
  liveTarget_ = nv;
  target_ = np;
  dirty_ = true;
}

void RatPedal::prepare(const ProcessSpec& spec) {
  if (!(spec.sampleRate >= 8000.0) || spec.maxBlockSize < 1)
    throw std::invalid_argument("pedal.rat: unsupported sample rate or block size");
  fs_ = spec.sampleRate;
  fsOs_ = cfg_.oversample ? 4.0 * fs_ : fs_;
  rampBase_ = rampSamplesFor(fs_);
  rampOs_ = rampSamplesFor(fsOs_);

  inHpf_.setHighPass(v_.inHpfHz, fs_);
  coupling_.setHighPass(std::min(v_.couplingHz(), 0.45 * fsOs_), fsOs_);
  outHpf_.setHighPass(v_.outHpfHz, fs_);
  stage_.prepare(fsOs_, v_);
  clip_.setAdaa(cfg_.adaa);
  tightOn_ = false;
  clipNone_ = false;
  retarget(true);
  padDelay_.set(pad_);
  dryDelay_.prepare(latency_, spec.maxBlockSize);
  if (cfg_.oversample) {
    os_.prepare(spec.maxBlockSize);
    osBuf_.assign(static_cast<std::size_t>(4 * spec.maxBlockSize), 0.0f);
  } else {
    osBuf_.clear();
  }
  reset();
}

void RatPedal::reset() {
  inHpf_.reset();
  tight_.reset();
  coupling_.reset();
  filter_.reset();
  outHpf_.reset();
  stage_.reset();
  clip_.reset();
  zPrev_ = 0.0f;
  padDelay_.reset();
  dryDelay_.reset();
  if (cfg_.oversample) os_.reset();
}

void RatPedal::process(float* io, int n) noexcept {
  if (dirty_) retarget(false);
  const bool flat = cfg_.flatFilters;
  if (!flat) inHpf_.process(io, n);
  dryDelay_.push(io, n);  // the clean mix is tapped after the input HPF, before TIGHT
  if (!flat && tightOn_) tight_.process(io, n);
  float* x = io;
  int m = n;
  if (cfg_.oversample) {
    x = osBuf_.data();
    m = 4 * n;
    os_.upsample(io, n, x);
  }
  if (!flat) {
    stage_.process(x, m);
    coupling_.process(x, m);
  }
  if (clipNone_) {
    if (cfg_.adaa) {  // stands in for the clipper's ADAA sample: same latency in every CLIP mode
      for (int i = 0; i < m; ++i) {
        const float in = x[i];
        x[i] = zPrev_;
        zPrev_ = in;
      }
    }
  } else {
    const float last = x[m - 1];
    clip_.process(x, m);
    zPrev_ = last;
  }
  padDelay_.process(x, m);
  if (cfg_.oversample) os_.downsample(x, n, io);
  if (!flat) {
    filter_.process(io, n);
    outHpf_.process(io, n);
  }
  if (!dry_.active() && dry_.cur == 0.0) {
    wet_.apply(io, n);  // mix 100 %: the dry branch is skipped entirely
  } else {
    for (int i = 0; i < n; ++i) {
      const float wg = wet_.next(), dg = dry_.next();
      io[i] = dg * dryDelay_.tap(i) + wg * io[i];
    }
  }
  dryDelay_.commit(n);
}

}  // namespace sawblade
