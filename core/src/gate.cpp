#include "sawblade/gate.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sawblade {
namespace {

constexpr double kSettleDb = 1e-4;  // snap to target within this many dB

double onePoleCoeff(double ms, double fs) {
  const double samples = ms * 0.001 * fs;
  return samples <= 0.0 ? 0.0 : std::exp(-1.0 / samples);
}

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

double peakFloorDb(const std::vector<float>& x, double fs, double keyHpfHz, const std::vector<std::uint8_t>* mask) {
  const double atk = onePoleCoeff(Gate::kEnvAttackMs, fs), rel = onePoleCoeff(Gate::kEnvReleaseMs, fs);
  const bool hp = keyHpfHz > 0.0;
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
  if (hp) {  // the same RBJ Butterworth high-pass as the gate's key filter
    const double w0 = 2.0 * 3.14159265358979323846 * std::min(keyHpfHz, 0.45 * fs) / fs;
    const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * 0.7071067811865476);
    const double a0 = 1.0 + alpha;
    b0 = (1.0 + cw) * 0.5 / a0;
    b1 = -(1.0 + cw) / a0;
    b2 = b0;
    a1 = -2.0 * cw / a0;
    a2 = (1.0 - alpha) / a0;
  }
  std::vector<double> picked;
  picked.reserve(x.size());
  double env = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    double k = static_cast<double>(x[i]);
    if (hp) {
      const double y = b0 * k + z1;
      z1 = b1 * k - a1 * y + z2;
      z2 = b2 * k - a2 * y;
      k = y;
    }
    const double a = std::fabs(k);
    env = a > env ? a + atk * (env - a) : a + rel * (env - a);
    if (mask == nullptr || (i < mask->size() && (*mask)[i] != 0)) picked.push_back(env);
  }
  if (picked.empty()) return std::numeric_limits<double>::quiet_NaN();
  const auto idx = static_cast<std::size_t>(std::floor(0.925 * static_cast<double>(picked.size() - 1) + 0.5));
  std::nth_element(picked.begin(), picked.begin() + static_cast<std::ptrdiff_t>(idx), picked.end());
  return 20.0 * std::log10(std::max(picked[idx], 1e-12));
}

void Gate::setThresholdDb(double openDb) noexcept {
  openDb_ = openDb;
  openLin_ = dbToLin(openDb);
  closeLin_ = dbToLin(openDb - std::max(0.0, params_.hysteresisDb));
  closeDb_ = openDb - std::max(0.0, params_.hysteresisDb);
}

void Gate::resetFloor() noexcept {
  ring_.fill(kInf);
  ringHead_ = subsDone_ = frameInSub_ = frameCount_ = sinceQualFrames_ = 0;
  subMin_ = kInf;
  frameMax_ = 0.0;
  floorDb_ = kFloorSeedDb;
}

void Gate::setParams(const GateParams& p) noexcept {
  // Entering floorRelative starts the follower from its seed.
  if (p.thresholdMode == GateThresholdMode::FloorRelative && params_.thresholdMode != GateThresholdMode::FloorRelative)
    resetFloor();
  params_ = p;
  updateCoefficients();
}

// One finished 50 ms frame: its statistic is the maximum of the gate's peak envelope over the frame (linear).
void Gate::floorFrame(double frameMaxEnv) noexcept {
  const double framePeakDb = 20.0 * std::log10(std::max(frameMaxEnv, 1e-9));
  if (framePeakDb < floorDb_ + kQualifyDb) {
    subMin_ = std::min(subMin_, framePeakDb);
    sinceQualFrames_ = 0;
  } else if (sinceQualFrames_ < 1000000) {
    ++sinceQualFrames_;
  }
  if (++frameInSub_ >= kFramesPerSub) {
    ring_[static_cast<std::size_t>(ringHead_)] = subMin_;
    ringHead_ = (ringHead_ + 1) % kSubWindows;
    if (subsDone_ < kSubWindows) ++subsDone_;
    subMin_ = kInf;
    frameInSub_ = 0;
  }
  double m = subMin_;
  for (double v : ring_) m = std::min(m, v);
  if (m != kInf) {
    floorDb_ = subsDone_ < kSubWindows ? std::min(kFloorSeedDb, m) : m;
  } else if (sinceQualFrames_ * (kFrameMs * 0.001) > kLeakAfterS) {
    floorDb_ += kLeakDbPerS * kFrameMs * 0.001;
  }
  floorDb_ = std::min(kFloorMaxDb, std::max(kFloorMinDb, floorDb_));
  setThresholdDb(floorDb_ + params_.floorOffsetDb);
}

void Gate::updateCoefficients() noexcept {
  envAtk_ = onePoleCoeff(Gate::kEnvAttackMs, sampleRate_);
  envRel_ = onePoleCoeff(Gate::kEnvReleaseMs, sampleRate_);
  gainAtk_ = onePoleCoeff(params_.attackMs, sampleRate_);
  gainRel_ = onePoleCoeff(params_.releaseMs, sampleRate_);
  frameLen_ = std::max(1, static_cast<int>(std::lround(kFrameMs * 0.001 * sampleRate_)));
  setThresholdDb(params_.thresholdMode == GateThresholdMode::FloorRelative ? floorDb_ + params_.floorOffsetDb
                                                                           : params_.thresholdDb);
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
  resetFloor();
  updateCoefficients();
}

void Gate::processKeyed(const float* key, float* io, int numSamples) noexcept {
  if (!params_.enabled) return;
  const double rangeDb = params_.rangeDb;
  const bool expander = params_.mode == GateMode::Expander;
  const bool floorRel = params_.thresholdMode == GateThresholdMode::FloorRelative;
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
    if (floorRel) {  // the floor is read off the same peak envelope the gate compares (H.1)
      frameMax_ = std::max(frameMax_, env_);
      if (++frameCount_ >= frameLen_) {
        floorFrame(frameMax_);
        frameMax_ = 0.0;
        frameCount_ = 0;
      }
    }

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
