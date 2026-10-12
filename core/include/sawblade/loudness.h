#pragma once

#include <cstdint>
#include <optional>

// ITU-R BS.1770-4 integrated loudness. Load-time only (allocates). Used for the backing-loudness
// metadata of a StemSet; nothing in the audio path applies gain from it.
namespace sawblade {

struct LoudnessBiquad {
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;  // y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2
};

struct KWeighting {
  LoudnessBiquad shelf;     // stage 1: high shelf (head model)
  LoudnessBiquad highpass;  // stage 2: RLB high-pass
};

// K-weighting filters designed for any sample rate; at 48 kHz they equal the BS.1770-4 table values.
KWeighting designKWeighting(double sampleRate);

// Integrated loudness in LUFS of a stereo signal (channel weights 1.0): K-weighting, 400 ms blocks
// with 75 % overlap, absolute gate -70 LUFS, relative gate -10 LU. Returns none when no block passes
// the absolute gate (silence) or the signal is shorter than one block. Double accumulation.
std::optional<double> integratedLoudnessLufs(const float* left, const float* right, std::int64_t frames,
                                             double sampleRate);

}  // namespace sawblade
