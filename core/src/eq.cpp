#include "sawblade/eq.h"

#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>

namespace sawblade {

void validateEqBand(const EqBand& b, double fs) {
  if (!std::isfinite(fs) || fs <= 0.0) throw std::invalid_argument("EQ: invalid sample rate");
  if (!std::isfinite(b.freq) || b.freq <= 0.0)
    throw std::invalid_argument("EQ: freq must be > 0");
  if (b.freq >= 0.49 * fs)
    throw std::invalid_argument("EQ: freq must be < 0.49 * sampleRate");
  if (!std::isfinite(b.q) || b.q <= 0.0) throw std::invalid_argument("EQ: q must be > 0");
  if (!std::isfinite(b.gainDb)) throw std::invalid_argument("EQ: gainDb must be finite");
}

BiquadCoeffs designBiquad(const EqBand& band, double fs) {
  validateEqBand(band, fs);
  const double w0 = 2.0 * std::numbers::pi * band.freq / fs;
  const double cw = std::cos(w0);
  const double sw = std::sin(w0);
  const double alpha = sw / (2.0 * band.q);
  const double A = std::pow(10.0, band.gainDb / 40.0);

  double b0, b1, b2, a0, a1, a2;
  switch (band.type) {
    case EqType::Peak:
      b0 = 1.0 + alpha * A;
      b1 = -2.0 * cw;
      b2 = 1.0 - alpha * A;
      a0 = 1.0 + alpha / A;
      a1 = -2.0 * cw;
      a2 = 1.0 - alpha / A;
      break;
    case EqType::LowShelf: {
      const double t = 2.0 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1.0) - (A - 1.0) * cw + t);
      b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
      b2 = A * ((A + 1.0) - (A - 1.0) * cw - t);
      a0 = (A + 1.0) + (A - 1.0) * cw + t;
      a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
      a2 = (A + 1.0) + (A - 1.0) * cw - t;
      break;
    }
    case EqType::HighShelf: {
      const double t = 2.0 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1.0) + (A - 1.0) * cw + t);
      b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
      b2 = A * ((A + 1.0) + (A - 1.0) * cw - t);
      a0 = (A + 1.0) - (A - 1.0) * cw + t;
      a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
      a2 = (A + 1.0) - (A - 1.0) * cw - t;
      break;
    }
    case EqType::HighPass:
      b0 = (1.0 + cw) / 2.0;
      b1 = -(1.0 + cw);
      b2 = (1.0 + cw) / 2.0;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cw;
      a2 = 1.0 - alpha;
      break;
    case EqType::LowPass:
    default:
      b0 = (1.0 - cw) / 2.0;
      b1 = 1.0 - cw;
      b2 = (1.0 - cw) / 2.0;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cw;
      a2 = 1.0 - alpha;
      break;
  }
  return BiquadCoeffs{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

double biquadMagnitudeDb(const BiquadCoeffs& c, double freqHz, double fs) {
  const double w = 2.0 * std::numbers::pi * freqHz / fs;
  const std::complex<double> z1 = std::polar(1.0, -w);
  const std::complex<double> z2 = z1 * z1;
  const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
  const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
  return 20.0 * std::log10(std::abs(num / den));
}

void Biquad::process(float* io, int n) noexcept {
  const double b0 = c_.b0, b1 = c_.b1, b2 = c_.b2, a1 = c_.a1, a2 = c_.a2;
  double z1 = z1_, z2 = z2_;
  for (int i = 0; i < n; ++i) {
    const double x = io[i];
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    io[i] = static_cast<float>(y);
  }
  z1_ = z1;
  z2_ = z2;
}

void ParametricEq::configure(double sampleRate, std::span<const EqBand> bands) {
  if (bands.size() > static_cast<std::size_t>(kMaxBands))
    throw std::invalid_argument("EQ: at most 16 bands");
  for (const EqBand& b : bands) validateEqBand(b, sampleRate);  // validate all before mutating
  numBands_ = static_cast<int>(bands.size());
  for (int i = 0; i < numBands_; ++i) bands_[static_cast<std::size_t>(i)] = bands[static_cast<std::size_t>(i)];
  design(sampleRate);
  reset();
}

void ParametricEq::design(double sampleRate) {
  sampleRate_ = sampleRate;
  numActive_ = 0;
  for (int i = 0; i < numBands_; ++i) {
    const EqBand& b = bands_[static_cast<std::size_t>(i)];
    if (!b.enabled) continue;
    const auto k = static_cast<std::size_t>(numActive_);
    filters_[k].setCoeffs(designBiquad(b, sampleRate));
    ++numActive_;
  }
}

void ParametricEq::prepare(const ProcessSpec& spec) {
  for (int i = 0; i < numBands_; ++i)
    if (bands_[static_cast<std::size_t>(i)].enabled)
      validateEqBand(bands_[static_cast<std::size_t>(i)], spec.sampleRate);
  design(spec.sampleRate);
  reset();
}

void ParametricEq::reset() noexcept {
  for (auto& f : filters_) f.reset();
}

void ParametricEq::process(float* io, int numSamples) noexcept {
  for (int k = 0; k < numActive_; ++k) filters_[static_cast<std::size_t>(k)].process(io, numSamples);
}

double ParametricEq::magnitudeDb(double freqHz) const {
  double db = 0.0;
  for (int k = 0; k < numActive_; ++k)
    db += biquadMagnitudeDb(filters_[static_cast<std::size_t>(k)].coeffs(), freqHz, sampleRate_);
  return db;
}

}  // namespace sawblade
