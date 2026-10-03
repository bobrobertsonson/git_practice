#include "sawblade/resample.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <stdexcept>

namespace sawblade {
namespace {

constexpr double kCutoffOfNyquist = 0.97;      // -6 dB point, relative to min(from,to)/2
constexpr double kTransitionOfNyquist = 0.14;  // pass edge .. stop edge, relative to min(from,to)/2
constexpr std::uint64_t kMaxPhases = 8192;      // beyond this the kernel is evaluated per output sample

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

bool isInteger(double v) { return std::fabs(v - std::round(v)) < 1e-9 && v >= 1.0 && v < 9e15; }

class Kernel {
 public:
  Kernel(double fromRate, double toRate) {
    init(fromRate, toRate);
    reach_ = static_cast<long long>(std::ceil(half_));
    taps_ = static_cast<std::size_t>(2 * reach_ + 2);
    invI0_ = 1.0 / besselI0(beta_);
  }

  void init(double fromRate, double toRate) {
    const double nyq = 0.5 * std::min(fromRate, toRate);
    fc_ = kCutoffOfNyquist * nyq / (0.5 * fromRate);  // cutoff relative to the input Nyquist
    const double dfNorm = kTransitionOfNyquist * nyq / fromRate;  // transition width, cycles/input sample
    const double total = (kResampleStopbandDb - 7.95) / (2.285 * 2.0 * std::numbers::pi * dfNorm);
    half_ = 0.5 * total;
  }

  long long firstOffset() const { return -reach_; }  // tap i sits at input index base + firstOffset() + i
  std::size_t taps() const { return taps_; }

  // Weights for an output sample located `frac` (0 <= frac < 1) input samples after input index
  // `base`. Normalised to unity DC gain.
  void weights(double frac, double* w) const {
    double sum = 0.0;
    for (std::size_t i = 0; i < taps_; ++i) {
      const double d = static_cast<double>(firstOffset() + static_cast<long long>(i)) - frac;
      const double r = d / half_;
      double v = 0.0;
      if (r > -1.0 && r < 1.0)
        v = fc_ * sinc(fc_ * d) * besselI0(beta_ * std::sqrt(1.0 - r * r)) * invI0_;
      w[i] = v;
      sum += v;
    }
    const double g = 1.0 / sum;
    for (std::size_t i = 0; i < taps_; ++i) w[i] *= g;
  }

 private:
  const double beta_ = kResampleKaiserBeta;
  double fc_ = 1.0, half_ = 1.0, invI0_ = 1.0;
  long long reach_ = 0;
  std::size_t taps_ = 0;
};

}  // namespace

std::size_t resampledLength(std::size_t n, double fromRate, double toRate) {
  if (!(fromRate > 0.0) || !(toRate > 0.0) || !std::isfinite(fromRate) || !std::isfinite(toRate))
    throw std::invalid_argument("resample: invalid rate");
  return static_cast<std::size_t>(std::llround(static_cast<double>(n) * toRate / fromRate));
}

std::vector<float> resample(const std::vector<float>& in, double fromRate, double toRate) {
  return resample(in, fromRate, toRate, resampledLength(in.size(), fromRate, toRate));
}

std::vector<float> resample(const std::vector<float>& in, double fromRate, double toRate, std::size_t outLen) {
  if (!(fromRate > 0.0) || !(toRate > 0.0) || !std::isfinite(fromRate) || !std::isfinite(toRate))
    throw std::invalid_argument("resample: invalid rate");
  if (fromRate == toRate || in.empty()) return in;

  const Kernel kernel(fromRate, toRate);
  const std::size_t taps = kernel.taps();
  const long long off = kernel.firstOffset();
  const auto inLen = static_cast<long long>(in.size());

  // Rational ratio (both rates integral): output j is at input time j*M/L; phase table of L kernels.
  std::uint64_t L = 0, M = 0;
  std::vector<double> table;
  if (isInteger(fromRate) && isInteger(toRate)) {
    const auto f = static_cast<std::uint64_t>(std::llround(fromRate)), t = static_cast<std::uint64_t>(std::llround(toRate));
    const std::uint64_t g = std::gcd(f, t);
    if (t / g <= kMaxPhases) {
      L = t / g;
      M = f / g;
      table.resize(static_cast<std::size_t>(L) * taps);
      for (std::uint64_t p = 0; p < L; ++p)
        kernel.weights(static_cast<double>(p) / static_cast<double>(L), &table[static_cast<std::size_t>(p) * taps]);
    }
  }

  std::vector<double> scratch(table.empty() ? taps : 0);
  std::vector<float> out(outLen);
  const double step = fromRate / toRate;
  for (std::size_t j = 0; j < outLen; ++j) {
    long long base;
    const double* w;
    if (!table.empty()) {
      const std::uint64_t pos = static_cast<std::uint64_t>(j) * M;
      base = static_cast<long long>(pos / L);
      w = &table[static_cast<std::size_t>(pos % L) * taps];
    } else {
      const double t = static_cast<double>(j) * step;
      const double fl = std::floor(t);
      base = static_cast<long long>(fl);
      kernel.weights(t - fl, scratch.data());
      w = scratch.data();
    }
    const long long start = base + off;
    double acc = 0.0;
    if (start >= 0 && start + static_cast<long long>(taps) <= inLen) {
      const float* x = in.data() + start;
      for (std::size_t i = 0; i < taps; ++i) acc += static_cast<double>(x[i]) * w[i];
    } else {
      for (std::size_t i = 0; i < taps; ++i) {
        const long long k = start + static_cast<long long>(i);
        if (k >= 0 && k < inLen) acc += static_cast<double>(in[static_cast<std::size_t>(k)]) * w[i];
      }
    }
    out[j] = static_cast<float>(acc);
  }
  return out;
}

}  // namespace sawblade
