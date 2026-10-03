#pragma once

#include "sawblade/processor.h"

namespace sawblade {

// Parameters and defaults per docs/PRESET_SCHEMA.md. (`enabled` defaults to true here; the
// preset parser applies "enabled=false when the gate object is omitted".)
struct GateParams {
  bool enabled = true;
  double thresholdDb = -55.0;  // open threshold on the key envelope (dBFS)
  double hysteresisDb = 6.0;   // closes below thresholdDb - hysteresisDb
  double attackMs = 0.5;       // gain smoothing time constant when opening
  double holdMs = 20.0;        // minimum open time after the envelope falls below close threshold
  double releaseMs = 60.0;     // gain smoothing time constant when closing
  double rangeDb = -90.0;      // attenuation when fully closed
};

// Envelope: peak follower (0.1 ms attack / 10 ms release one-pole, fixed). State machine:
// opens when env >= open threshold; closes once env has stayed below the close threshold
// (open - hysteresis) for holdMs. The gain is smoothed by a one-pole toward its target
// (0 dB open, rangeDb closed) using attack/release as time constants. The one-pole runs in
// the dB domain: a linear-domain one-pole can only fall ~43 dB in 5 time constants, so it
// could never reach a -90 dB range within a sensible release time.
//
// Not a "hard" gate in the allocation sense: all processing is allocation-free.
class Gate : public Processor {
 public:
  void setParams(const GateParams& p) noexcept;  // call before prepare(), or between blocks
  const GateParams& params() const noexcept { return params_; }

  void prepare(const ProcessSpec& spec) override;
  void reset() noexcept override;

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

  double env_ = 0.0;
  bool open_ = false;
  int holdCount_ = 0;
  double gainDb_ = -90.0;
};

}  // namespace sawblade
