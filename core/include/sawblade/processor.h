#pragma once

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
};

}  // namespace sawblade
