#include "sawblade/gate.h"

#include <algorithm>
#include <cmath>

namespace sawblade {
namespace {

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
  envAtk_ = onePoleCoeff(Gate::kEnvAttackMs, sampleRate_);
  envRel_ = onePoleCoeff(Gate::kEnvReleaseMs, sampleRate_);
  gainAtk_ = onePoleCoeff(params_.attackMs, sampleRate_);
  gainRel_ = onePoleCoeff(params_.releaseMs, sampleRate_);
  openLin_ = dbToLin(params_.thresholdDb);
  closeLin_ = dbToLin(params_.thresholdDb - std::max(0.0, params_.hysteresisDb));
  closeDb_ = params_.thresholdDb - std::max(0.0, params_.hysteresisDb);
  rampStepDb_ = params_.releaseMs > 0.0 ? -params_.rangeDb / (params_.releaseMs * 0.001 * sampleRate_) : 1e9;
  keyHp_ = params_.keyHighPassHz > 0.0;
  if (keyHp_) {
    // RBJ 2nd-order Butterworth high-pass (Q = 1/sqrt(2)).
    const double w0 = 2.0 * 3.14159265358979323846 * std::min(params_.keyHighPassHz, 0.45 * sampleRate_) / sampleRate_;
    const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * 0.7071067811865476);
    const double a0 = 1.0 + alpha;
    kb0_ = (1.0 + cw) * 0.5 / a0;
    kb1_ = -(1.0 + cw) / a0;
    kb2_ = kb0_;
    ka1_ = -2.0 * cw / a0;
    ka2_ = (1.0 - alpha) / a0;
  }
  holdSamples_ = static_cast<int>(std::lround(std::max(0.0, params_.holdMs) * 0.001 * sampleRate_));
}

void Gate::prepare(const ProcessSpec& spec) {
  sampleRate_ = spec.sampleRate;
  updateCoefficients();
  reset();
}

void Gate::reset() {
  env_ = 0.0;
  kz1_ = kz2_ = 0.0;
  open_ = false;
  holdCount_ = 0;
  gainDb_ = params_.rangeDb;
}

void Gate::processKeyed(const float* key, float* io, int numSamples) noexcept {
  if (!params_.enabled) return;
  const double rangeDb = params_.rangeDb;
  const bool expander = params_.mode == GateMode::Expander;
  const bool linearRelease = params_.releaseCurve == GateReleaseCurve::LinearDb;
  for (int i = 0; i < numSamples; ++i) {
    double k = static_cast<double>(key[i]);
    if (keyHp_) {
      const double y = kb0_ * k + kz1_;
      kz1_ = kb1_ * k - ka1_ * y + kz2_;
      kz2_ = kb2_ * k - ka2_ * y;
      k = y;
    }
    const double x = std::fabs(k);
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

    double target = open_ ? 0.0 : rangeDb;
    if (expander && !open_ && env_ < closeLin_) {
      const double envDb = 20.0 * std::log10(std::max(env_, 1e-12));
      target = std::max(rangeDb, -(params_.ratio - 1.0) * (closeDb_ - envDb));
    } else if (expander && !open_) {
      target = 0.0;
    }
    const double diff = target - gainDb_;
    if (std::fabs(diff) <= kSettleDb) {
      gainDb_ = target;
    } else if (diff < 0.0 && linearRelease) {
      gainDb_ = std::max(target, gainDb_ - rampStepDb_);
    } else {
      const double c = diff > 0.0 ? gainAtk_ : gainRel_;
      gainDb_ = target - c * diff;  // one-pole toward target
    }
    // At exactly 0 dB skip the pow() and keep the open gate bit-transparent.
    if (gainDb_ != 0.0) io[i] = static_cast<float>(io[i] * dbToLin(gainDb_));
  }
}

}  // namespace sawblade
