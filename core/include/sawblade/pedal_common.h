#pragma once

#include <array>
#include <cmath>
#include <numbers>

namespace sawblade {

// Internal switches for the modeled pedals. Not part of any preset: the registry always builds
// pedals with the defaults. Tests use them to prove the aliasing test is sensitive (oversampling
// and ADAA off), and to measure latency by construction (flatFilters: every linear filter is
// bypassed, so the small-signal impulse response is symmetric about the reported latency).
struct PedalImplConfig {
  bool oversample = true;
  bool adaa = true;
  bool flatFilters = false;
};

// First-order filter by bilinear transform with prewarp; double state, float in/out.
class OnePole {
 public:
  void setLowPass(double fc, double fs) noexcept {
    const double k = std::tan(std::numbers::pi * fc / fs);
    b0_ = b1_ = k / (1.0 + k);
    a1_ = (k - 1.0) / (1.0 + k);
  }
  void setHighPass(double fc, double fs) noexcept {
    const double k = std::tan(std::numbers::pi * fc / fs);
    b0_ = 1.0 / (1.0 + k);
    b1_ = -b0_;
    a1_ = (k - 1.0) / (1.0 + k);
  }
  void reset() noexcept { z_ = 0.0; }
  void process(float* io, int n) noexcept {
    double z = z_;
    for (int i = 0; i < n; ++i) {
      const double x = io[i];
      const double y = b0_ * x + z;
      z = b1_ * x - a1_ * y;
      io[i] = static_cast<float>(y);
    }
    z_ = z;
  }

 private:
  double b0_ = 1.0, b1_ = 0.0, a1_ = 0.0, z_ = 0.0;
};

// Delay of 0..kMax samples, in place, allocation-free.
class ShortDelay {
 public:
  static constexpr int kMax = 7;
  void set(int d) noexcept {
    d_ = d < 0 ? 0 : (d > kMax ? kMax : d);
    reset();
  }
  void reset() noexcept {
    buf_.fill(0.0f);
    w_ = 0;
  }
  int delay() const noexcept { return d_; }
  void process(float* io, int n) noexcept {
    if (d_ == 0) return;
    for (int i = 0; i < n; ++i) {
      const float x = io[i];
      io[i] = buf_[static_cast<std::size_t>((w_ + kSize - d_) % kSize)];
      buf_[static_cast<std::size_t>(w_)] = x;
      w_ = (w_ + 1) % kSize;
    }
  }

 private:
  static constexpr int kSize = kMax + 1;
  std::array<float, kSize> buf_{};
  int w_ = 0, d_ = 0;
};

inline float dbToGain(double db) noexcept { return static_cast<float>(std::pow(10.0, db / 20.0)); }

// Samples of padding (at 4 fs) so that the oversampled-domain total is a multiple of 4.
constexpr int osPadding(int osTotal) { return (4 - osTotal % 4) % 4; }

}  // namespace sawblade
