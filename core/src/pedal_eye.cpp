#include "sawblade/pedal_eye.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sawblade {

EyePedal::EyePedal(const EyeParams& p, PedalImplConfig cfg) : p_(p), cfg_(cfg) {
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

void EyePedal::prepare(const ProcessSpec& spec) {
  if (!(spec.sampleRate >= 8000.0) || spec.maxBlockSize < 1)
    throw std::invalid_argument("pedal.eye: unsupported sample rate or block size");
  const double fs = spec.sampleRate;
  const double fsOs = cfg_.oversample ? 4.0 * fs : fs;
  const auto cl = [&](double fc) { return std::min(fc, 0.45 * fsOs); };

  inHpf_.setHighPass(std::min(20.0 * std::pow(10.0, p_.tightness / 10.0), 0.45 * fs), fs);
  hpf100_.setHighPass(100.0, fsOs);
  lpf8k_.setLowPass(cl(8000.0), fsOs);
  lpf5k_.setLowPass(cl(5000.0), fsOs);
  post_[0].setCoeffs(designBiquad({EqType::LowPass, cl(6500.0), 0.0, 0.5411961001461969, true}, fsOs));
  post_[1].setCoeffs(designBiquad({EqType::LowPass, cl(6500.0), 0.0, 1.3065629648763766, true}, fsOs));
  eq_.configure(fs, 10.0, 10.0);

  g1_ = dbToGain(10.0 + 4.2 * p_.gain);
  levelGain_ = dbToGain(pedalLevelDb(p_.level));
  for (AdaaClipper* c : {&clip1_, &clip2_}) {
    c->setShape(0.5, 0.5);  // silicon
    c->setAdaa(cfg_.adaa);
  }
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
  for (int i = 0; i < m; ++i) x[i] *= g1_;
  clip1_.process(x, m);
  if (!flat) lpf5k_.process(x, m);
  for (int i = 0; i < m; ++i) x[i] *= 10.0f;
  clip2_.process(x, m);
  if (!flat) {
    post_[0].process(x, m);
    post_[1].process(x, m);
  }
  padDelay_.process(x, m);
  if (cfg_.oversample) os_.downsample(x, n, io);
  if (!flat) eq_.process(io, n);
  for (int i = 0; i < n; ++i) io[i] *= levelGain_;
}

std::unique_ptr<Processor> createEye(const Block& b, const BlockBuildContext&) {
  return std::make_unique<EyePedal>(static_cast<const EyeBlockParams&>(*b.params).p);
}

}  // namespace sawblade
