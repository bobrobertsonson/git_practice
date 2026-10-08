#pragma once

#include "sawblade/calibration.h"

namespace sawblade {

struct ProcessSpec {
  double sampleRate;
  int maxBlockSize;
};

// Mono, in-place DSP block. prepare() and reset() may allocate/throw (not RT-safe); only
// process() is RT-safe.
// process() is real-time safe: no allocation, locks, I/O, exceptions or logging.
class Processor {
 public:
  virtual ~Processor() = default;
  virtual void prepare(const ProcessSpec&) = 0;
  virtual void reset() = 0;
  virtual void process(float* io, int numSamples) noexcept = 0;  // n <= maxBlockSize
  virtual int latencySamples() const noexcept { return 0; }
  // RT-safe. Live input / output gain (dB) ramped linearly over `rampSamples`. Returns false when
  // the block has no such gains (the default).
  virtual bool setLiveGainsDb(double /*inDb*/, double /*outDb*/, int /*rampSamples*/) noexcept { return false; }
  // v0.8 input calibration. What the block contributes to the level plan: Neutral (no level conversion) by default; a
  // capture reports its metadata (Nam). Immutable after construction, any thread.
  virtual calibration::BlockLevelInfo levelInfo() const noexcept { return {}; }
  // RT-safe. Applies the planned calibration gain (see calibration::BlockCalibration) on top of the block's own gains,
  // ramped over `rampSamples` like setLiveGainsDb. Blocks without level metadata ignore it (the default).
  virtual void setCalibration(const calibration::BlockCalibration& /*c*/, int /*rampSamples*/) noexcept {}
  // Live block parameters (the BlockType::liveParams descriptor order; enums as choice indexes).
  // RT-safe: no allocation, locks, I/O or exceptions. Call from the thread that calls process() (or
  // serialise with it). Default: ignore.
  virtual void setLiveParams(const float* /*values*/, int /*count*/) noexcept {}
};

}  // namespace sawblade
