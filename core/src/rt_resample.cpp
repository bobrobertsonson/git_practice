#include "sawblade/rt_resample.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <numeric>
#include <stdexcept>

#include "sawblade/resample.h"

namespace sawblade {
namespace {

double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  const double q = x * x / 4.0;
  for (int k = 1; k < 500; ++k) {
    term *= q / (static_cast<double>(k) * k);
    sum += term;
    if (term < 1e-18 * sum) break;
  }
  return sum;
}

double sinc(double x) {
  if (std::fabs(x) < 1e-12) return 1.0;
  const double px = std::numbers::pi * x;
  return std::sin(px) / px;
}

bool nearInteger(double v) { return std::fabs(v - std::round(v)) < 1e-6 && v >= 1.0 && v < 9e15; }

std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept {  // b > 0
  std::int64_t q = a / b;
  if (a % b < 0) --q;
  return q;
}

void checkRate(double r) {
  if (!(r > 0.0) || !std::isfinite(r)) throw std::invalid_argument("RtResampler: invalid rate");
}

}  // namespace

RtResampler::Ratio RtResampler::ratioFor(double fromRate, double toRate) {
  checkRate(fromRate);
  checkRate(toRate);
  const double x = toRate / fromRate;
  if (x < 1.0 / 64.0 || x > 64.0) throw std::invalid_argument("RtResampler: ratio outside 1/64..64");
  if (nearInteger(fromRate) && nearInteger(toRate)) {
    const auto f = static_cast<std::int64_t>(std::llround(fromRate));
    const auto t = static_cast<std::int64_t>(std::llround(toRate));
    const std::int64_t g = std::gcd(f, t);
    if (t / g <= kMaxPhases) return {t / g, f / g};
  }
  // Continued-fraction convergents of x = L/M, bounded by L <= kMaxPhases.
  std::int64_t p0 = 0, q0 = 1, p1 = 1, q1 = 0;  // previous / current convergent (p = L, q = M)
  double r = x;
  for (int i = 0; i < 40; ++i) {
    const auto a = static_cast<std::int64_t>(std::floor(r));
    const std::int64_t p2 = a * p1 + p0, q2 = a * q1 + q0;
    if (p2 > kMaxPhases) break;
    p0 = p1;
    q0 = q1;
    p1 = p2;
    q1 = q2;
    const double frac = r - static_cast<double>(a);
    if (frac < 1e-12) break;
    r = 1.0 / frac;
  }
  if (p1 < 1 || q1 < 1) throw std::invalid_argument("RtResampler: cannot approximate the ratio");
  const std::int64_t g = std::gcd(p1, q1);
  return {p1 / g, q1 / g};
}

void RtResampler::prepare(double fromRate, double toRate, int maxBlock) {
  prepare(fromRate, toRate, ratioFor(fromRate, toRate), maxBlock);
}

void RtResampler::prepare(double fromRate, double toRate, Ratio ratio, int maxBlock) {
  checkRate(fromRate);
  checkRate(toRate);
  if (maxBlock < 1) throw std::invalid_argument("RtResampler: maxBlock must be >= 1");
  if (ratio.L < 1 || ratio.M < 1 || ratio.L > kMaxPhases) throw std::invalid_argument("RtResampler: bad ratio");

  // Kernel design: identical to the offline converter (resample.cpp), expressed in input samples.
  const double nyq = 0.5 * std::min(fromRate, toRate);
  const double fc = kResampleCutoffOfNyquist * nyq / (0.5 * fromRate);  // cutoff relative to the input Nyquist
  const double dfNorm = kResampleTransitionOfNyquist * nyq / fromRate;
  const double total = (kResampleStopbandDb - 7.95) / (2.285 * 2.0 * std::numbers::pi * dfNorm);
  const double half = 0.5 * total;
  const int reach = static_cast<int>(std::ceil(half));
  // The offline kernel spans 2*reach+2 taps but its first and last taps are identically zero;
  // the effective support is the 2*reach taps at offsets 1-reach .. reach from the base index.
  const int taps = 2 * reach;
  const double invI0 = 1.0 / besselI0(kResampleKaiserBeta);

  std::vector<float> table(static_cast<std::size_t>(ratio.L) * static_cast<std::size_t>(taps));
  std::vector<double> w(static_cast<std::size_t>(taps));
  for (std::int64_t p = 0; p < ratio.L; ++p) {
    const double frac = static_cast<double>(p) / static_cast<double>(ratio.L);
    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
      const double d = static_cast<double>(1 - reach + i) - frac;
      const double r = d / half;
      double v = 0.0;
      if (r > -1.0 && r < 1.0) v = fc * sinc(fc * d) * besselI0(kResampleKaiserBeta * std::sqrt(1.0 - r * r)) * invI0;
      w[static_cast<std::size_t>(i)] = v;
      sum += v;
    }
    const double g = 1.0 / sum;
    for (int i = 0; i < taps; ++i)
      table[static_cast<std::size_t>(p) * static_cast<std::size_t>(taps) + static_cast<std::size_t>(i)] =
          static_cast<float>(w[static_cast<std::size_t>(i)] * g);
  }

  ratio_ = ratio;
  reach_ = reach;
  taps_ = taps;
  maxBlock_ = maxBlock;
  table_ = std::move(table);
  buf_.assign(static_cast<std::size_t>(taps) + static_cast<std::size_t>(maxBlock), 0.0f);
  s_ = 0;
  reset();
}

