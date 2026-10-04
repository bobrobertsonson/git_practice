#include "sawblade/pedal_eye.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sawblade {

EyePedal::EyePedal(const EyeParams& p, PedalImplConfig cfg) : target_(p), applied_(p), cfg_(cfg) {
  eyeLiveFromParams(p, liveTarget_.data());
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

void EyePedal::retarget(bool immediate) noexcept {
  const EyeParams& n = target_;
  const EyeParams& a = applied_;
  if (immediate || n.tightness != a.tightness)
    inHpf_.setHighPass(std::min(HmVoicing::tightHz0 * std::pow(10.0, n.tightness / HmVoicing::tightDecadeDiv), 0.45 * fs_), fs_);
  const double g1 = dbToGain(EyeConstants::g1BaseDb + EyeConstants::g1DbPerUnit * n.gain);
  const double lvl = dbToGain(pedalLevelDb(n.level));
  if (immediate) {
    g1_.setImmediate(g1);
    level_.setImmediate(lvl);
  } else {
    g1_.setTarget(g1, rampOs_);
    level_.setTarget(lvl, rampBase_);
  }
  applied_ = n;
  dirty_ = false;
}

void EyePedal::setLiveParams(const float* values, int count) noexcept {
  const EyeParams np = eyeParamsFromLive(values, count);
  std::array<float, kEyeNumLive> nv;
  eyeLiveFromParams(np, nv.data());
  if (nv == liveTarget_) return;
  liveTarget_ = nv;
  target_ = np;
  dirty_ = true;
}

void EyePedal::prepare(const ProcessSpec& spec) {
  if (!(spec.sampleRate >= 8000.0) || spec.maxBlockSize < 1)
    throw std::invalid_argument("pedal.eye: unsupported sample rate or block size");
  fs_ = spec.sampleRate;
  fsOs_ = cfg_.oversample ? 4.0 * fs_ : fs_;
  rampBase_ = rampSamplesFor(fs_);
  rampOs_ = rampSamplesFor(fsOs_);
  const auto cl = [&](double fc) { return std::min(fc, 0.45 * fsOs_); };
  const HmVoicing::Mode& m0 = HmVoicing::kModes[0];

  hpf100_.setHighPass(EyeConstants::preHpfHz, fsOs_);
  lpf8k_.setLowPass(cl(HmVoicing::preLpfHz), fsOs_);
  lpf5k_.setLowPass(cl(m0.interLpfHz), fsOs_);
  post_[0].setCoeffs(designBiquad({EqType::LowPass, cl(m0.postLpfHz), 0.0, HmVoicing::postQ1, true}, fsOs_));
  post_[1].setCoeffs(designBiquad({EqType::LowPass, cl(m0.postLpfHz), 0.0, HmVoicing::postQ2, true}, fsOs_));
  eq_.configure(fs_, EyeConstants::eqLow, EyeConstants::eqHigh);
  g2_ = dbToGain(HmVoicing::interstageDb);
  const ClipShapeSpec s = clipShapeSpec(ClipType::Silicon);
  for (AdaaClipper* c : {&clip1_, &clip2_}) {
    c->setShape(s.kPos, s.kNeg, s.order);
    c->setAdaa(cfg_.adaa);
  }
  retarget(true);
  padDelay_.set(pad_);
  if (cfg_.oversample) {
    os_.prepare(spec.maxBlockSize);
    osBuf_.assign(static_cast<std::size_t>(4 * spec.maxBlockSize), 0.0f);
  } else {
    osBuf_.clear();
  }
  reset();
}

void EyePedal::reset() {
  inHpf_.reset();
  hpf100_.reset();
  lpf8k_.reset();
  lpf5k_.reset();
  for (auto& b : post_) b.reset();
  clip1_.reset();
  clip2_.reset();
  eq_.reset();
  padDelay_.reset();
  if (cfg_.oversample) os_.reset();
}

void EyePedal::process(float* io, int n) noexcept {
  if (dirty_) retarget(false);
  const bool flat = cfg_.flatFilters;
  if (!flat) inHpf_.process(io, n);
  float* x = io;
  int m = n;
  if (cfg_.oversample) {
    x = osBuf_.data();
    m = 4 * n;
    os_.upsample(io, n, x);
  }
  if (!flat) {
    hpf100_.process(x, m);
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
  if (!flat) eq_.process(io, n);
  level_.apply(io, n);
}

std::unique_ptr<Processor> createEye(const Block& b, const BlockBuildContext&) {
  return std::make_unique<EyePedal>(static_cast<const EyeBlockParams&>(*b.params).p);
}

}  // namespace sawblade
