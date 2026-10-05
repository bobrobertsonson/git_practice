#include "PreviewRender.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

#include "BinaryData.h"
#include "sawblade/render.h"
#include "sawblade/resample.h"

namespace sawblade::plugin {
namespace {
std::uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint16_t rd16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
}  // namespace

AudioFile decodeWavMemory(const void* data, std::size_t size) {
  const auto* b = static_cast<const unsigned char*>(data);
  if (size < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0) throw std::runtime_error("not a WAV");
  int fmtTag = 0, channels = 0, bits = 0;
  double rate = 0.0;
  const unsigned char* pcm = nullptr;
  std::size_t pcmSize = 0;
  for (std::size_t pos = 12; pos + 8 <= size;) {
    const std::size_t len = rd32(b + pos + 4);
    const unsigned char* body = b + pos + 8;
    const std::size_t avail = std::min(len, size - pos - 8);
    if (std::memcmp(b + pos, "fmt ", 4) == 0 && avail >= 16) {
      fmtTag = rd16(body);
      channels = rd16(body + 2);
      rate = rd32(body + 4);
      bits = rd16(body + 14);
    } else if (std::memcmp(b + pos, "data", 4) == 0) {
      pcm = body;
      pcmSize = avail;
    }
    pos += 8 + len + (len & 1u);
  }
  if (pcm == nullptr || channels < 1 || rate <= 0.0) throw std::runtime_error("incomplete WAV");
  const int bytes = bits / 8;
  const bool isFloat = fmtTag == 3 && bits == 32;
  if (!(isFloat || (fmtTag == 1 && (bits == 16 || bits == 24 || bits == 32)))) throw std::runtime_error("unsupported WAV format");
  const std::size_t frames = pcmSize / static_cast<std::size_t>(bytes * channels);
  AudioFile a;
  a.sampleRate = rate;
  a.channels = 1;
  a.interleaved.resize(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const unsigned char* p = pcm + i * static_cast<std::size_t>(bytes * channels);
    float v;
    if (isFloat) {
      std::uint32_t u = rd32(p);
      std::memcpy(&v, &u, 4);
    } else if (bits == 16) {
      v = static_cast<float>(static_cast<std::int16_t>(rd16(p))) / 32768.0f;
    } else if (bits == 24) {
      std::int32_t s = static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
      if (s & 0x800000) s -= 0x1000000;
      v = static_cast<float>(s) / 8388608.0f;
    } else {
      v = static_cast<float>(static_cast<std::int32_t>(rd32(p)) / 2147483648.0);
    }
    a.interleaved[i] = v;
  }
  return a;
}

AudioFile embeddedPreviewRiff() { return decodeWavMemory(BinaryData::preview_riff_wav, static_cast<std::size_t>(BinaryData::preview_riff_wavSize)); }

std::vector<float> renderPreview(const Preset& preset, const AudioFile& riff, double hostRate, CaptureCache* cache, std::string& error) {
  try {
    AudioFile in;
    in.channels = 1;
    in.sampleRate = hostRate;
    in.interleaved = std::fabs(riff.sampleRate - hostRate) < 1e-9 ? riff.interleaved : resample(riff.interleaved, riff.sampleRate, hostRate);
    RenderOptions o;
    o.outRate = OutRate::Input;
    o.normalizePeakDbfs = kPreviewNormalizeDbfs;
    o.cache = cache;
    RenderResult r = renderPreset(preset, in, o);
    return std::move(r.samples);
  } catch (const std::exception& e) {
    error = e.what();
    return {};
  }
}

}  // namespace sawblade::plugin
