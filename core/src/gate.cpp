#include "sawblade/gate.h"

#include <algorithm>
#include <cmath>

namespace sawblade {
namespace {

constexpr double kEnvAttackMs = 0.1;
constexpr double kEnvReleaseMs = 10.0;
constexpr double kSettleDb = 1e-4;  // snap to target within this many dB

double onePoleCoeff(double ms, double fs) {
  const double samples = ms * 0.001 * fs;
  return samples <= 0.0 ? 0.0 : std::exp(-1.0 / samples);
}

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

void Gate::setParams(const GateParams& p) noexcept {
  params_ = p;
  updateCoefficients();
}

void Gate::updateCoefficients() noexcept {
  envAtk_ = onePoleCoeff(kEnvAttackMs, sampleRate_);
  envRel_ = onePoleCoeff(kEnvReleaseMs, sampleRate_);
  gainAtk_ = onePoleCoeff(params_.attackMs, sampleRate_);
  gainRel_ = onePoleCoeff(params_.releaseMs, sampleRate_);
  openLin_ = dbToLin(params_.thresholdDb);
  closeLin_ = dbToLin(params_.thresholdDb - std::max(0.0, params_.hysteresisDb));
  holdSamples_ = static_cast<int>(std::lround(std::max(0.0, params_.holdMs) * 0.001 * sampleRate_));
}

void Gate::prepare(const ProcessSpec& spec) {
  sampleRate_ = spec.sampleRate;
  updateCoefficients();
  reset();
}

void Gate::reset() noexcept {
  env_ = 0.0;
  open_ = false;
  holdCount_ = 0;
  gainDb_ = params_.rangeDb;
}

void Gate::processKeyed(const float* key, float* io, int numSamples) noexcept {
  if (!params_.enabled) return;
  const double rangeDb = params_.rangeDb;
  for (int i = 0; i < numSamples; ++i) {
    const double x = std::fabs(static_cast<double>(key[i]));
    env_ = x > env_ ? x + envAtk_ * (env_ - x) : x + envRel_ * (env_ - x);

    if (!open_) {
      if (env_ >= openLin_) {
        open_ = true;
        holdCount_ = holdSamples_;
      }
    } else if (env_ >= closeLin_) {
      holdCount_ = holdSamples_;
    } else if (holdCount_ > 0) {
      --holdCount_;
    } else {
      open_ = false;
    }

    const double target = open_ ? 0.0 : rangeDb;
    const double diff = target - gainDb_;
    if (std::fabs(diff) <= kSettleDb) {
      gainDb_ = target;
    } else {
      const double c = diff > 0.0 ? gainAtk_ : gainRel_;
      gainDb_ = target - c * diff;  // one-pole toward target
    }
    // At exactly 0 dB skip the pow() and keep the open gate bit-transparent.
    if (gainDb_ != 0.0) io[i] = static_cast<float>(io[i] * dbToLin(gainDb_));
  }
}

}  // namespace sawblade
