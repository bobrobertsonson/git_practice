#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string_view>
#include <vector>

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

// ---- clipper types shared by the pedal circuits (docs/specs/phase7b_chainsaw_pedal.md, 1.3) ------
enum class ClipType : int { Silicon = 0, Led = 1, Asymmetric = 2, Soft = 3 };
constexpr int kNumClipTypes = 4;
// `follow` = the second stage uses the first stage's clipper.
enum class Clip2Type : int { Follow = 0, Silicon = 1, Led = 2, Asymmetric = 3, Soft = 4 };

// Knees (before bias / crunch) and the polynomial order m of the clip shape (m = 1 cubic, m = 2 quintic).
struct ClipShapeSpec {
  double kPos, kNeg;
  int order;
};

constexpr ClipShapeSpec clipShapeSpec(ClipType t) {
  switch (t) {
    case ClipType::Silicon: return {0.5, 0.5, 1};
    case ClipType::Led: return {1.4, 1.4, 2};
    case ClipType::Asymmetric: return {0.5, 0.3, 1};
    case ClipType::Soft: return {0.3, 0.3, 1};
  }
  return {0.5, 0.5, 1};
}

inline constexpr const char* kClipNames[kNumClipTypes] = {"silicon", "led", "asymmetric", "soft"};
inline constexpr const char* kClip2Names[kNumClipTypes + 1] = {"follow", "silicon", "led", "asymmetric", "soft"};

constexpr const char* clipTypeName(ClipType t) { return kClipNames[static_cast<int>(t)]; }
constexpr const char* clip2TypeName(Clip2Type t) { return kClip2Names[static_cast<int>(t)]; }
constexpr ClipType resolveClip2(ClipType clip, Clip2Type c2) {
  return c2 == Clip2Type::Follow ? clip : static_cast<ClipType>(static_cast<int>(c2) - 1);
}

// Bias multiplies the negative knee by 1 - 0.5 * bias / 10.
inline double biasKneeFactor(double bias) noexcept { return 1.0 - 0.5 * bias / 10.0; }

// ---- live-parameter helpers ------------------------------------------------------------------------
inline constexpr double kLiveRampMs = 20.0;

// Linear gain ramp. With no ramp pending, apply() multiplies by the one constant (float(target)),
// exactly like a static build.
struct GainRamp {
  double cur = 1.0, target = 1.0, step = 0.0;
  int left = 0;
  void setImmediate(double g) noexcept {
    cur = target = g;
    step = 0.0;
    left = 0;
  }
  void setTarget(double g, int rampSamples) noexcept {
    if (g == target && left == 0) return;
    if (rampSamples <= 0) {
      setImmediate(g);
      return;
    }
    target = g;
    step = (g - cur) / rampSamples;
    left = rampSamples;
  }
  bool active() const noexcept { return left > 0; }
  float next() noexcept {  // one sample
    if (left > 0) {
      cur += step;
      if (--left == 0) cur = target;
    }
    return static_cast<float>(cur);
  }
  void apply(float* x, int n) noexcept {
    int i = 0;
    while (left > 0 && i < n) x[i++] *= next();
    const float g = static_cast<float>(cur);
    for (; i < n; ++i) x[i] *= g;
  }
};

inline int rampSamplesFor(double rate) noexcept { return static_cast<int>(std::lround(kLiveRampMs * 1e-3 * rate)); }

// Base-rate delay of exactly `d` samples carrying the dry input of the clean mix. Sized in prepare().
// Usage per block: push(in, n); then tap(i) for i in [0, n) gives in[i - d]; then commit(n).
class DryDelay {
 public:
  void prepare(int delay, int maxBlock) {
    d_ = delay;
    std::size_t sz = 1;
    while (sz < static_cast<std::size_t>(delay + maxBlock + 1)) sz <<= 1;
    buf_.assign(sz, 0.0f);
    mask_ = sz - 1;
    w_ = 0;
  }
  void reset() noexcept {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    w_ = 0;
  }
  void push(const float* in, int n) noexcept {
    for (int i = 0; i < n; ++i) buf_[(w_ + static_cast<std::size_t>(i)) & mask_] = in[i];
  }
  float tap(int i) const noexcept { return buf_[(w_ + static_cast<std::size_t>(i) - static_cast<std::size_t>(d_)) & mask_]; }
  void commit(int n) noexcept { w_ = (w_ + static_cast<std::size_t>(n)) & mask_; }

 private:
  std::vector<float> buf_;
  std::size_t mask_ = 0, w_ = 0;
  int d_ = 0;
};

}  // namespace sawblade
