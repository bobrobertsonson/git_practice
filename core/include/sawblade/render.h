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
  RenderError(RenderErrorKind kind, const std::string& message, std::string jsonPath = {})
      : std::runtime_error(message), kind_(kind), jsonPath_(std::move(jsonPath)) {}
  RenderErrorKind kind() const noexcept { return kind_; }
  // JSON path of the offending preset member when known (PresetError path, or the `file` member of
  // a capture that failed to load); empty otherwise. what() is unchanged by it.
  const std::string& jsonPath() const noexcept { return jsonPath_; }

 private:
  RenderErrorKind kind_;
  std::string jsonPath_;
};

enum class OutRate {
  Input,  // convert the rendered audio back to the input rate (default)
  Render  // keep the render rate
};

class CaptureCache;  // capture_cache.h

// Which channel of a multi-channel DI file feeds the chain (v0.8 I4a; docs/PRESET_SCHEMA.md "Offline stereo DI"). Offline there is
// no auto-detection over time: Auto = the louder of the first two channels by whole-file RMS (a tie picks the left channel), so
// one-sided stereo DIs work; Mix = the mean of channels 0 and 1. Channels beyond the second are dropped with a warning. A mono
// file is used as is (the rule is then "mono"). The choice is made on the whole file before any block runs, so it cannot depend on
// the block size.
enum class DiChannel { Auto, Left, Right, Mix };
const char* diChannelName(DiChannel c);  // "auto", "L", "R", "mix"

struct RenderOptions {
  int blockSize = 256;                      // processing block size, 1..65536
  // Render rate. none = auto: the rate the preset's NAM models were trained at (all non-bypassed
  // NAM blocks on enabled paths must agree, else RenderError Preset naming the blocks; a model that
  // records no rate counts as 48 kHz, kAssumedNamSampleRate, the NAM convention); a preset with no
  // NAM blocks renders at the input rate.
  std::optional<double> renderRate;
  OutRate outRate = OutRate::Input;
  std::optional<double> normalizePeakDbfs;  // scale the output so its peak equals this; none by default
  // Optional, non-owning (must outlive the call; thread-safe, may be shared by concurrent renders):
  // NAM models and IRs are taken from it instead of being re-read. Output is bit-identical.
  CaptureCache* cache = nullptr;
  // v0.3 level matching: apply the preset's `output.autoTrimDb` (after the output gain) as the plugin does with LEVEL MATCH on.
  // Off by default, so renders, goldens, the matcher and the NAM export never see it. The caller makes sure the trim is fresh
  // (ensureAutoTrim, auto_trim.h): a stale or missing one is applied as stored (0 when missing).
  bool applyAutoTrim = false;
  // v0.8 input calibration (chain.h ChainCalibration). Off by default: renders, goldens, the matcher and the NAM export never
  // see it until a caller opts in. This is the explicit setting (used as is unless calibrationFromPreset).
  ChainCalibration calibration;
  // v0.8 I4a (schema v5): follow the preset's `calibration.mode`: Calibrated -> calibration on, Legacy -> off (bit-identical to a
  // render without calibration). `calibration.defaults` is still used. tonerender and the Python binding set it.
  bool calibrationFromPreset = false;
  // The interface level (dBu at 0 dBFS) the calibration plans with; none = the assumed kAssumedDeviceDbu, which the report says.
  // Replaces calibration.device.dbu when set. Ignored while calibration is off (the report still records it).
  std::optional<double> deviceDbu;
  // Multi-channel input: see DiChannel. Ignored for a mono file.
  DiChannel diChannel = DiChannel::Auto;
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
  double blend = 0.5;          // the preset's blend (for the report)
  // Mono. Length N (the input length) when outputRate == inputRate, else round(N * out / in).
  // Advanced by info.latencySamples (at the render rate); resampling adds no delay.
  std::vector<float> samples;
  ChainInfo info;
  SignalStats input, output;   // output stats are after normalization
  double normalizeGainDb = 0.0;
  CalibrationPlan calibration;  // the plan in force (enabled false when off); written to the report
  std::string calibrationMode = "legacy";    // "calibrated" | "legacy": what the render ran with
  std::string calibrationSource = "options";  // "preset" (followed calibration.mode) | "options" (RenderOptions::calibration)
  // The DI-channel rule that was applied (report "diChannel").
  struct DiChannelInfo {
    int fileChannels = 1;
    std::string rule = "mono";  // requested: "mono" (single channel file) | "auto" | "L" | "R" | "mix"
    std::string used = "mono";  // what fed the chain: "mono" | "L" | "R" | "mix"
    double rmsDbfsL = 0.0, rmsDbfsR = 0.0;  // whole-file RMS of the first two channels (stereo only; -inf = silence)
  } diChannel;
  double autoTrimDb = 0.0;      // applied (RenderOptions::applyAutoTrim), else 0
  double prepareSeconds = 0.0;  // chain prepare() incl. NAM prewarm and the alignment probe
  double renderSeconds = 0.0;   // wall time of the process() loop only
  double resampleSeconds = 0.0;  // wall time of the input and output sample-rate conversions
  double realTimeFactor = 0.0;  // renderSeconds / (input duration); < 1 is faster than real time
  std::vector<std::string> warnings;  // chain + render warnings
  std::vector<CaptureAttribution> captures;  // every capture that has `source`
};

// Renders `in` (mono, or a multi-channel file reduced to one channel by RenderOptions::diChannel) through `preset`. If the render
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
