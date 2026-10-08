#include "sawblade/oversampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace sawblade {
namespace {

double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  const double q = x * x / 4.0;
  for (int k = 1; k < 200; ++k) {
    term *= q / (static_cast<double>(k) * k);
    sum += term;
    if (term < 1e-18 * sum) break;
  }
  return sum;
}

}  // namespace

double Oversampler4x::kaiserBetaFor(double a) {
  if (a > 50.0) return 0.1102 * (a - 8.7);
  if (a >= 21.0) return 0.5842 * std::pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0);
  return 0.0;
}

std::vector<double> Oversampler4x::designHalfBand(int numTaps, double beta) {
  if (numTaps < 3 || numTaps % 4 != 3) throw std::invalid_argument("half-band taps must be 4m+3");
  const int c = (numTaps - 1) / 2;
  std::vector<double> h(static_cast<std::size_t>(numTaps), 0.0);
  const double i0b = besselI0(beta);
  double oddSum = 0.0;
  for (int j = 0; j < numTaps; ++j) {
    const int t = j - c;
    if (t == 0 || t % 2 == 0) continue;  // zeros at even offsets; centre handled below
    const double ideal = std::sin(std::numbers::pi * t / 2.0) / (std::numbers::pi * t);
    const double r = static_cast<double>(t) / c;
    const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
    h[static_cast<std::size_t>(j)] = ideal * w;
    oddSum += ideal * w;
  }
  // Exact DC gain 1 with the centre tap exactly 0.5 (the half-band property).
  const double s = 0.5 / oddSum;
  for (int j = 0; j < numTaps; ++j)
    if ((j - c) % 2 != 0) h[static_cast<std::size_t>(j)] *= s;
  h[static_cast<std::size_t>(c)] = 0.5;
  return h;
}

void Oversampler4x::design(Stage& s, int taps, bool up) {
  const auto h = designHalfBand(taps, kaiserBetaFor(kDesignStopbandDb));
  s.taps = taps;
  s.m = (taps - 3) / 4;
  s.k = 2 * s.m + 2;
  s.even.resize(static_cast<std::size_t>(s.k));
  for (int i = 0; i < s.k; ++i)
    s.even[static_cast<std::size_t>(i)] =
        static_cast<float>((up ? 2.0 : 1.0) * h[static_cast<std::size_t>(2 * i)]);
  s.hist.assign(static_cast<std::size_t>(up ? s.k - 1 : taps - 1), 0.0f);
}

void Oversampler4x::prepare(int maxBlock) {
  if (maxBlock < 1) throw std::invalid_argument("Oversampler4x::prepare: maxBlock must be >= 1");
  maxBlock_ = maxBlock;
  design(up1_, kStage1Taps, true);
  design(up2_, kStage2Taps, true);
  design(down2_, kStage2Taps, false);
  design(down1_, kStage1Taps, false);
  mid_.assign(static_cast<std::size_t>(2 * maxBlock), 0.0f);
  work_.assign(static_cast<std::size_t>(kStage1Taps + 4 * maxBlock + 8), 0.0f);
}

void Oversampler4x::reset() noexcept {
  for (Stage* s : {&up1_, &up2_, &down2_, &down1_}) std::fill(s->hist.begin(), s->hist.end(), 0.0f);
}

// out[2t] = sum_i e_i x[t-i] (symmetric, folded); out[2t+1] = x[t-m].
void Oversampler4x::upStage(Stage& s, const float* in, int n, float* out, float* work) noexcept {
  const int hl = s.k - 1;
  std::memcpy(work, s.hist.data(), static_cast<std::size_t>(hl) * sizeof(float));
  std::memcpy(work + hl, in, static_cast<std::size_t>(n) * sizeof(float));
  const float* e = s.even.data();
  const int half = s.k / 2;
  for (int t = 0; t < n; ++t) {
    const float* p = work + hl + t;  // p[-i] = x[t-i]
    float acc = 0.0f;
    for (int i = 0; i < half; ++i) acc += e[i] * (p[-i] + p[-(s.k - 1 - i)]);
    out[2 * t] = acc;
    out[2 * t + 1] = p[-s.m];
  }
  std::memcpy(s.hist.data(), work + n, static_cast<std::size_t>(hl) * sizeof(float));
}

