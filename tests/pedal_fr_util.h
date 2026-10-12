#pragma once

// Small-signal magnitude response of a Processor, shared by tests/tools/pedal_fr.cpp and the
// pedal tests. Method (docs/specs/phase7_modeled_pedals.md, section 6): impulse at -90 dBFS,
// 65536 samples, fs = 48 kHz, FFT magnitude normalised by the impulse amplitude.

#include <algorithm>
#include <cmath>
#include <vector>

#include "fft_util.h"
#include "sawblade/processor.h"

namespace sawblade::test {

constexpr std::size_t kFrN = 65536;
constexpr double kFrFs = 48000.0;
constexpr double kFrImpulseDb = -90.0;

// Returns |H| in dB for bins 0..N/2 (bin k = k * fs / N). Prepares `p` itself.
inline std::vector<double> smallSignalResponseDb(Processor& p, double fs = kFrFs, std::size_t n = kFrN) {
  const int block = 4096;
  p.prepare({fs, block});
  const float amp = static_cast<float>(std::pow(10.0, kFrImpulseDb / 20.0));
  std::vector<float> x(n, 0.0f);
  x[0] = amp;
  for (std::size_t pos = 0; pos < n; pos += block)
    p.process(x.data() + pos, static_cast<int>(std::min<std::size_t>(block, n - pos)));
  const auto spec = fftReal(x, n);
  std::vector<double> db(n / 2 + 1);
  for (std::size_t k = 0; k < db.size(); ++k) db[k] = 20.0 * std::log10(std::max(std::abs(spec[k]) / amp, 1e-30));
  return db;
}

// THD of a 500 Hz sine through `p` (already prepared at fs): 1 s of signal, the first 0.25 s discarded,
// the analysis window is the last 32768 samples with the tone snapped to a bin centre. THD =
// harmonics 2..20 over the fundamental (dB); h2 = H2 in dBc.
struct ThdPoint {
  double thdDb, h2Dbc;
};
inline ThdPoint thdPoint(Processor& p, double levelDbfs, double fs = kFrFs, int block = 512) {
  constexpr std::size_t N = 32768;
  const std::size_t total = static_cast<std::size_t>(fs);  // 1 s
  const double f0 = std::round(500.0 * N / fs) * fs / N;
  const double amp = std::pow(10.0, levelDbfs / 20.0);
  std::vector<float> x(total);
  for (std::size_t i = 0; i < total; ++i) x[i] = static_cast<float>(amp * std::sin(2.0 * 3.14159265358979323846 * f0 * static_cast<double>(i) / fs));
  p.reset();
  for (std::size_t pos = 0; pos < total; pos += static_cast<std::size_t>(block))
    p.process(x.data() + pos, static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), total - pos)));
  const std::vector<float> y(x.end() - static_cast<std::ptrdiff_t>(N), x.end());
  const auto spec = fftReal(y, N);
  const std::size_t k0 = static_cast<std::size_t>(std::llround(f0 * N / fs));
  double s = 0.0;
  for (std::size_t h = 2; h <= 20; ++h) s += std::norm(spec[h * k0]);
  const double fund = std::abs(spec[k0]);
  return {10.0 * std::log10(std::max(s, 1e-60) / (fund * fund)), 20.0 * std::log10(std::max(std::abs(spec[2 * k0]), 1e-30) / fund)};
}

inline std::size_t frBin(double hz, double fs = kFrFs, std::size_t n = kFrN) {
  return static_cast<std::size_t>(std::llround(hz * static_cast<double>(n) / fs));
}
inline double frAt(const std::vector<double>& db, double hz, double fs = kFrFs, std::size_t n = kFrN) {
  return db[frBin(hz, fs, n)];
}

}  // namespace sawblade::test