void RtResampler::reset() noexcept {
  len_ = 0;
  inCount_ = 0;
  outIdx_ = 0;
}

void RtResampler::setPhaseOffset(std::int64_t s) noexcept {
  s_ = s < 0 ? 0 : s;
  reset();
}

std::int64_t RtResampler::phaseOffsetForIntegerDelay(std::int64_t extraInputDelay) const noexcept {
  // (extra + H + s/L) * L / M = ((extra + H) * L + s) / M must be an integer.
  const std::int64_t a = ((extraInputDelay + reach_) * ratio_.L) % ratio_.M;
  return (ratio_.M - a) % ratio_.M;
}

int RtResampler::process(const float* in, int nIn, float* out) noexcept {
  if (nIn <= 0 || table_.empty()) return 0;
  if (nIn > maxBlock_) nIn = maxBlock_;  // contract violation; never overrun the buffer
  std::memcpy(buf_.data() + len_, in, static_cast<std::size_t>(nIn) * sizeof(float));
  len_ += nIn;
  inCount_ += nIn;
  const std::int64_t bufBase = inCount_ - len_;  // global index of buf_[0]

  const std::int64_t L = ratio_.L, M = ratio_.M;
  const auto taps = static_cast<std::size_t>(taps_);
  int written = 0;
  const int cap = maxOutputFor(nIn);
  while (written < cap) {
    const std::int64_t n = outIdx_ * M - static_cast<std::int64_t>(reach_) * L - s_;
    const std::int64_t base = floorDiv(n, L);
    if (base + reach_ >= inCount_) break;  // needs input not yet received
    const auto phase = static_cast<std::size_t>(n - base * L);
    const std::int64_t start = base + 1 - reach_;  // global index of the first tap
    const float* w = table_.data() + phase * taps;
    std::size_t i0 = 0;
    if (start < bufBase) i0 = static_cast<std::size_t>(bufBase - start);  // before the stream start: zeros
    const float* x = buf_.data() + (start + static_cast<std::int64_t>(i0) - bufBase);
    w += i0;
    const std::size_t cnt = taps - i0;
    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    std::size_t i = 0;
    for (; i + 4 <= cnt; i += 4) {
      a0 += x[i] * w[i];
      a1 += x[i + 1] * w[i + 1];
      a2 += x[i + 2] * w[i + 2];
      a3 += x[i + 3] * w[i + 3];
    }
    for (; i < cnt; ++i) a0 += x[i] * w[i];
    out[written++] = (a0 + a1) + (a2 + a3);
    ++outIdx_;
  }

  // Drop history that no future output can reach.
  const std::int64_t n = outIdx_ * M - static_cast<std::int64_t>(reach_) * L - s_;
  const std::int64_t nextStart = floorDiv(n, L) + 1 - reach_;
  if (nextStart > bufBase) {
    const auto drop = static_cast<int>(std::min<std::int64_t>(nextStart - bufBase, len_));
    std::memmove(buf_.data(), buf_.data() + drop, static_cast<std::size_t>(len_ - drop) * sizeof(float));
    len_ -= drop;
  }
  return written;
}

}  // namespace sawblade
