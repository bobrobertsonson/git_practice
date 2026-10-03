#include "sawblade/ir.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include "sawblade/resample.h"
#include "sawblade/wav_io.h"

namespace sawblade {
std::vector<float> resampleSinc(const std::vector<float>& in, double fromRate, double toRate) {
  return resample(in, fromRate, toRate, ResampleProfile::Ir);
}

void normalizeL2(std::vector<float>& ir) {
  double e = 0.0;
  for (const float v : ir) e += static_cast<double>(v) * v;
  if (e <= 0.0) return;
  const double g = 1.0 / std::sqrt(e);
  for (float& v : ir) v = static_cast<float>(v * g);
}

IrData loadIr(const std::filesystem::path& path, double targetSampleRate, bool normalize) {
  if (!(targetSampleRate > 0.0))
    throw std::runtime_error("IR error (" + path.string() + "): invalid target sample rate");
  const AudioFile f = readWav(path);
  IrData out;
  out.sourceSampleRate = f.sampleRate;

  const auto ch = static_cast<std::size_t>(f.channels);
  const std::size_t frames = f.interleaved.size() / ch;
  std::vector<float> mono(frames);
  for (std::size_t i = 0; i < frames; ++i) mono[i] = f.interleaved[i * ch];
  if (f.channels > 1)
    out.warnings.push_back(path.string() + ": " + std::to_string(f.channels) +
                           "-channel IR; using the left channel only");
  if (mono.empty()) throw std::runtime_error("IR error (" + path.string() + "): file contains no samples");

  out.samples = resampleSinc(mono, f.sampleRate, targetSampleRate);

  const auto maxLen = static_cast<std::size_t>(std::llround(kMaxIrSeconds * targetSampleRate));
  if (out.samples.size() > maxLen) {
    out.samples.resize(maxLen);
    out.warnings.push_back(path.string() + ": IR truncated to 2.0 s");
  }
  if (normalize) normalizeL2(out.samples);
  return out;
}

}  // namespace sawblade
