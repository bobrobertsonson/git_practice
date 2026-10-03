#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sawblade/chain.h"
#include "sawblade/preset.h"
#include "sawblade/wav_io.h"

// Offline rendering: the single library entry point shared by `tonerender` and the golden tests.
namespace sawblade {

enum class RenderErrorKind {
  Preset,  // the preset is invalid, including semantic errors that only show up at load/prepare
           // time for the render rate (EQ band >= 0.49 fs, NAM sample-rate mismatch, ...)
  Io       // a file could not be read/written, a capture failed to load, a hash did not match
};

class RenderError : public std::runtime_error {
 public:
  RenderError(RenderErrorKind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}
  RenderErrorKind kind() const noexcept { return kind_; }

 private:
  RenderErrorKind kind_;
};

enum class OutRate {
  Input,  // convert the rendered audio back to the input rate (default)
  Render  // keep the render rate
};

struct RenderOptions {
  int blockSize = 256;                      // processing block size, 1..65536
  // Render rate. none = auto: the rate the preset's NAM models were trained at (all non-bypassed
  // NAM blocks on enabled paths with a known rate must agree, else RenderError Preset naming the
  // blocks); a preset with no such blocks renders at the input rate.
  std::optional<double> renderRate;
  OutRate outRate = OutRate::Input;
  std::optional<double> normalizePeakDbfs;  // scale the output so its peak equals this; none by default
};

struct SignalStats {
  double peakDbfs = 0.0;  // -infinity for digital silence
  double rmsDbfs = 0.0;
};

struct CaptureAttribution {
  std::string where;  // JSON path of the capture in the preset, e.g. "paths.a.blocks[0].model"
  std::string file;
  CaptureSource source;
};

struct RenderResult {
  std::string presetName;
  double sampleRate = 0.0;     // rate of `samples` (== outputRate)
  double inputRate = 0.0;
  double renderRate = 0.0;     // the rate the chain ran at; info.* latencies are in samples at this rate
  double outputRate = 0.0;
  int blockSize = 0;
  // Mono. Length N (the input length) when outputRate == inputRate, else round(N * out / in).
  // Advanced by info.latencySamples (at the render rate); resampling adds no delay.
  std::vector<float> samples;
  ChainInfo info;
  SignalStats input, output;   // output stats are after normalization
  double normalizeGainDb = 0.0;
  double prepareSeconds = 0.0;  // chain prepare() incl. NAM prewarm and the alignment probe
  double renderSeconds = 0.0;   // wall time of the process() loop only
  double resampleSeconds = 0.0;  // wall time of the input and output sample-rate conversions
  double realTimeFactor = 0.0;  // renderSeconds / (input duration); < 1 is faster than real time
  std::vector<std::string> warnings;  // chain + render warnings
  std::vector<CaptureAttribution> captures;  // every capture that has `source`
};

// Renders `in` (mono; extra channels are dropped with a warning) through `preset`. If the render
// rate differs from the input rate the input is resampled (resample.h) before and the result
// after the chain; equal rates involve no resampling (bit-identical). The output is
// the same length as the input (at the output rate) and latency-compensated: advanced by Chain::latencySamples() (the
// tail is flushed with zeros). The alignment delay is part of the tone and stays in the audio.
// Throws RenderError.
RenderResult renderPreset(const Preset& preset, const AudioFile& in, const RenderOptions& opts = {});

// loadPresetFile + readWav + renderPreset. Throws RenderError.
RenderResult renderFile(const std::filesystem::path& presetPath, const std::filesystem::path& inputWav,
                        const RenderOptions& opts = {});

// Writes `r.samples` as a float32 mono WAV at r.sampleRate. Throws RenderError (Io).
void writeRenderedWav(const std::filesystem::path& path, const RenderResult& r);

// The report written by `tonerender --report`.
nlohmann::json reportJson(const RenderResult& r);

}  // namespace sawblade
