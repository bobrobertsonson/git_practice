#include "sawblade/ir.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include "sawblade/wav_io.h"

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

double sinc(double x) {
  if (std::fabs(x) < 1e-12) return 1.0;
  const double px = std::numbers::pi * x;
  return std::sin(px) / px;
}

constexpr int kHalfTaps = 64;    // input-sample taps per side at unity cutoff
constexpr double kKaiserBeta = 9.0;

}  // namespace

std::vector<float> resampleSinc(const std::vector<float>& in, double fromRate, double toRate) {
  if (!(fromRate > 0.0) || !(toRate > 0.0)) throw std::invalid_argument("resampleSinc: invalid rate");
  if (fromRate == toRate || in.empty()) return in;

  const double step = fromRate / toRate;                 // input samples per output sample
  const double fc = std::min(1.0, toRate / fromRate);    // cutoff relative to input Nyquist
  const double halfWidth = kHalfTaps / fc;               // kernel half width in input samples
  const double invI0 = 1.0 / besselI0(kKaiserBeta);
  const auto outLen = static_cast<std::size_t>(std::ceil(static_cast<double>(in.size()) / step));
  const long long inLen = static_cast<long long>(in.size());

  std::vector<float> out(outLen);
  for (std::size_t j = 0; j < outLen; ++j) {
    const double t = static_cast<double>(j) * step;
    const long long lo = std::max<long long>(0, static_cast<long long>(std::ceil(t - halfWidth)));
    const long long hi = std::min<long long>(inLen - 1, static_cast<long long>(std::floor(t + halfWidth)));
    double acc = 0.0;
    for (long long k = lo; k <= hi; ++k) {
      const double d = static_cast<double>(k) - t;
      const double r = d / halfWidth;
      if (r <= -1.0 || r >= 1.0) continue;
      const double w = besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) * invI0;
      acc += static_cast<double>(in[static_cast<std::size_t>(k)]) * fc * sinc(fc * d) * w;
    }
    out[j] = static_cast<float>(acc);
  }
  return out;
}

void normalizeL2(std::vector<float>& ir) {
  double e = 0.0;
  for (const float v : ir) e += static_cast<double>(v) * v;
  if (e <= 0.0) return;
  const double g = 1.0 / std::sqrt(e);
  for (float& v : ir) v = static_cast<float>(v * g);
}

IrData loadIr(const std::filesystem::path& path, double targetSampleRate, bool normalize) {
  if (!(targetSampleRate > 0.0))
    throw std::runtime_error("IR error (" + path.string() + "): invalid target sample rate");
  const AudioFile f = readWav(path);
  IrData out;
  out.sourceSampleRate = f.sampleRate;

  const auto ch = static_cast<std::size_t>(f.channels);
  const std::size_t frames = f.interleaved.size() / ch;
  std::vector<float> mono(frames);
  for (std::size_t i = 0; i < frames; ++i) mono[i] = f.interleaved[i * ch];
  if (f.channels > 1)
    out.warnings.push_back(path.string() + ": " + std::to_string(f.channels) +
                           "-channel IR; using the left channel only");
  if (mono.empty()) throw std::runtime_error("IR error (" + path.string() + "): file contains no samples");

  out.samples = resampleSinc(mono, f.sampleRate, targetSampleRate);

  const auto maxLen = static_cast<std::size_t>(std::llround(kMaxIrSeconds * targetSampleRate));
  if (out.samples.size() > maxLen) {
    out.samples.resize(maxLen);
    out.warnings.push_back(path.string() + ": IR truncated to 2.0 s");
  }
  if (normalize) normalizeL2(out.samples);
  return out;
}

}  // namespace sawblade
