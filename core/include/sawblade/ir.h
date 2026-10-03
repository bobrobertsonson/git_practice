#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace sawblade {

constexpr double kMaxIrSeconds = 2.0;

struct IrData {
  std::vector<float> samples;          // mono, at the target sample rate
  std::vector<std::string> warnings;   // e.g. stereo -> left channel used, truncation
  double sourceSampleRate = 0.0;
};

// Load-time only. Mono WAV (stereo/multichannel: left channel + warning), resampled to
// targetSampleRate with an offline Kaiser-windowed sinc (64 input taps per side at unity
// cutoff; wider when downsampling), truncated to 2.0 s at the target rate, and, if `normalize`,
// scaled so the L2 norm is 1. Throws std::runtime_error (path in message).
IrData loadIr(const std::filesystem::path& path, double targetSampleRate, bool normalize = true);

// The pure-DSP pieces, exposed for reuse and tests.
std::vector<float> resampleSinc(const std::vector<float>& in, double fromRate, double toRate);
void normalizeL2(std::vector<float>& ir);

}  // namespace sawblade