// y[t] = 0.5 v[2t-c] + sum_i h[2i] v[2t-2i]  (v at the high rate, in has 2n samples).
void Oversampler4x::downStage(Stage& s, const float* in, int n, float* out, float* work) noexcept {
  const int hl = s.taps - 1;
  const int c = hl / 2;
  std::memcpy(work, s.hist.data(), static_cast<std::size_t>(hl) * sizeof(float));
  std::memcpy(work + hl, in, static_cast<std::size_t>(2 * n) * sizeof(float));
  const float* h = s.even.data();
  const int half = s.k / 2;
  for (int t = 0; t < n; ++t) {
    const float* p = work + hl + 2 * t;  // p[-j] = v[2t-j]
    float acc = 0.5f * p[-c];
    for (int i = 0; i < half; ++i) acc += h[i] * (p[-2 * i] + p[-2 * (s.k - 1 - i)]);
    out[t] = acc;
  }
  std::memcpy(s.hist.data(), work + 2 * n, static_cast<std::size_t>(hl) * sizeof(float));
}

void Oversampler4x::upsample(const float* in, int n, float* out4n) noexcept {
  upStage(up1_, in, n, mid_.data(), work_.data());
  upStage(up2_, mid_.data(), 2 * n, out4n, work_.data());
}

void Oversampler4x::downsample(const float* in4n, int n, float* out) noexcept {
  downStage(down2_, in4n, 2 * n, mid_.data(), work_.data());
  downStage(down1_, mid_.data(), n, out, work_.data());
}

void OversamplerNx::prepare(int factor, int maxIn) {
  if ((factor != 2 && factor != 4 && factor != 8) || maxIn < 1) throw std::invalid_argument("OversamplerNx::prepare: factor 2, 4 or 8");
  factor_ = factor;
  levels_ = factor == 2 ? 1 : factor == 4 ? 2 : 3;
  up_.assign(static_cast<std::size_t>(levels_), {});
  down_.assign(static_cast<std::size_t>(levels_), {});
  for (int i = 0; i < levels_; ++i) {
    Oversampler4x::design(up_[static_cast<std::size_t>(i)], kTaps, true);
    Oversampler4x::design(down_[static_cast<std::size_t>(i)], kTaps, false);
  }
  a_.assign(static_cast<std::size_t>(maxIn * factor), 0.0f);
  b_.assign(static_cast<std::size_t>(maxIn * factor), 0.0f);
  work_.assign(static_cast<std::size_t>(kTaps + maxIn * factor + 8), 0.0f);
}

void OversamplerNx::reset() noexcept {
  for (auto* v : {&up_, &down_})
    for (auto& s : *v) std::fill(s.hist.begin(), s.hist.end(), 0.0f);
}

void OversamplerNx::upsample(const float* in, int n, float* out) noexcept {
  if (n <= 0) return;
  const float* src = in;
  int cnt = n;
  for (int i = 0; i < levels_; ++i) {
    float* dst = i == levels_ - 1 ? out : (i % 2 == 0 ? a_.data() : b_.data());
    Oversampler4x::upStage(up_[static_cast<std::size_t>(i)], src, cnt, dst, work_.data());
    src = dst;
    cnt *= 2;
  }
}

void OversamplerNx::downsample(const float* in, int n, float* out) noexcept {
  if (n <= 0) return;
  const float* src = in;
  int cnt = n * factor_ / 2;  // output count of the first down stage
  for (int i = levels_ - 1; i >= 0; --i) {
    float* dst = i == 0 ? out : (i % 2 == 0 ? a_.data() : b_.data());
    Oversampler4x::downStage(down_[static_cast<std::size_t>(i)], src, cnt, dst, work_.data());
    src = dst;
    cnt /= 2;
  }
}

}  // namespace sawblade
