#pragma once

namespace sawblade {

struct ProcessSpec {
  double sampleRate;
  int maxBlockSize;
};

// Mono, in-place DSP block. prepare() may allocate; reset() and process() must not.
// process() is real-time safe: no allocation, locks, I/O, exceptions or logging.
class Processor {
 public:
  virtual ~Processor() = default;
  virtual void prepare(const ProcessSpec&) = 0;
  virtual void reset() noexcept = 0;
  virtual void process(float* io, int numSamples) noexcept = 0;  // n <= maxBlockSize
  virtual int latencySamples() const noexcept { return 0; }
};

}  // namespace sawblade
