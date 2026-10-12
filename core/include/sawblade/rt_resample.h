#pragma once

#include <cstdint>
#include <vector>

// Real-time fixed-ratio polyphase sample-rate converter (streaming, mono, float).
// Separate from the offline resample.h: this one is built once (allocating, prepare()) and then
// converts a stream in arbitrary block sizes with no allocation, locks or exceptions.
namespace sawblade {

// Filter: the same Kaiser-windowed sinc as the offline converter (resample.h constants: beta 10,
// cutoff 0.97 and transition 0.14 of the lower Nyquist, ~99 dB design stopband), one kernel per
// output phase, float coefficients, unity DC gain per phase.
//
// Timing model (all integer arithmetic, so it is exact and independent of block sizes). With the
// reduced ratio L/M (L = output-rate factor, M = input-rate factor), output sample j is the
// band-limited input evaluated at input time
//     t_j = j * M / L - (H + s / L)           [input samples; the stream starts at 0, zero before]
// H = ceil(kernel half-width) is the filter's causal delay and s the phase offset (default 0).
// Output j depends on input samples <= floor(j * M / L) only, so the converter never needs
// look-ahead beyond the samples it has been given: after consuming N input samples it has
// produced every output j with floor((j*M - s)/L) <= N - 1, i.e. about N * L / M of them.
//
// Latency: the delay of the conversion is delayInputSamples() = H + s / L input samples
// (delayOutputSamples() in output samples). s can be chosen with phaseOffsetForIntegerDelay() so
// that, together with some extra upstream delay, the total delay is a whole number of output
// samples; the plugin uses this to report an exact integer latency.
class RtResampler {
 public:
  struct Ratio {
    std::int64_t L = 1, M = 1;  // output rate / input rate == L / M, reduced
  };

  static constexpr std::int64_t kMaxPhases = 8192;

  // Exact reduced ratio for integral rates (|rate - round(rate)| < 1e-6); otherwise the best
  // rational approximation with L <= kMaxPhases (continued fractions; relative error < 1e-7).
  // Throws std::invalid_argument for non-positive/non-finite rates or ratios outside 1/64..64.
  static Ratio ratioFor(double fromRate, double toRate);

  // Allocates. maxBlock is the largest `nIn` that process() will be given. Throws
  // std::invalid_argument on bad arguments. The rates are used for the filter design (cutoff
  // relative to the lower Nyquist) and, via ratioFor(), for L/M.
  void prepare(double fromRate, double toRate, int maxBlock);
  // As above with an explicit ratio (so that a forward and a reverse converter use exactly
  // reciprocal ratios even when the rates are not integral).
  void prepare(double fromRate, double toRate, Ratio ratio, int maxBlock);

  // Clears the stream state (next output is j = 0). Keeps the phase offset. RT-safe.
  void reset() noexcept;

  // Sets s (>= 0; typically from phaseOffsetForIntegerDelay) and resets the stream.
  void setPhaseOffset(std::int64_t s) noexcept;
  std::int64_t phaseOffset() const noexcept { return s_; }

  // The s in [0, M) such that (extraInputDelay + H + s/L) * L / M is an integer, i.e. such that
  // `extraInputDelay` samples of delay upstream of this converter, plus this converter's own
  // delay, come to a whole number of output samples.
  std::int64_t phaseOffsetForIntegerDelay(std::int64_t extraInputDelay) const noexcept;

  // Converts nIn (<= maxBlock) input samples; writes the outputs that became available to `out`
  // and returns their count (0 .. maxOutputFor(nIn)). `out` must hold maxOutputFor(nIn) samples.
  // `in` and `out` must not overlap. RT-safe.
  int process(const float* in, int nIn, float* out) noexcept;

  int maxOutputFor(int nIn) const noexcept {
    return static_cast<int>(static_cast<std::int64_t>(nIn) * ratio_.L / ratio_.M) + 2;
  }

  Ratio ratio() const noexcept { return ratio_; }
  int taps() const noexcept { return taps_; }
  int wholeDelay() const noexcept { return reach_; }  // H, input samples
  double delayInputSamples() const noexcept {
    return static_cast<double>(reach_) + static_cast<double>(s_) / static_cast<double>(ratio_.L);
  }
  double delayOutputSamples() const noexcept {
    return delayInputSamples() * static_cast<double>(ratio_.L) / static_cast<double>(ratio_.M);
  }
  bool prepared() const noexcept { return !table_.empty(); }

 private:
  Ratio ratio_;
  int reach_ = 0, taps_ = 0, maxBlock_ = 0;
  std::int64_t s_ = 0;
  std::vector<float> table_;  // L phases x taps
  std::vector<float> buf_;    // history + incoming block
  int len_ = 0;               // valid samples in buf_
  std::int64_t inCount_ = 0;  // total input samples consumed; buf_[0] has global index inCount_ - len_
  std::int64_t outIdx_ = 0;   // next output index j
};

}  // namespace sawblade
