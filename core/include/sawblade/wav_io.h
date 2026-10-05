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

// Load-time only. Reads a WAV (readWav), FLAC or MP3 file, chosen by the extension (".wav" / ".flac" /
// ".mp3", case-insensitive); FLAC is decoded to float in [-1, 1] (16/24-bit exact), MP3 with dr_mp3
// (the decoder's delay / padding frames are not trimmed). m4a / aac are not supported in core (the
// plugin decodes them with the platform decoder). Throws
// std::runtime_error (message contains the path) for any other extension or on decode failure.
AudioFile readAudioFile(const std::filesystem::path& path);

// Writes a mono 32-bit float WAV. Throws std::runtime_error (message contains the path).
void writeWavFloat32(const std::filesystem::path& path, double sampleRate,
                     const std::vector<float>& mono);

// Writes a stereo 32-bit float WAV. `left` and `right` must have equal lengths. Throws
// std::runtime_error (message contains the path).
void writeWavFloat32Stereo(const std::filesystem::path& path, double sampleRate,
                           const std::vector<float>& left, const std::vector<float>& right);

}  // namespace sawblade
