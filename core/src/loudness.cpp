#include "sawblade/loudness.h"

#include <cmath>
#include <numbers>
#include <vector>

namespace sawblade {

KWeighting designKWeighting(double fs) {
  KWeighting k;
  {
    const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    const double K = std::tan(std::numbers::pi * f0 / fs);
    const double Vh = std::pow(10.0, G / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    k.shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
    k.shelf.b1 = 2.0 * (K * K - Vh) / a0;
    k.shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
    k.shelf.a1 = 2.0 * (K * K - 1.0) / a0;
    k.shelf.a2 = (1.0 - K / Q + K * K) / a0;
  }
  {
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;
    const double K = std::tan(std::numbers::pi * f0 / fs);
    const double a0 = 1.0 + K / Q + K * K;
    k.highpass.b0 = 1.0;
    k.highpass.b1 = -2.0;
    k.highpass.b2 = 1.0;
    k.highpass.a1 = 2.0 * (K * K - 1.0) / a0;
    k.highpass.a2 = (1.0 - K / Q + K * K) / a0;
  }
  return k;
}

namespace {

struct Biq {
  LoudnessBiquad c;
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  double run(double x) {
    const double y = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
  }
};

}  // namespace

std::optional<double> integratedLoudnessLufs(const float* left, const float* right, std::int64_t frames,
                                             double sampleRate) {
  if (frames <= 0 || !(sampleRate > 0.0)) return std::nullopt;
  const auto block = static_cast<std::int64_t>(std::llround(0.4 * sampleRate));
  const auto step = static_cast<std::int64_t>(std::llround(0.1 * sampleRate));
  if (block < 1 || step < 1 || frames < block) return std::nullopt;

  const KWeighting kw = designKWeighting(sampleRate);
  Biq sl{kw.shelf}, hl{kw.highpass}, sr{kw.shelf}, hr{kw.highpass};
  // Prefix sums of the per-sample K-weighted energy of both channels.
  std::vector<double> cum(static_cast<std::size_t>(frames) + 1, 0.0);
  for (std::int64_t i = 0; i < frames; ++i) {
    const double l = hl.run(sl.run(static_cast<double>(left[i])));
    const double r = hr.run(sr.run(static_cast<double>(right[i])));
    cum[static_cast<std::size_t>(i) + 1] = cum[static_cast<std::size_t>(i)] + l * l + r * r;
  }

  std::vector<double> z;  // per-block sum over channels of the mean square
  for (std::int64_t s = 0; s + block <= frames; s += step)
    z.push_back((cum[static_cast<std::size_t>(s + block)] - cum[static_cast<std::size_t>(s)]) / static_cast<double>(block));

  auto lufs = [](double e) { return -0.691 + 10.0 * std::log10(e); };
  double sum = 0.0;
  std::size_t n = 0;
  for (double e : z)
    if (e > 0.0 && lufs(e) > -70.0) {
      sum += e;
      ++n;
    }
  if (n == 0) return std::nullopt;
  const double relThreshold = lufs(sum / static_cast<double>(n)) - 10.0;
  double sum2 = 0.0;
  std::size_t n2 = 0;
  for (double e : z)
    if (e > 0.0 && lufs(e) > -70.0 && lufs(e) > relThreshold) {
      sum2 += e;
      ++n2;
    }
  if (n2 == 0) return std::nullopt;
  return lufs(sum2 / static_cast<double>(n2));
}

}  // namespace sawblade
