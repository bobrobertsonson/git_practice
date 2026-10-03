#pragma once

#include <filesystem>
#include <vector>

namespace sawblade {

struct AudioFile {
  double sampleRate = 0.0;
  int channels = 0;
  std::vector<float> interleaved;  // frames * channels samples, nominal range [-1, 1]
};

// Load-time only (allocates, does I/O). Reads 16/24/32-bit PCM and 32-bit float WAV.
// Throws std::runtime_error (message contains the path) on any failure.
AudioFile readWav(const std::filesystem::path& path);

// Writes a mono 32-bit float WAV. Throws std::runtime_error (message contains the path).
void writeWavFloat32(const std::filesystem::path& path, double sampleRate,
                     const std::vector<float>& mono);

}  // namespace sawblade
