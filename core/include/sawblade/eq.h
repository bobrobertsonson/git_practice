#pragma once

#include <array>
#include <span>

#include "sawblade/processor.h"

namespace sawblade {

enum class EqType { Peak, LowShelf, HighShelf, HighPass, LowPass };

struct EqBand {
  EqType type = EqType::Peak;
  double freq = 1000.0;   // Hz, must be in (0, 0.49 * fs)
  double gainDb = 0.0;    // ignored for HighPass / LowPass
  double q = 0.7071067811865476;  // RBJ Q, must be > 0 (shelves use it as RBJ Q)
  bool enabled = true;
};

// Normalized biquad coefficients (a0 == 1): y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2.
struct BiquadCoeffs {
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

// Throws std::invalid_argument if freq <= 0, freq >= 0.49 * sampleRate, q <= 0, or any value
// is not finite.
void validateEqBand(const EqBand& band, double sampleRate);

// RBJ Audio-EQ-Cookbook design in double. Validates like validateEqBand.
BiquadCoeffs designBiquad(const EqBand& band, double sampleRate);

// Analytic magnitude response in dB of a biquad at freqHz.
double biquadMagnitudeDb(const BiquadCoeffs& c, double freqHz, double sampleRate);

// Transposed direct form II, double state.
class Biquad {
 public:
  void setCoeffs(const BiquadCoeffs& c) noexcept { c_ = c; }
  void reset() noexcept { z1_ = z2_ = 0.0; }
  void process(float* io, int n) noexcept;
  const BiquadCoeffs& coeffs() const noexcept { return c_; }

 private:
  BiquadCoeffs c_{};
  double z1_ = 0.0, z2_ = 0.0;
};

// Cascade of up to 16 bands (array order); disabled bands are skipped.
class ParametricEq : public Processor {
 public:
  static constexpr int kMaxBands = 16;

  // Configure before prepare() (not real-time safe: validates and may throw
  // std::invalid_argument; nothing is changed if it throws). The sampleRate here is used for
  // validation and magnitudeDb(); prepare() redesigns for spec.sampleRate and throws if a
  // band is invalid at that rate.
  void configure(double sampleRate, std::span<const EqBand> bands);

  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;

  // Combined analytic response (sum of enabled bands' dB) at the configured sample rate.
  double magnitudeDb(double freqHz) const;

  int numActiveBands() const noexcept { return numActive_; }

 private:
  void design(double sampleRate);

  double sampleRate_ = 48000.0;
  std::array<EqBand, kMaxBands> bands_{};
  int numBands_ = 0;
  std::array<Biquad, kMaxBands> filters_{};
  int numActive_ = 0;
};

}  // namespace sawblade
