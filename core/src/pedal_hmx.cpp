#include "sawblade/pedal_hmx.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sawblade {
namespace {

const HmVoicing::Mode& stockMode() noexcept { return HmVoicing::kModes[0]; }
const HmParams& stock() noexcept {
  static const HmParams p{};
  return p;
}

double g1Db(const HmxParams& p) noexcept {
  return HmVoicing::g1BaseDb + stockMode().s1 * p.distortion + (p.boost ? HmxConstants::boostDb : 0.0);
}

}  // namespace

std::array<EqBand, HmxPedal::kNumEqBands> HmxPedal::eqBands(const HmxParams& p) {
  using K = HmxConstants;
  const double fLM = K::lowMidBaseHz * std::pow(K::lowMidOctaveRatio, p.lowMidFreq / 10.0);
  const double fHM = K::highMidBaseHz * std::pow(K::highMidRatio, (p.highMidFreq - 5.0) / 5.0);
  const double hi = HmVoicing::highBaseDb;
  return {{
      {EqType::Peak, stock().lowFreq, HmVoicing::lowBaseDb + stockMode().sLow * p.low, stock().lowQ, true},  // 11 low gyrator
      {EqType::Peak, fLM, K::lowMidDbPerUnit * (p.lowMid - 5.0), K::lowMidQ, true},                          // 12 low-mid
      {EqType::Peak, fHM, hi + HmVoicing::highSlope * p.highMid, HmVoicing::highQ, true},                    // 13 high-mid (gyrator A, movable)
      {EqType::Peak, stock().highFreq * stock().highSpread, hi + HmVoicing::highSlope * p.high, HmVoicing::highQ, true},  // 14 high (gyrator B)
      {EqType::Peak, stock().presenceFreq, stock().presenceDb, HmVoicing::presenceQ, true},                  // 15 fixed presence peak
      {EqType::HighShelf, K::presenceShelfHz, K::presenceDbPerUnit * (p.presence - 5.0), K::presenceShelfQ, true},  // 16 presence shelf
      {EqType::LowPass, stock().rolloffHz, 0.0, HmVoicing::rolloffQ, true},                                  // 17 roll-off
  }};
}

HmxPedal::HmxPedal(const HmxParams& p, PedalImplConfig cfg) : target_(p), applied_(p), cfg_(cfg) {
  hmxLiveFromParams(p, liveTarget_.data());
  const int nAdaa = cfg_.adaa ? 2 * AdaaClipper::kLatency : 0;
  if (cfg_.oversample) {
    const int osTotal = Oversampler4x::roundTripLatencyOs() + nAdaa;
    pad_ = osPadding(osTotal);
    latency_ = (osTotal + pad_) / 4;
  } else {
    pad_ = 0;
    latency_ = nAdaa;
  }
}

void HmxPedal::designFilters(const HmxParams& p) noexcept {
  const double fs = fs_;
  const auto clb = [&](double fc) { return std::min(fc, 0.45 * fs); };
  inHpf_.setHighPass(clb(HmVoicing::tightHz0 * std::pow(10.0, p.tightness / HmVoicing::tightDecadeDiv)), fs);
  auto bands = eqBands(p);
  for (std::size_t i = 0; i < bands.size(); ++i) {
    bands[i].freq = clb(bands[i].freq);
    eq_[i].setCoeffs(designBiquad(bands[i], fs));
  }
}

// Applies target_: immediate = prepare() (no ramps), else the live path.
void HmxPedal::retarget(bool immediate) noexcept {
  const HmxParams& n = target_;
  const HmxParams& a = applied_;
  if (immediate || n.tightness != a.tightness || n.low != a.low || n.lowMid != a.lowMid || n.lowMidFreq != a.lowMidFreq ||
      n.highMid != a.highMid || n.highMidFreq != a.highMidFreq || n.high != a.high || n.presence != a.presence)
    designFilters(n);
  if (immediate || n.clip != a.clip) {
    const ClipShapeSpec s = clipShapeSpec(n.clip);
    for (AdaaClipper* c : {&clip1_, &clip2_}) c->setShape(s.kPos, s.kNeg, s.order);
  }
  const double m = n.mix / 100.0;
  const double lvl = static_cast<double>(dbToGain(pedalLevelDb(n.level))) * m;
  const double g1 = dbToGain(g1Db(n));
  if (immediate) {
    g1_.setImmediate(g1);
    wet_.setImmediate(lvl);
    dry_.setImmediate(1.0 - m);
  } else {
    g1_.setTarget(g1, rampOs_);
    wet_.setTarget(lvl, rampBase_);
    dry_.setTarget(1.0 - m, rampBase_);
  }
  applied_ = n;
  dirty_ = false;
}

void HmxPedal::setLiveParams(const float* values, int count) noexcept {
  const HmxParams np = hmxParamsFromLive(values, count);
  std::array<float, kHmxNumLive> nv;
  hmxLiveFromParams(np, nv.data());
  if (nv == liveTarget_) return;
  liveTarget_ = nv;
  target_ = np;
  dirty_ = true;
}

void HmxPedal::prepare(const ProcessSpec& spec) {
  if (!(spec.sampleRate >= 8000.0) || spec.maxBlockSize < 1)
    throw std::invalid_argument("pedal.hmx: unsupported sample rate or block size");
  fs_ = spec.sampleRate;
  fsOs_ = cfg_.oversample ? 4.0 * fs_ : fs_;
  rampBase_ = rampSamplesFor(fs_);
  rampOs_ = rampSamplesFor(fsOs_);
  const auto cl = [&](double fc) { return std::min(fc, 0.45 * fsOs_); };
  const HmVoicing::Mode& m0 = stockMode();

  hpf60_.setHighPass(HmVoicing::preHpfHz, fsOs_);
  lpf8k_.setLowPass(cl(HmVoicing::preLpfHz), fsOs_);
  lpf5k_.setLowPass(cl(m0.interLpfHz), fsOs_);
  post_[0].setCoeffs(designBiquad({EqType::LowPass, cl(m0.postLpfHz), 0.0, HmVoicing::postQ1, true}, fsOs_));
  post_[1].setCoeffs(designBiquad({EqType::LowPass, cl(m0.postLpfHz), 0.0, HmVoicing::postQ2, true}, fsOs_));
  dcBlock_.setHighPass(HmxConstants::dcBlockHz, fs_);
  g2_ = dbToGain(HmVoicing::interstageDb);
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

void HmxPedal::reset() {
  inHpf_.reset();
  hpf60_.reset();
  lpf8k_.reset();
  lpf5k_.reset();
  dcBlock_.reset();
  for (auto& b : post_) b.reset();
  for (auto& b : eq_) b.reset();
  clip1_.reset();
  clip2_.reset();
  padDelay_.reset();
  dryDelay_.reset();
  if (cfg_.oversample) os_.reset();
}

void HmxPedal::process(float* io, int n) noexcept {
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
  if (!flat) lpf5k_.process(x, m);
  for (int i = 0; i < m; ++i) x[i] *= g2_;
  clip2_.process(x, m);
  if (!flat) {
    post_[0].process(x, m);
    post_[1].process(x, m);
  }
  padDelay_.process(x, m);
  if (cfg_.oversample) os_.downsample(x, n, io);
  if (!flat) {
    dcBlock_.process(io, n);
    for (auto& b : eq_) b.process(io, n);
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

std::unique_ptr<Processor> createHmx(const Block& b, const BlockBuildContext&) {
  return std::make_unique<HmxPedal>(static_cast<const HmxBlockParams&>(*b.params).p);
}

}  // namespace sawblade
