#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include <limits>

#include "sawblade/processor.h"

namespace sawblade {

// Parameters and defaults per docs/PRESET_SCHEMA.md. (`enabled` defaults to true here; the
// preset parser applies "enabled=false when the gate object is omitted".)
enum class GateMode { Gate, Expander };
enum class GateReleaseCurve { OnePole, LinearDb };
// absolute: thresholdDb is used as set. floorRelative: threshold = the key's tracked noise floor + floorOffsetDb
// (thresholdDb is then unused); see Gate "floor follower".
enum class GateThresholdMode { Absolute, FloorRelative };

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
  // Live dynamics policy (preset v4). Defaults reproduce the earlier behaviour bit-for-bit.
  GateThresholdMode thresholdMode = GateThresholdMode::Absolute;
  double floorOffsetDb = 10.0;  // floorRelative only: open threshold = floor estimate + this
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
// Floor follower (thresholdMode floorRelative): minimum statistics on the gate's own peak envelope (key-high-passed).
// Frame statistic: the maximum of the peak envelope over 50 ms;
// only frames below estimate + 20 dB feed the sub-window minima (playing never feeds the floor); the estimate is the
// minimum over a 3 s window held as a fixed ring of 100 ms sub-window minima. When no frame has qualified for 10 s the
// estimate leaks up at +1 dB/s. Clamped to [-96, -40] dBFS, seeded at -70 dBFS (until the first window has filled the
// estimate is min(seed, running minimum)). Everything is counted in samples, so the result does not depend on the block
// size. The thresholds are refreshed whenever the estimate changes (frame boundaries).
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

  // Floor follower constants.
  static constexpr double kFrameMs = 50.0;
  static constexpr int kFramesPerSub = 2;       // 100 ms sub-windows
  static constexpr int kSubWindows = 30;        // 3 s
  static constexpr double kQualifyDb = 20.0;    // a frame feeds the minima when below estimate + this
  static constexpr double kLeakAfterS = 10.0;
  static constexpr double kLeakDbPerS = 1.0;
  static constexpr double kFloorSeedDb = -70.0, kFloorMinDb = -96.0, kFloorMaxDb = -40.0;

  // The tracked floor estimate (dBFS) and the open threshold in force (dBFS). Any thread may read after the audio stops.
  double floorEstimateDb() const noexcept { return floorDb_; }
  double openThresholdDb() const noexcept { return openDb_; }

  bool isOpen() const noexcept { return open_; }
  double gainDb() const noexcept { return gainDb_; }

 private:
  void updateCoefficients() noexcept;
  void setThresholdDb(double openDb) noexcept;
  void resetFloor() noexcept;
  void floorFrame(double frameMaxEnv) noexcept;

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

  // Floor follower state (floorRelative only).
  static constexpr double kInf = std::numeric_limits<double>::infinity();
  std::array<double, kSubWindows> ring_{};
  int ringHead_ = 0, subsDone_ = 0, frameInSub_ = 0;
  double subMin_ = kInf;
  int frameLen_ = 2400, frameCount_ = 0;
  double frameMax_ = 0.0;
  int sinceQualFrames_ = 0;
  double floorDb_ = kFloorSeedDb;
  double openDb_ = -55.0;
};

// peakFloorDb: the matcher's DI floor, defined on the gate's own detector: the 92.5th percentile of the peak envelope
// (kEnvAttackMs / kEnvReleaseMs, after the key high-pass when keyHpfHz > 0) over the samples where `mask` is non-zero (all
// samples when null). dBFS; NaN when no sample is selected. Off the audio thread (allocates).
double peakFloorDb(const std::vector<float>& x, double fs, double keyHpfHz = 0.0, const std::vector<std::uint8_t>* mask = nullptr);

}  // namespace sawblade
