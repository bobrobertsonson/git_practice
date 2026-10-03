#pragma once

#include <cstddef>
#include <vector>

// High-quality offline sample-rate conversion shared by IR loading and tonerender's render I/O.
// Not real-time safe: allocates, and runs in O(N * taps).
namespace sawblade {

// Design (see resample.cpp): Kaiser-windowed sinc, beta 10 (about 99 dB stopband), cutoff at 0.97 of
// the lower Nyquist with a transition band of 0.14 of it (44.1 <-> 48 kHz: -6 dB at 21.4 kHz,
// passband edge 19.8 kHz, stopband edge 22.9 kHz). Linear phase and zero net delay: output sample j
// is the band-limited signal evaluated at input time j * fromRate / toRate, so the output is
// time-aligned with the input. Rational ratios use an exact polyphase table; other ratios evaluate
// the kernel per output sample.
constexpr double kResampleKaiserBeta = 10.0;
constexpr double kResampleStopbandDb = 99.0;  // design attenuation, used to size the kernel

// The default output length of resample(): round(n * toRate / fromRate).
std::size_t resampledLength(std::size_t n, double fromRate, double toRate);

// Returns `in` unchanged (bit-identical) when fromRate == toRate. Throws std::invalid_argument for
// non-positive or non-finite rates.
std::vector<float> resample(const std::vector<float>& in, double fromRate, double toRate);

// As above with an explicit output length (used to convert back to exactly the original length).
std::vector<float> resample(const std::vector<float>& in, double fromRate, double toRate, std::size_t outLen);

}  // namespace sawblade
