#include "sawblade/pedal_hm.h"

#include <algorithm>
#include <stdexcept>

namespace sawblade {
namespace {

double g1Db(const HmVoicing& v, const HmParams& p) noexcept { return v.g1BaseDb + v.mode(p.mode).s1 * p.distortion + p.gain1Db; }
double g2Db(const HmVoicing& v, const HmParams& p) noexcept { return v.interstageDb + p.gain2Db; }
bool customTrimsOn(const HmVoicing& v, const HmParams& p) noexcept { return v.customTrims && p.mode == HmMode::Custom; }

}  // namespace

const HmVoicing& HmVoicing::v2() noexcept {
  static const HmVoicing v{};  // the default member initialisers ARE the phase 7 / 7b voicing
  return v;
}

const HmVoicing& HmVoicing::v3() noexcept {
  static const HmVoicing v = [] {
    HmVoicing x;
    x.version = 3;
    // Drive range: real units are saturated from D-2 (phase 7.1): 26..46 dB across the knob.
    x.g1BaseDb = 26.0;
    // stock / custom: wider post-clip LPF (the v2 model was ~12 dB short at 10 kHz); custom has no gain change.
    // modded: post-clip LPF 11 kHz. The spec also asks for interLpfHz = 11 kHz; with the asymmetric stage-2
    // knee that alias floor is -67..-72 dB (budget -80) because the interstage LPF is what limits it
    // (inter 5 kHz: -85..-89 dB, 6.5 kHz: -82..-84 dB, 8 kHz: -73..-80 dB). 6.5 kHz is the highest corner
    // that keeps every clip type under -80 dB and still gives the +3 dB brighter top the spec wants
    // (measured +3.4 dB; 5 kHz gives only +2.4). Reported to the lead.
    x.modes = {{{2.0, 3.0, 5000.0, 9500.0}, {2.0, 3.0, 5000.0, 9500.0}, {2.0, 3.0, 6500.0, 11000.0}}};
    // Asymmetric diode clip (real units: H2 ~ -9, H3 ~ -18 dB re the fundamental). The `silicon` knees are
    // k+ = 0.5 and a FITTED k-, found by the scan in the test "v3 k- scan" (tests/test_pedals_v3.cpp):
    // k- from 0.5 to 3.0 in steps of 0.05, the whole pedal at D 10, L = H = 5, 500 Hz; pick the k- whose
    // H2 is nearest -9 dBc, then check H3 in [-22, -14] dBc.
    //  * At the spec's -20 dBFS the pedal is a hard square wave at D 10 (46 dB + 20 dB interstage into
    //    knees of 0.5..3): H2 never exceeds -25 dBc for any k- in 0.5..3 and H3 stays at -7.6 dBc, so
    //    the windows are unreachable there. At -60 dBFS (the same drive point with 40 dB more headroom)
    //    the scan lands on k- = 2.05 with both stages asymmetric (H2 -9.1, H3 -14.7 dBc).
    //  * Both stages asymmetric alias too much (-73 dB at 5 kHz, D 10; the budget is -80). The spec's
    //    fallback, stage 2 only (stage 1 stays symmetric, 0.5 / 0.5), scans to k- = 2.10 (H2 -9.0,
    //    H3 -14.95 dBc) and measures -89 dB. That is the table: stage 1 symmetric, stage 2 k- = 2.10.
    x.silKPos = 0.5;
    x.silKNeg1 = 0.5;
    x.silKNeg2 = 2.10;
    // The asymmetric clip makes a DC offset at the output: 10 Hz DC blocker after the downsampler.
    x.dcBlockHz = 10.0;
    // Custom mode (measured on four standard/custom pairs): +2.5 dB, k- 25 % toward k+, two shelves.
    x.customTrims = true;
    x.customOutDb = 2.5;
    x.customKNegPull = 0.25;
    // Fit bands (phase 7.1 final: 8 labelled stock models, 1.36 dB RMS).
    x.fitBands = true;
    x.fitLowShelfHz = 85.0; x.fitLowShelfDb = 1.7; x.fitLowShelfQ = 0.7071067811865476;
    x.fitMidHz = 683.0; x.fitMidDb = 4.5; x.fitMidQ = 2.4;
    x.fitCutHz = 5500.0; x.fitCutDb = -12.0; x.fitCutQ = 1.54;
    return x;
  }();
  return v;
}

HmColorEq::Bands HmColorEq::bands(const HmParams& p) {
  const HmVoicing& v = HmVoicing::forVersion(p.modelVersion);
  const double hi = v.highBaseDb + v.highSlope * p.high;
  Bands r;
  r.b[0] = {EqType::Peak, p.lowFreq, v.lowBaseDb + v.mode(p.mode).sLow * p.low, p.lowQ, true};  // low gyrator
  r.b[1] = {EqType::Peak, p.highFreq, hi, v.highQ, true};                                       // high gyrator A
  r.b[2] = {EqType::Peak, p.highFreq * p.highSpread, hi, v.highQ, true};                        // high gyrator B
  r.b[3] = {EqType::Peak, p.presenceFreq, p.presenceDb, v.presenceQ, true};                     // presence peak
  r.b[4] = {EqType::LowPass, p.rolloffHz, 0.0, v.rolloffQ, true};                               // output roll-off
  r.n = 5;
  if (v.fitBands) {
    r.b[static_cast<std::size_t>(r.n++)] = {EqType::LowShelf, v.fitLowShelfHz, v.fitLowShelfDb, v.fitLowShelfQ, true};
    r.b[static_cast<std::size_t>(r.n++)] = {EqType::Peak, v.fitMidHz, v.fitMidDb, v.fitMidQ, true};
    r.b[static_cast<std::size_t>(r.n++)] = {EqType::Peak, v.fitCutHz, v.fitCutDb, v.fitCutQ, true};
  }
  if (customTrimsOn(v, p)) {
    r.b[static_cast<std::size_t>(r.n++)] = {EqType::LowShelf, v.customLowHz, p.customLowDb, v.customLowQ, true};
    r.b[static_cast<std::size_t>(r.n++)] = {EqType::HighShelf, v.customHighHz, p.customHighDb, v.customHighQ, true};
  }
  return r;
}

std::array<EqBand, HmColorEq::kNumBands> HmColorEq::bands(double low, double high) {
  HmParams p = HmParams::v2();
  p.low = low;
  p.high = high;
  const Bands r = bands(p);
  std::array<EqBand, kNumBands> out;
  for (int i = 0; i < kNumBands; ++i) out[static_cast<std::size_t>(i)] = r.b[static_cast<std::size_t>(i)];
  return out;
}

void HmColorEq::configure(double fs, double low, double high) {
  const auto b = bands(low, high);
  n_ = kNumBands;
  for (std::size_t i = 0; i < b.size(); ++i) f_[i].setCoeffs(designBiquad(b[i], fs));
  reset();
}

void HmColorEq::setParams(double fs, const HmParams& p) noexcept {
  Bands b = bands(p);
  for (int i = b.n; i < kMaxBands; ++i) f_[static_cast<std::size_t>(i)].reset();
  for (int i = n_; i < b.n; ++i) f_[static_cast<std::size_t>(i)].reset();  // newly active bands start clean
  for (int i = 0; i < b.n; ++i) {
    EqBand& e = b.b[static_cast<std::size_t>(i)];
    e.freq = std::min(e.freq, 0.45 * fs);
    f_[static_cast<std::size_t>(i)].setCoeffs(designBiquad(e, fs));
  }
  n_ = b.n;
}

void HmColorEq::reset() noexcept {
  for (auto& f : f_) f.reset();
}

void HmColorEq::process(float* io, int n) noexcept {
  for (int i = 0; i < n_; ++i) f_[static_cast<std::size_t>(i)].process(io, n);
}

HmPedal::HmPedal(const HmParams& p, PedalImplConfig cfg, const HmVoicing* voicing)
    : v_(voicing ? *voicing : HmVoicing::forVersion(p.modelVersion)), target_(p), applied_(p), cfg_(cfg) {
  hmLiveFromParams(p, liveTarget_.data());
  const int nAdaa = cfg_.adaa ? 2 * AdaaClipper::kLatency : 0;
  if (cfg_.oversample) {
    const int osTotal = Oversampler4x::roundTripLatencyOs() + nAdaa;
    pad_ = osPadding(osTotal);
    latency_ = (osTotal + pad_) / 4;
  } else {
    pad_ = 0;
    latency_ = nAdaa;  // at the base rate, no oversampler
  }
}

void HmPedal::designFilters(const HmParams& p) noexcept {
  const double fs = fs_, fsOs = fsOs_;
  const auto cl = [&](double fc) { return std::min(fc, 0.45 * fsOs); };
  const HmVoicing::Mode& m = v_.mode(p.mode);
  inHpf_.setHighPass(std::min(v_.tightHz0 * std::pow(10.0, p.tightness / v_.tightDecadeDiv), 0.45 * fs), fs);
  lpfInter_.setLowPass(cl(m.interLpfHz), fsOs);
  post_[0].setCoeffs(designBiquad({EqType::LowPass, cl(m.postLpfHz), 0.0, v_.postQ1, true}, fsOs));
  post_[1].setCoeffs(designBiquad({EqType::LowPass, cl(m.postLpfHz), 0.0, v_.postQ2, true}, fsOs));
  if (v_.dcBlockHz > 0.0) dcBlock_.setHighPass(std::min(v_.dcBlockHz, 0.45 * fs), fs);
  eq_.setParams(fs, p);
}

// The knees of one stage: clipShapeSpec, with the voicing's `silicon` knees (v3: asymmetric, fitted) and,
// in custom mode, k- pulled toward k+.
ClipShapeSpec HmPedal::shapeFor(ClipType t, int stage, const HmParams& p) const noexcept {
  ClipShapeSpec s = clipShapeSpec(t);
  if (t == ClipType::Silicon) {
    s.kPos = v_.silKPos;
    s.kNeg = stage == 1 ? v_.silKNeg1 : v_.silKNeg2;
  }
  if (p.mode == HmMode::Custom && v_.customKNegPull != 0.0) s.kNeg -= v_.customKNegPull * (s.kNeg - s.kPos);
  return s;
}

void HmPedal::setShapes(const HmParams& p) noexcept {
  const ClipShapeSpec s1 = shapeFor(p.clip, 1, p);
  const ClipShapeSpec s2 = shapeFor(resolveClip2(p.clip, p.clip2), 2, p);
  const double bf = biasKneeFactor(p.bias);
  clip1_.setShape(s1.kPos, s1.kNeg * bf, s1.order);
  clip2_.setShape(s2.kPos, s2.kNeg * bf, s2.order);
}

// Applies target_: immediate = prepare() (no ramps), else the live path.
void HmPedal::retarget(bool immediate) noexcept {
  const HmParams& n = target_;
  const HmParams& a = applied_;
  const bool eqChanged = immediate || n.mode != a.mode || n.low != a.low || n.high != a.high || n.lowFreq != a.lowFreq ||
                         n.lowQ != a.lowQ || n.highFreq != a.highFreq || n.highSpread != a.highSpread ||
                         n.presenceFreq != a.presenceFreq || n.presenceDb != a.presenceDb || n.rolloffHz != a.rolloffHz;
  if (immediate || n.tightness != a.tightness || n.mode != a.mode || eqChanged) designFilters(n);
  if (immediate || n.clip != a.clip || n.clip2 != a.clip2 || n.bias != a.bias || n.mode != a.mode) setShapes(n);
  const double m = n.mix / 100.0;
  double lvl = static_cast<double>(dbToGain(pedalLevelDb(n.level))) * m;
  if (customTrimsOn(v_, n)) lvl *= static_cast<double>(dbToGain(v_.customOutDb));
  const double g1 = dbToGain(g1Db(v_, n)), g2 = dbToGain(g2Db(v_, n));
  if (immediate) {
    g1_.setImmediate(g1);
    g2_.setImmediate(g2);
    wet_.setImmediate(lvl);
    dry_.setImmediate(1.0 - m);
  } else {
    g1_.setTarget(g1, rampOs_);
    g2_.setTarget(g2, rampOs_);
    wet_.setTarget(lvl, rampBase_);
    dry_.setTarget(1.0 - m, rampBase_);
  }
  applied_ = n;
  dirty_ = false;
}

void HmPedal::setLiveParams(const float* values, int count) noexcept {
  HmParams np = hmParamsFromLive(values, count);
  np.modelVersion = target_.modelVersion;  // the version and the static trims are not live
  np.customLowDb = target_.customLowDb;
  np.customHighDb = target_.customHighDb;
  std::array<float, kHmNumLive> nv;
  hmLiveFromParams(np, nv.data());
  if (nv == liveTarget_) return;  // same values (in float, as the host sends them): nothing to do
  liveTarget_ = nv;
  target_ = np;
  dirty_ = true;
}

void HmPedal::prepare(const ProcessSpec& spec) {
  if (!(spec.sampleRate >= 8000.0) || spec.maxBlockSize < 1)
    throw std::invalid_argument("pedal.hm: unsupported sample rate or block size");
  fs_ = spec.sampleRate;
  fsOs_ = cfg_.oversample ? 4.0 * fs_ : fs_;
  rampBase_ = rampSamplesFor(fs_);
  rampOs_ = rampSamplesFor(fsOs_);

  hpf60_.setHighPass(v_.preHpfHz, fsOs_);
  lpf8k_.setLowPass(std::min(v_.preLpfHz, 0.45 * fsOs_), fsOs_);
  for (AdaaClipper* c : {&clip1_, &clip2_}) c->setAdaa(cfg_.adaa);
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

void HmPedal::reset() {
  inHpf_.reset();
  hpf60_.reset();
  lpf8k_.reset();
  lpfInter_.reset();
  for (auto& b : post_) b.reset();
  clip1_.reset();
  clip2_.reset();
  eq_.reset();
  dcBlock_.reset();
  padDelay_.reset();
  dryDelay_.reset();
  if (cfg_.oversample) os_.reset();
}

void HmPedal::process(float* io, int n) noexcept {
  if (dirty_) retarget(false);
  const bool flat = cfg_.flatFilters;
  dryDelay_.push(io, n);  // the clean mix is tapped before the tightness filter
  if (!flat) inHpf_.process(io, n);
  float* x = io;
  int m = n;
  if (cfg_.oversample) {
    x = osBuf_.data();
    m = 4 * n;
    os_.upsample(io, n, x);
  }
  if (!flat) {
    hpf60_.process(x, m);
    lpf8k_.process(x, m);
  }
  g1_.apply(x, m);
  clip1_.process(x, m);
  if (!flat) lpfInter_.process(x, m);
  g2_.apply(x, m);
  clip2_.process(x, m);
  if (!flat) {
    post_[0].process(x, m);
    post_[1].process(x, m);
  }
  padDelay_.process(x, m);
  if (cfg_.oversample) os_.downsample(x, n, io);
  if (!flat) {
    if (v_.dcBlockHz > 0.0) dcBlock_.process(io, n);
    eq_.process(io, n);
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
