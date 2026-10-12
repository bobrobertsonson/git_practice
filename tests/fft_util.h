#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

namespace sawblade::test {

// In-place radix-2 FFT (size must be a power of two). Test/tool use only.
inline void fft(std::vector<std::complex<double>>& a) {
  const std::size_t n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * std::numbers::pi / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < len / 2; ++k) {
        const auto u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

inline std::vector<std::complex<double>> fftReal(const std::vector<float>& x, std::size_t n) {
  std::vector<std::complex<double>> a(n);
  for (std::size_t i = 0; i < n && i < x.size(); ++i) a[i] = x[i];
  fft(a);
  return a;
}

}  // namespace sawblade::test
