#include "sawblade/pedal_hm.h"

#include <algorithm>
#include <stdexcept>

namespace sawblade {
namespace {

const HmVoicing::Mode& modeOf(HmMode m) noexcept { return HmVoicing::kModes[static_cast<std::size_t>(m)]; }

double g1Db(const HmParams& p) noexcept { return HmVoicing::g1BaseDb + modeOf(p.mode).s1 * p.distortion + p.gain1Db; }
double g2Db(const HmParams& p) noexcept { return HmVoicing::interstageDb + p.gain2Db; }

}  // namespace

std::array<EqBand, HmColorEq::kNumBands> HmColorEq::bands(const HmParams& p) {
  const double hi = HmVoicing::highBaseDb + HmVoicing::highSlope * p.high;
  return {{
      {EqType::Peak, p.lowFreq, HmVoicing::lowBaseDb + modeOf(p.mode).sLow * p.low, p.lowQ, true},  // low gyrator
      {EqType::Peak, p.highFreq, hi, HmVoicing::highQ, true},                                       // high gyrator A
      {EqType::Peak, p.highFreq * p.highSpread, hi, HmVoicing::highQ, true},                        // high gyrator B
      {EqType::Peak, p.presenceFreq, p.presenceDb, HmVoicing::presenceQ, true},                     // presence peak
      {EqType::LowPass, p.rolloffHz, 0.0, HmVoicing::rolloffQ, true},                               // output roll-off
  }};
}

std::array<EqBand, HmColorEq::kNumBands> HmColorEq::bands(double low, double high) {
  HmParams p;
  p.low = low;
  p.high = high;
  return bands(p);
}

void HmColorEq::configure(double fs, double low, double high) {
  const auto b = bands(low, high);
  for (std::size_t i = 0; i < b.size(); ++i) f_[i].setCoeffs(designBiquad(b[i], fs));
  reset();
}

void HmColorEq::setParams(double fs, const HmParams& p) noexcept {
  auto b = bands(p);
  for (std::size_t i = 0; i < b.size(); ++i) {
    b[i].freq = std::min(b[i].freq, 0.45 * fs);
    f_[i].setCoeffs(designBiquad(b[i], fs));
  }
}

void HmColorEq::reset() noexcept {
  for (auto& f : f_) f.reset();
}

void HmColorEq::process(float* io, int n) noexcept {
  for (auto& f : f_) f.process(io, n);
}

HmPedal::HmPedal(const HmParams& p, PedalImplConfig cfg) : target_(p), applied_(p), cfg_(cfg) {
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
  const HmVoicing::Mode& m = modeOf(p.mode);
  inHpf_.setHighPass(std::min(HmVoicing::tightHz0 * std::pow(10.0, p.tightness / HmVoicing::tightDecadeDiv), 0.45 * fs), fs);
  lpfInter_.setLowPass(cl(m.interLpfHz), fsOs);
  post_[0].setCoeffs(designBiquad({EqType::LowPass, cl(m.postLpfHz), 0.0, HmVoicing::postQ1, true}, fsOs));
  post_[1].setCoeffs(designBiquad({EqType::LowPass, cl(m.postLpfHz), 0.0, HmVoicing::postQ2, true}, fsOs));
  eq_.setParams(fs, p);
}

void HmPedal::setShapes(const HmParams& p) noexcept {
  const ClipShapeSpec s1 = clipShapeSpec(p.clip);
  const ClipShapeSpec s2 = clipShapeSpec(resolveClip2(p.clip, p.clip2));
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
  if (immediate || n.clip != a.clip || n.clip2 != a.clip2 || n.bias != a.bias) setShapes(n);
  const double m = n.mix / 100.0;
  const double lvl = static_cast<double>(dbToGain(pedalLevelDb(n.level))) * m;
  const double g1 = dbToGain(g1Db(n)), g2 = dbToGain(g2Db(n));
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
  const HmParams np = hmParamsFromLive(values, count);
  if (np == target_) return;
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

  hpf60_.setHighPass(HmVoicing::preHpfHz, fsOs_);
  lpf8k_.setLowPass(std::min(HmVoicing::preLpfHz, 0.45 * fsOs_), fsOs_);
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
  if (!flat) eq_.process(io, n);
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
