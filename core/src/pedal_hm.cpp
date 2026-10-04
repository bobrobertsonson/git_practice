#include "sawblade/pedal_hm.h"

#include <algorithm>
#include <stdexcept>

namespace sawblade {

std::array<EqBand, HmColorEq::kNumBands> HmColorEq::bands(double low, double high) {
  const double hi = -8.0 + 2.2 * high;
  return {{
      {EqType::Peak, 100.0, -12.0 + 3.0 * low, 0.8, true},   // low gyrator
      {EqType::Peak, 1000.0, hi, 1.2, true},                 // high gyrator A
      {EqType::Peak, 1500.0, hi, 1.2, true},                 // high gyrator B
      {EqType::Peak, 4800.0, 8.0, 2.0, true},                // fixed HF presence peak
      {EqType::LowPass, 9000.0, 0.0, 0.707, true},           // output roll-off
  }};
}

void HmColorEq::configure(double fs, double low, double high) {
  const auto b = bands(low, high);
  for (std::size_t i = 0; i < b.size(); ++i) f_[i].setCoeffs(designBiquad(b[i], fs));
  reset();
}

void HmColorEq::reset() noexcept {
  for (auto& f : f_) f.reset();
}

void HmColorEq::process(float* io, int n) noexcept {
  for (auto& f : f_) f.process(io, n);
}

HmPedal::HmPedal(const HmParams& p, PedalImplConfig cfg) : p_(p), cfg_(cfg) {
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

void HmPedal::prepare(const ProcessSpec& spec) {
  if (!(spec.sampleRate >= 8000.0) || spec.maxBlockSize < 1)
    throw std::invalid_argument("pedal.hm: unsupported sample rate or block size");
  const double fs = spec.sampleRate;
  const double fsOs = cfg_.oversample ? 4.0 * fs : fs;
  const auto cl = [&](double fc) { return std::min(fc, 0.45 * fsOs); };

  inHpf_.setHighPass(20.0, fs);
  hpf60_.setHighPass(60.0, fsOs);
  lpf8k_.setLowPass(cl(8000.0), fsOs);
  lpf5k_.setLowPass(cl(5000.0), fsOs);
  // 4th-order Butterworth 6.5 kHz as two RBJ low-pass sections.
  post_[0].setCoeffs(designBiquad({EqType::LowPass, cl(6500.0), 0.0, 0.5411961001461969, true}, fsOs));
  post_[1].setCoeffs(designBiquad({EqType::LowPass, cl(6500.0), 0.0, 1.3065629648763766, true}, fsOs));
  eq_.configure(fs, p_.low, p_.high);

  g1_ = dbToGain(6.0 + 4.0 * p_.distortion);
  levelGain_ = dbToGain(pedalLevelDb(p_.level));
  for (AdaaClipper* c : {&clip1_, &clip2_}) {
    c->setShape(0.5, 0.5);  // silicon diode pair, symmetric
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

void HmPedal::reset() {
  inHpf_.reset();
  hpf60_.reset();
  lpf8k_.reset();
  lpf5k_.reset();
  for (auto& b : post_) b.reset();
  clip1_.reset();
  clip2_.reset();
  eq_.reset();
  padDelay_.reset();
  if (cfg_.oversample) os_.reset();
}

void HmPedal::process(float* io, int n) noexcept {
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
  if (!flat) eq_.process(io, n);
  for (int i = 0; i < n; ++i) io[i] *= levelGain_;
}

}  // namespace sawblade
