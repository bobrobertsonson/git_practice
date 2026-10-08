#pragma once

#include <vector>

namespace sawblade {

// 4x oversampler: a cascade of two 2x polyphase half-band FIR stages (base rate -> 2x -> 4x on
// the way up, 4x -> 2x -> base rate on the way down).
//
// Why linear-phase FIR and not minimum-phase IIR half-bands: the group delay is constant (the
// clipper that runs in the oversampled domain sees an undistorted waveform, only delayed), and the
// delay is an exact integer number of oversampled samples, so a pedal can report an exact
// integer base-rate latency for host / chain compensation. Costs are the latency (a few dozen
// samples) and the multiplies; both are fixed and reported, never hidden.
//
// Design (every base rate; all edges are fractions of the base rate fs, so the same tables serve
// 44.1..192 kHz):
//  * stage 1 (fs -> 2 fs): Kaiser-windowed half-band, 87 taps, pass edge 0.4167 fs (20 kHz at
//    48 kHz), stop edge 0.5833 fs (28 kHz at 48 kHz);
//  * stage 2 (2 fs -> 4 fs): 27 taps; its images start at 1.5833 fs so the transition band can
//    be wide.
// Both stages are designed for >= 110 dB stopband in prepare() in double and stored as float.
// Aliases/images between 0.4167 fs and 0.5833 fs are allowed (transition band).
//
// A half-band has every second tap equal to zero except the centre tap (0.5); the polyphase form
// exploits that: one branch is a pure delay, the other a symmetric FIR evaluated with folded
// pairs. Output of the round trip up->down is the input delayed by roundTripLatencyOs() samples at
// 4 fs (an exact multiple of 2, see below).
//
// Threading: prepare() allocates; reset(), upsample() and downsample() are real-time safe.
// Block-size independent: the output for a sample stream does not depend on how it is chunked
// (bit-identical).
class OversamplerNx;

class Oversampler4x {
 public:
  static constexpr int kStage1Taps = 87;  // 4m+3
  static constexpr int kStage2Taps = 27;
  static constexpr double kDesignStopbandDb = 110.0;

  // One-way delays in samples of the stage's *output* rate (the centre tap position).
  static constexpr int kStage1Delay = (kStage1Taps - 1) / 2;  // at 2 fs
  static constexpr int kStage2Delay = (kStage2Taps - 1) / 2;  // at 4 fs
  // Delay of upsample() alone, and of downsample() alone, in samples at 4 fs.
  static constexpr int kOneWayLatencyOs = 2 * kStage1Delay + kStage2Delay;
  // up + down, in samples at 4 fs.
  static constexpr int roundTripLatencyOs() { return 2 * kOneWayLatencyOs; }

  // Design of a half-band lowpass (cutoff fs/4 of the stage's high rate): `numTaps` must be 4m+3;
  // returns h[0..numTaps-1] with h[centre] == 0.5, sum == 1, zeros at the even offsets. Exposed
  // for tests.
  static std::vector<double> designHalfBand(int numTaps, double kaiserBeta);
  static double kaiserBetaFor(double stopbandDb);

  // Allocates for blocks of up to maxBlock base-rate samples and designs the filters.
  void prepare(int maxBlock);
  void reset() noexcept;

  // n <= maxBlock base-rate samples in -> 4n out. `in` and `out4n` must not overlap.
  void upsample(const float* in, int n, float* out4n) noexcept;
  // 4n in -> n out. `in4n` and `out` must not overlap.
  void downsample(const float* in4n, int n, float* out) noexcept;

  int maxBlock() const noexcept { return maxBlock_; }

 private:
  friend class OversamplerNx;  // shares the half-band stage design and kernels
  struct Stage {
    int taps = 0, m = 0, k = 0;     // taps = 4m+3, k = 2m+2 coefficients of the even branch
    std::vector<float> even;        // up: 2*h[2i]; down: h[2i]   (i < k)
    std::vector<float> hist;        // up: k-1 samples; down: taps-1 samples
  };
  static void design(Stage& s, int taps, bool up);
  static void upStage(Stage& s, const float* in, int n, float* out, float* work) noexcept;
  static void downStage(Stage& s, const float* in, int n, float* out, float* work) noexcept;

  int maxBlock_ = 0;
  Stage up1_, up2_, down2_, down1_;
  std::vector<float> mid_, work_;
};

// Extra 2x / 4x / 8x oversampling that runs on top of an already oversampled signal (pedal.rat's op-amp stage),
// a cascade of 1..3 of the same polyphase half-band stages (kTaps = 19 each, >= 110 dB stopband; the signal to
// protect is below 0.11 of the incoming rate, so the transition band is wide). Linear phase: the round trip delay
// is an exact integer number of samples at the highest rate, roundTripLatencyHi() (2 * kDelay * (2^L - 1) where
// L = log2(factor)). Block-size independent (bit-identical). prepare() allocates; the rest is real-time safe.
class OversamplerNx {
 public:
  static constexpr int kTaps = 19;
  static constexpr int kDelay = (kTaps - 1) / 2;
  void prepare(int factor, int maxIn);  // factor in {2, 4, 8}; maxIn = largest n passed to up/downsample
  void reset() noexcept;
  int factor() const noexcept { return factor_; }
  // Round-trip delay in samples of the highest rate (factor * incoming rate).
  static constexpr int roundTripLatencyHi(int factor) {
    int total = 0, rate = 1;
    for (int f = factor; f > 1; f >>= 1) {
      rate <<= 1;  // output rate of this up stage, relative to the incoming one
      total += 2 * kDelay * (factor / rate);  // up + down, expressed at the highest rate
    }
    return total;
  }
  void upsample(const float* in, int n, float* outNF) noexcept;       // n -> n * factor (in, out must not overlap)
  void downsample(const float* inNF, int n, float* out) noexcept;     // n * factor -> n

 private:
  int factor_ = 1, levels_ = 0;
  std::vector<Oversampler4x::Stage> up_, down_;
  std::vector<float> a_, b_, work_;
};

}  // namespace sawblade
