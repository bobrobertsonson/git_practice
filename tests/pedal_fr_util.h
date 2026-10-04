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

inline std::size_t frBin(double hz, double fs = kFrFs, std::size_t n = kFrN) {
  return static_cast<std::size_t>(std::llround(hz * static_cast<double>(n) / fs));
}
inline double frAt(const std::vector<double>& db, double hz, double fs = kFrFs, std::size_t n = kFrN) {
  return db[frBin(hz, fs, n)];
}

}  // namespace sawblade::test
