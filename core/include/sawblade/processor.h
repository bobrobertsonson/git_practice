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
  // Live block parameters (the BlockType::liveParams descriptor order; enums as choice indexes).
  // RT-safe: no allocation, locks, I/O or exceptions. Call from the thread that calls process() (or
  // serialise with it). Default: ignore.
  virtual void setLiveParams(const float* /*values*/, int /*count*/) noexcept {}
};

}  // namespace sawblade
