#pragma once

#include "sawblade/processor.h"

namespace sawblade {

// Parameters and defaults per docs/PRESET_SCHEMA.md. (`enabled` defaults to true here; the
// preset parser applies "enabled=false when the gate object is omitted".)
enum class GateMode { Gate, Expander };
enum class GateReleaseCurve { OnePole, LinearDb };

struct GateParams {
  bool enabled = true;
  double thresholdDb = -55.0;  // open threshold on the key envelope (dBFS)
  double hysteresisDb = 6.0;   // closes below thresholdDb - hysteresisDb
  double attackMs = 0.5;       // gain smoothing time constant when opening
  double holdMs = 20.0;        // minimum open time after the envelope falls below close threshold
  double releaseMs = 60.0;     // gain smoothing time constant when closing
  double rangeDb = -90.0;      // attenuation when fully closed
  // Phase 3.5. Defaults reproduce the original behaviour bit-for-bit.
  GateMode mode = GateMode::Gate;
  double ratio = 4.0;          // expander only: downward ratio below the close threshold (1.5-10)
  double keyHighPassHz = 0.0;  // 0 = off, else 40-400 Hz, 12 dB/oct; filters the key only
  GateReleaseCurve releaseCurve = GateReleaseCurve::OnePole;  // linear-db: -rangeDb/releaseMs dB per ms
};

// Envelope: peak follower (0.1 ms attack / 10 ms release one-pole, fixed). State machine:
// opens when env >= open threshold; closes once env has stayed below the close threshold
// (open - hysteresis) for holdMs. The gain is smoothed by a one-pole toward its target
// (0 dB open, rangeDb closed) using attack/release as time constants. The one-pole runs in
// the dB domain: a linear-domain one-pole can only fall ~43 dB in 5 time constants, so it
// could never reach a -90 dB range within a sensible release time.
//
// Expander mode: while closed, the gain target is not the flat rangeDb but the downward
// expansion curve of the key envelope: gainDb = max(rangeDb, -(ratio-1) * (closeDb - envDb)),
// 0 dB at or above the close threshold. Hysteresis and hold are unchanged. The key may be
// high-passed (biquad Butterworth, key only) so low-string rumble does not hold the gate open.
// releaseCurve "linear-db" slews the falling gain at a constant dB/ms (rangeDb over releaseMs)
// instead of the one-pole, which is slow near the end of a deep fall.
//
// Not a "hard" gate in the allocation sense: all processing is allocation-free.
class Gate : public Processor {
 public:
  // Fixed envelope detector time constants (schema v1).
  static constexpr double kEnvAttackMs = 0.1;
  static constexpr double kEnvReleaseMs = 10.0;

  void setParams(const GateParams& p) noexcept;  // call before prepare(), or between blocks
  const GateParams& params() const noexcept { return params_; }

  void prepare(const ProcessSpec& spec) override;
  void reset() override;

  // Self-keyed.
  void process(float* io, int numSamples) noexcept override { processKeyed(io, io, numSamples); }

  // key may alias io (e.g. key = DI before the split). The key sample is read before io is
  // written, per sample.
  void processKeyed(const float* key, float* io, int numSamples) noexcept;

  bool isOpen() const noexcept { return open_; }
  double gainDb() const noexcept { return gainDb_; }

 private:
  void updateCoefficients() noexcept;

  GateParams params_{};
  double sampleRate_ = 48000.0;

  double envAtk_ = 0.0, envRel_ = 0.0;     // one-pole coefficients for the envelope
  double gainAtk_ = 0.0, gainRel_ = 0.0;   // one-pole coefficients for the gain (dB domain)
  double openLin_ = 0.0, closeLin_ = 0.0;
  int holdSamples_ = 0;

  // Key high-pass biquad (transposed direct form II), coefficients in double.
  bool keyHp_ = false;
  double kb0_ = 1.0, kb1_ = 0.0, kb2_ = 0.0, ka1_ = 0.0, ka2_ = 0.0;
  double kz1_ = 0.0, kz2_ = 0.0;
  double closeDb_ = 0.0;
  double rampStepDb_ = 0.0;  // dB per sample, linear-db release

  double env_ = 0.0;
  bool open_ = false;
  int holdCount_ = 0;
  double gainDb_ = -90.0;
};

}  // namespace sawblade
