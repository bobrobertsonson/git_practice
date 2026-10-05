#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <vector>

namespace sawblade::test {

inline std::vector<float> sine(double freq, double fs, std::size_t n, double amp = 0.5) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i)
    x[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(i) / fs));
  return x;
}

inline std::vector<float> noise(std::size_t n, unsigned seed = 1, float amp = 0.3f) {
  std::mt19937 g(seed);
  std::uniform_real_distribution<float> d(-amp, amp);
  std::vector<float> x(n);
  for (auto& v : x) v = d(g);
  return x;
}

inline double rms(const float* x, std::size_t n) {
  double s = 0.0;
  for (std::size_t i = 0; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(n));
}

inline double toDb(double lin) { return 20.0 * std::log10(lin); }

// Run `proc` over x in chunks of the given sizes (cycled), in place.
template <class Proc>
void processChunked(Proc& p, std::vector<float>& x, const std::vector<int>& sizes) {
  std::size_t pos = 0, k = 0;
  while (pos < x.size()) {
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(sizes[k++ % sizes.size()]), x.size() - pos);
    p.process(x.data() + pos, static_cast<int>(n));
    pos += n;
  }
}

// Exact comparison of two renders (tolerance 0) that, on a mismatch, reports the first differing index and the
// maximum |difference| instead of letting Catch print both arrays. Call inside a TEST_CASE (uses INFO / REQUIRE).
#define SAWBLADE_REQUIRE_SAME_SAMPLES(expected, actual)                                                           \
  do {                                                                                                            \
    const auto& sbE_ = (expected);                                                                                \
    const auto& sbA_ = (actual);                                                                                  \
    REQUIRE(sbE_.size() == sbA_.size());                                                                          \
    std::size_t sbFirst_ = sbE_.size(), sbCount_ = 0;                                                             \
    double sbMax_ = 0.0;                                                                                          \
    for (std::size_t sbI_ = 0; sbI_ < sbE_.size(); ++sbI_) {                                                      \
      if (sbE_[sbI_] == sbA_[sbI_]) continue;                                                                     \
      if (sbFirst_ == sbE_.size()) sbFirst_ = sbI_;                                                               \
      ++sbCount_;                                                                                                 \
      sbMax_ = std::max(sbMax_, std::fabs(static_cast<double>(sbE_[sbI_]) - static_cast<double>(sbA_[sbI_])));    \
    }                                                                                                             \
    INFO("samples differing: " << sbCount_ << " of " << sbE_.size() << ", first at index " << sbFirst_            \
                               << ", max |diff| = " << sbMax_);                                                   \
    REQUIRE(sbCount_ == 0);                                                                                       \
  } while (0)

}  // namespace sawblade::test
