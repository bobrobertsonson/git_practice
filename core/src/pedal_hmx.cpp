#include "sawblade/pedal_hmx.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sawblade {

std::array<EqBand, HmxPedal::kNumEqBands> HmxPedal::eqBands(const HmxParams& p) {
  const double fLM = 200.0 * std::pow(3.0, p.lowMidFreq / 10.0);
  const double fHM = 1000.0 * std::pow(1.6, (p.highMidFreq - 5.0) / 5.0);
  return {{
      {EqType::Peak, 100.0, -12.0 + 3.0 * p.low, 0.8, true},         // 11 low gyrator
      {EqType::Peak, fLM, 2.0 * (p.lowMid - 5.0), 1.0, true},        // 12 low-mid
      {EqType::Peak, fHM, -8.0 + 2.2 * p.highMid, 1.2, true},        // 13 high-mid (gyrator A, movable)
      {EqType::Peak, 1500.0, -8.0 + 2.2 * p.high, 1.2, true},        // 14 high (gyrator B)
      {EqType::Peak, 4800.0, 8.0, 2.0, true},                        // 15 fixed presence peak
      {EqType::HighShelf, 3500.0, 1.2 * (p.presence - 5.0), 0.7071067811865476, true},  // 16 presence shelf
      {EqType::LowPass, 9000.0, 0.0, 0.707, true},                   // 17 roll-off
  }};
}

HmxPedal::HmxPedal(const HmxParams& p, PedalImplConfig cfg) : p_(p), cfg_(cfg) {
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

void HmxPedal::prepare(const ProcessSpec& spec) {
  if (!(spec.sampleRate >= 8000.0) || spec.maxBlockSize < 1)
    throw std::invalid_argument("pedal.hmx: unsupported sample rate or block size");
  const double fs = spec.sampleRate;
  const double fsOs = cfg_.oversample ? 4.0 * fs : fs;
  const auto cl = [&](double fc) { return std::min(fc, 0.45 * fsOs); };
  const auto clb = [&](double fc) { return std::min(fc, 0.45 * fs); };

  inHpf_.setHighPass(clb(20.0 * std::pow(10.0, p_.tightness / 10.0)), fs);
  hpf60_.setHighPass(60.0, fsOs);
  lpf8k_.setLowPass(cl(8000.0), fsOs);
  lpf5k_.setLowPass(cl(5000.0), fsOs);
  post_[0].setCoeffs(designBiquad({EqType::LowPass, cl(6500.0), 0.0, 0.5411961001461969, true}, fsOs));
  post_[1].setCoeffs(designBiquad({EqType::LowPass, cl(6500.0), 0.0, 1.3065629648763766, true}, fsOs));
  dcBlock_.setHighPass(10.0, fs);
  auto bands = eqBands(p_);
  for (std::size_t i = 0; i < bands.size(); ++i) {
    bands[i].freq = clb(bands[i].freq);
    eq_[i].setCoeffs(designBiquad(bands[i], fs));
  }

  const auto knees = stages::clipKnees(p_.clip);
  for (AdaaClipper* c : {&clip1_, &clip2_}) {
    c->setShape(knees.kPos, knees.kNeg);
    c->setAdaa(cfg_.adaa);
  }
  g1_ = dbToGain(6.0 + 4.0 * p_.distortion + (p_.boost ? 9.0 : 0.0));
  const double m = p_.mix / 100.0;
  mixDry_ = p_.mix < 100.0;
  wetGain_ = mixDry_ ? static_cast<float>(m * static_cast<double>(dbToGain(pedalLevelDb(p_.level)))) : dbToGain(pedalLevelDb(p_.level));
  dryGain_ = static_cast<float>(1.0 - m);
  padDelay_.set(pad_);
  dryDelay_.set(latency_);
  if (cfg_.oversample) {
    os_.prepare(spec.maxBlockSize);
    osBuf_.assign(static_cast<std::size_t>(4 * spec.maxBlockSize), 0.0f);
  } else {
    osBuf_.clear();
  }
  dryBuf_.assign(mixDry_ ? static_cast<std::size_t>(spec.maxBlockSize) : 0u, 0.0f);
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
  const bool flat = cfg_.flatFilters;
  if (mixDry_) {
    std::copy(io, io + n, dryBuf_.data());
    dryDelay_.process(dryBuf_.data(), n);
  }
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
  for (int i = 0; i < m; ++i) x[i] *= g1_;
  clip1_.process(x, m);
  if (!flat) lpf5k_.process(x, m);
  for (int i = 0; i < m; ++i) x[i] *= 10.0f;  // +20 dB interstage gain
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
  if (mixDry_) {
    const float* d = dryBuf_.data();
    for (int i = 0; i < n; ++i) io[i] = dryGain_ * d[i] + wetGain_ * io[i];
  } else {
    for (int i = 0; i < n; ++i) io[i] *= wetGain_;
  }
}

std::unique_ptr<Processor> createHmx(const Block& b, const BlockBuildContext&) {
  return std::make_unique<HmxPedal>(static_cast<const HmxBlockParams&>(*b.params).p);
}

}  // namespace sawblade
