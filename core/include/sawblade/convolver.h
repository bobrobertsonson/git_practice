#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "sawblade/processor.h"

namespace sawblade {

// Zero-latency uniformly partitioned convolution (mono, float).
//
//   * Head: the first 128 taps run as a direct-form FIR in the time domain, per sample.
//   * Tail: taps 128..L-1 are split into 128-tap partitions, FFT size 256 (PFFFT, overlap-save),
//     with a frequency-domain delay line of past input spectra.
//   * Input is buffered per sample; the tail partitions are processed at *fixed* 128-sample
//     boundaries of the stream, whatever the host block size. Output is therefore bit-identical
//     for every host block size and latencySamples() == 0.
//
// CPU profile (important for RT budgeting): the head costs 128 MACs per sample, but the tail
// work (1 forward FFT + one complex multiply-accumulate per partition + 1 inverse FFT) happens
// all at once on the sample that completes each 128-sample block. For a 2 s IR at 48 kHz that is
// ~750 partitions per spike, so the worst-case callback is far more expensive than the average
// one. With host blocks smaller than 128 only some callbacks pay it. This is the classic
// trade-off of uniform partitioning; a non-uniform scheme would smooth it (not needed in phase 1).
//
// setIr() allocates (call it off the audio thread, then hand the object over e.g. via
// SwapSlot). prepare()/reset()/process() do not allocate in this implementation (only process() is contractually RT-safe). With no IR set, the convolver passes
// audio through unchanged.
class Convolver : public Processor {
 public:
  static constexpr int kPartition = 128;
  static constexpr int kFftSize = 2 * kPartition;

  Convolver();
  ~Convolver() override;
  Convolver(const Convolver&) = delete;
  Convolver& operator=(const Convolver&) = delete;

  // Replaces the impulse response (any length >= 1) and clears state. Throws
  // std::invalid_argument on an empty IR.
  void setIr(const std::vector<float>& ir);

  void prepare(const ProcessSpec& spec) override;  // host block size is irrelevant to the design
  void reset() override;
  void process(float* io, int numSamples) noexcept override;
  int latencySamples() const noexcept override { return 0; }

  std::size_t irLength() const noexcept { return irLength_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::size_t irLength_ = 0;
};

}  // namespace sawblade
