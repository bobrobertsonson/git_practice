#include "sawblade/pedal_ts.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace sawblade {

TsPedal::TsPedal(const TsParams& p, PedalImplConfig cfg) : p_(p), cfg_(cfg) {
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

void TsPedal::prepare(const ProcessSpec& spec) {
  if (!(spec.sampleRate >= 8000.0) || spec.maxBlockSize < 1)
    throw std::invalid_argument("pedal.ts: unsupported sample rate or block size");
  const double fs = spec.sampleRate;
  const double fsOs = cfg_.oversample ? 4.0 * fs : fs;

  const double rd = 51e3 + 500e3 * p_.drive / 10.0;
  const double gd = rd / 4.7e3;
  const double fcFb = std::min(1.0 / (2.0 * std::numbers::pi * rd * 51e-12), 0.45 * fsOs);
  inHpf_.setHighPass(20.0, fs);
  branchHpf_.setHighPass(720.0, fsOs);
  branchLpf_.setLowPass(fcFb, fsOs);
  branchGain_ = static_cast<float>(gd);
  clip_.setShape(0.45, 0.30);
  clip_.setAdaa(cfg_.adaa);
  cleanDelay_.set(clip_.latencySamples());
  padDelay_.set(pad_);
  tone_.setLowPass(std::min(723.0 * std::pow(10.0, p_.tone / 10.0), 0.45 * fs), fs);
  outHpf_.setHighPass(10.0, fs);
  levelGain_ = dbToGain(pedalLevelDb(p_.level));

  if (cfg_.oversample) {
    os_.prepare(spec.maxBlockSize);
    osBuf_.assign(static_cast<std::size_t>(4 * spec.maxBlockSize), 0.0f);
    branchBuf_.assign(static_cast<std::size_t>(4 * spec.maxBlockSize), 0.0f);
  } else {
    osBuf_.clear();
    branchBuf_.assign(static_cast<std::size_t>(spec.maxBlockSize), 0.0f);
  }
  reset();
}

void TsPedal::reset() {
  inHpf_.reset();
  branchHpf_.reset();
  branchLpf_.reset();
  tone_.reset();
  outHpf_.reset();
  clip_.reset();
  cleanDelay_.reset();
  padDelay_.reset();
  if (cfg_.oversample) os_.reset();
}

void TsPedal::process(float* io, int n) noexcept {
  const bool flat = cfg_.flatFilters;
  if (!flat) inHpf_.process(io, n);
  float* x = io;
  int m = n;
  if (cfg_.oversample) {
    x = osBuf_.data();
    m = 4 * n;
    os_.upsample(io, n, x);
  }
  float* v = branchBuf_.data();
  std::copy(x, x + m, v);
  if (!flat) {
    branchHpf_.process(v, m);
    branchLpf_.process(v, m);
  }
  for (int i = 0; i < m; ++i) v[i] *= branchGain_;
  clip_.process(v, m);
  cleanDelay_.process(x, m);  // align the clean signal with the ADAA-delayed branch
  for (int i = 0; i < m; ++i) x[i] += v[i];
  padDelay_.process(x, m);
  if (cfg_.oversample) os_.downsample(x, n, io);
  if (!flat) {
    tone_.process(io, n);
    outHpf_.process(io, n);
  }
  for (int i = 0; i < n; ++i) io[i] *= levelGain_;
}

}  // namespace sawblade
