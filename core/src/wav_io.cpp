#include "sawblade/wav_io.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <algorithm>
#include <cctype>

#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"

namespace sawblade {
namespace {

[[noreturn]] void fail(const std::filesystem::path& path, const std::string& what) {
  throw std::runtime_error("WAV error (" + path.string() + "): " + what);
}

}  // namespace

AudioFile readWav(const std::filesystem::path& path) {
  drwav wav;
  const std::string p = path.string();
  if (!drwav_init_file(&wav, p.c_str(), nullptr)) fail(path, "cannot open or not a valid WAV file");

  struct Closer {
    drwav* w;
    ~Closer() { drwav_uninit(w); }
  } closer{&wav};

  const bool pcm = wav.translatedFormatTag == DR_WAVE_FORMAT_PCM &&
                   (wav.bitsPerSample == 16 || wav.bitsPerSample == 24 || wav.bitsPerSample == 32);
  const bool flt = wav.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT && wav.bitsPerSample == 32;
  if (!pcm && !flt) fail(path, "unsupported sample format (need 16/24/32-bit PCM or 32-bit float)");
  if (wav.channels == 0 || wav.sampleRate == 0) fail(path, "invalid channel count or sample rate");

  AudioFile out;
  out.sampleRate = static_cast<double>(wav.sampleRate);
  out.channels = static_cast<int>(wav.channels);
  const drwav_uint64 frames = wav.totalPCMFrameCount;
  out.interleaved.resize(static_cast<std::size_t>(frames) * wav.channels);
  const drwav_uint64 got = drwav_read_pcm_frames_f32(&wav, frames, out.interleaved.data());
  if (got != frames) fail(path, "truncated file (read " + std::to_string(got) + " of " +
                                    std::to_string(frames) + " frames)");
  return out;
}

namespace {

AudioFile readFlac(const std::filesystem::path& path) {
  const std::string p = path.string();
  unsigned int channels = 0, rate = 0;
  drflac_uint64 frames = 0;
  float* data = drflac_open_file_and_read_pcm_frames_f32(p.c_str(), &channels, &rate, &frames, nullptr);
  if (data == nullptr) fail(path, "cannot open or not a valid FLAC file");
  struct Freer {
    float* d;
    ~Freer() { drflac_free(d, nullptr); }
  } freer{data};
  if (channels == 0 || rate == 0) fail(path, "invalid channel count or sample rate");
  AudioFile out;
  out.sampleRate = static_cast<double>(rate);
  out.channels = static_cast<int>(channels);
  out.interleaved.assign(data, data + static_cast<std::size_t>(frames) * channels);
  return out;
}

AudioFile readMp3(const std::filesystem::path& path) {
  const std::string p = path.string();
  drmp3_config cfg{};
  drmp3_uint64 frames = 0;
  float* data = drmp3_open_file_and_read_pcm_frames_f32(p.c_str(), &cfg, &frames, nullptr);
  if (data == nullptr) fail(path, "cannot open or not a valid MP3 file");
  struct Freer {
    float* d;
    ~Freer() { drmp3_free(d, nullptr); }
  } freer{data};
  if (cfg.channels == 0 || cfg.sampleRate == 0) fail(path, "invalid channel count or sample rate");
  AudioFile out;
  out.sampleRate = static_cast<double>(cfg.sampleRate);
  out.channels = static_cast<int>(cfg.channels);
  out.interleaved.assign(data, data + static_cast<std::size_t>(frames) * cfg.channels);
  return out;
}

}  // namespace

AudioFile readAudioFile(const std::filesystem::path& path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (ext == ".wav") return readWav(path);
  if (ext == ".flac") return readFlac(path);
  if (ext == ".mp3") return readMp3(path);
  throw std::runtime_error("audio error (" + path.string() + "): unsupported file type (need .wav, .flac or .mp3)");
}

void writeWavFloat32(const std::filesystem::path& path, double sampleRate,
                     const std::vector<float>& mono) {
  if (!(sampleRate >= 1.0) || sampleRate > 4294967295.0) fail(path, "invalid sample rate");

  drwav_data_format fmt;
  fmt.container = drwav_container_riff;
  fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
  fmt.channels = 1;
  fmt.sampleRate = static_cast<drwav_uint32>(std::llround(sampleRate));
  fmt.bitsPerSample = 32;

  drwav wav;
  const std::string p = path.string();
  if (!drwav_init_file_write(&wav, p.c_str(), &fmt, nullptr)) fail(path, "cannot create file");
  const drwav_uint64 written = drwav_write_pcm_frames(&wav, mono.size(), mono.data());
  drwav_uninit(&wav);
  if (written != mono.size()) fail(path, "short write");
}

void writeWavFloat32Stereo(const std::filesystem::path& path, double sampleRate,
                           const std::vector<float>& left, const std::vector<float>& right) {
  if (!(sampleRate >= 1.0) || sampleRate > 4294967295.0) fail(path, "invalid sample rate");
  if (left.size() != right.size()) fail(path, "left and right channels differ in length");

  std::vector<float> inter(left.size() * 2);
  for (std::size_t i = 0; i < left.size(); ++i) {
    inter[2 * i] = left[i];
    inter[2 * i + 1] = right[i];
  }
  drwav_data_format fmt;
  fmt.container = drwav_container_riff;
  fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
  fmt.channels = 2;
  fmt.sampleRate = static_cast<drwav_uint32>(std::llround(sampleRate));
  fmt.bitsPerSample = 32;

  drwav wav;
  const std::string p = path.string();
  if (!drwav_init_file_write(&wav, p.c_str(), &fmt, nullptr)) fail(path, "cannot create file");
  const drwav_uint64 written = drwav_write_pcm_frames(&wav, left.size(), inter.data());
  drwav_uninit(&wav);
  if (written != left.size()) fail(path, "short write");
}

}  // namespace sawblade
