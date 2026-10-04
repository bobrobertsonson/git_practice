#include "sawblade/pedal_muff.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sawblade {
namespace {

double gainADb(const MuffParams& p) noexcept { return MuffVoicing::gainABaseDb + MuffVoicing::gainASlope * p.sustain; }
double gainBDb(const MuffParams& p) noexcept { return MuffVoicing::gainBDb + p.gain2Db; }
double stackCentre(const MuffParams& p) noexcept { return MuffVoicing::voiceCentreHz * std::pow(2.0, (p.voice - 5.0) / 5.0); }

}  // namespace

MuffPedal::MuffPedal(const MuffParams& p, PedalImplConfig cfg) : target_(p), applied_(p), cfg_(cfg) {
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

void MuffPedal::designFilters(const MuffParams& p) noexcept {
  const double fs = fs_;
  const auto clb = [&](double fc) { return std::min(fc, 0.45 * fs); };
  inHpf_.setHighPass(clb(MuffVoicing::tightHz0 * std::pow(10.0, p.tightness / MuffVoicing::tightDecadeDiv)), fs);
  const double c = stackCentre(p), r = std::sqrt(p.stackRatio);
  stackLp_.setLowPass(clb(c / r), fs);
  stackHp_.setHighPass(clb(c * r), fs);
  scoop_.setCoeffs(designBiquad({EqType::Peak, clb(c), MuffVoicing::scoopDbPerUnit * p.scoop, MuffVoicing::peakQ, true}, fs));
  rolloff_.setLowPass(clb(p.rolloffHz), fs);
}

void MuffPedal::setShapes(const MuffParams& p) noexcept {
  const ClipShapeSpec sa = clipShapeSpec(p.clip);
  const ClipShapeSpec sb = clipShapeSpec(resolveClip2(p.clip, p.clip2));
  const double cr = MuffVoicing::crunchBase - MuffVoicing::crunchSlope * p.crunch;
  const double bf = biasKneeFactor(p.bias);
  clipA_.setShape(sa.kPos * cr, sa.kNeg * bf * cr, sa.order);
  clipB_.setShape(sb.kPos * cr, sb.kNeg * bf * cr, sb.order);
}

void MuffPedal::retarget(bool immediate) noexcept {
  const MuffParams& n = target_;
  const MuffParams& a = applied_;
  if (immediate || n.tightness != a.tightness || n.voice != a.voice || n.stackRatio != a.stackRatio ||
      n.scoop != a.scoop || n.rolloffHz != a.rolloffHz)
    designFilters(n);
  if (immediate || n.clip != a.clip || n.clip2 != a.clip2 || n.bias != a.bias || n.crunch != a.crunch) setShapes(n);
  const double m = n.mix / 100.0;
  const double lvl = static_cast<double>(dbToGain(pedalLevelDb(n.volume) + MuffVoicing::recoveryDb)) * m;
  const double ga = dbToGain(gainADb(n)), gb = dbToGain(gainBDb(n));
  const double t = n.tone / 10.0;
  if (immediate) {
    gA_.setImmediate(ga);
    gB_.setImmediate(gb);
    wet_.setImmediate(lvl);
    dry_.setImmediate(1.0 - m);
    tone_.setImmediate(t);
  } else {
    gA_.setTarget(ga, rampOs_);
    gB_.setTarget(gb, rampOs_);
    wet_.setTarget(lvl, rampBase_);
    dry_.setTarget(1.0 - m, rampBase_);
    tone_.setTarget(t, rampBase_);
  }
  applied_ = n;
  dirty_ = false;
}

void MuffPedal::setLiveParams(const float* values, int count) noexcept {
  const MuffParams np = muffParamsFromLive(values, count);
  if (np == target_) return;
  target_ = np;
  dirty_ = true;
}

void MuffPedal::prepare(const ProcessSpec& spec) {
  if (!(spec.sampleRate >= 8000.0) || spec.maxBlockSize < 1)
    throw std::invalid_argument("pedal.muff: unsupported sample rate or block size");
  fs_ = spec.sampleRate;
  fsOs_ = cfg_.oversample ? 4.0 * fs_ : fs_;
  rampBase_ = rampSamplesFor(fs_);
  rampOs_ = rampSamplesFor(fsOs_);
  const auto cl = [&](double fc) { return std::min(fc, 0.45 * fsOs_); };
  aHpf_.setHighPass(MuffVoicing::aHpfHz, fsOs_);
  aLpf_.setLowPass(cl(MuffVoicing::aLpfHz), fsOs_);
  bHpf_.setHighPass(MuffVoicing::bHpfHz, fsOs_);
  bLpf_.setLowPass(cl(MuffVoicing::bLpfHz), fsOs_);
  post_.setCoeffs(designBiquad({EqType::LowPass, cl(MuffVoicing::postLpfHz), 0.0, MuffVoicing::postQ, true}, fsOs_));
  for (AdaaClipper* c : {&clipA_, &clipB_}) c->setAdaa(cfg_.adaa);
  retarget(true);
  padDelay_.set(pad_);
  dryDelay_.prepare(latency_, spec.maxBlockSize);
  tmp_.assign(static_cast<std::size_t>(spec.maxBlockSize), 0.0f);
  if (cfg_.oversample) {
    os_.prepare(spec.maxBlockSize);
    osBuf_.assign(static_cast<std::size_t>(4 * spec.maxBlockSize), 0.0f);
  } else {
    osBuf_.clear();
  }
  reset();
}

void MuffPedal::reset() {
  for (OnePole* f : {&inHpf_, &aHpf_, &aLpf_, &bHpf_, &bLpf_, &stackLp_, &stackHp_, &rolloff_}) f->reset();
  post_.reset();
  scoop_.reset();
  clipA_.reset();
  clipB_.reset();
  padDelay_.reset();
  dryDelay_.reset();
  if (cfg_.oversample) os_.reset();
}

void MuffPedal::process(float* io, int n) noexcept {
  if (dirty_) retarget(false);
  const bool flat = cfg_.flatFilters;
  dryDelay_.push(io, n);
  if (!flat) inHpf_.process(io, n);
  float* x = io;
  int m = n;
  if (cfg_.oversample) {
    x = osBuf_.data();
    m = 4 * n;
    os_.upsample(io, n, x);
  }
  if (!flat) {
    aHpf_.process(x, m);
    aLpf_.process(x, m);
  }
  gA_.apply(x, m);
  clipA_.process(x, m);
  if (!flat) {
    bHpf_.process(x, m);
    bLpf_.process(x, m);
  }
  gB_.apply(x, m);
  clipB_.process(x, m);
  if (!flat) post_.process(x, m);
  padDelay_.process(x, m);
  if (cfg_.oversample) os_.downsample(x, n, io);
  if (!flat) {
    // Passive stack: (1 - t) * LPF(fL) + t * HPF(fH), then the scoop notch and the recovery roll-off.
    float* hp = tmp_.data();
    std::copy(io, io + n, hp);
    stackLp_.process(io, n);
    stackHp_.process(hp, n);
    for (int i = 0; i < n; ++i) {
      const float t = tone_.next();
      io[i] = (1.0f - t) * io[i] + t * hp[i];
    }
    scoop_.process(io, n);
    rolloff_.process(io, n);
  }
  if (!dry_.active() && dry_.cur == 0.0) {
    wet_.apply(io, n);
  } else {
    for (int i = 0; i < n; ++i) {
      const float wg = wet_.next(), dg = dry_.next();
      io[i] = dg * dryDelay_.tap(i) + wg * io[i];
    }
  }
  dryDelay_.commit(n);
}

}  // namespace sawblade
