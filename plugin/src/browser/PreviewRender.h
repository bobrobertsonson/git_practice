#pragma once

// Offline render of the preview riff through a (candidate) preset. Message-thread-free, off the audio thread:
// called on the preview worker. No JUCE GUI types.

#include <string>
#include <vector>

#include "sawblade/capture_cache.h"
#include "sawblade/preset.h"
#include "sawblade/wav_io.h"

namespace sawblade::plugin {

constexpr double kPreviewNormalizeDbfs = -3.0;  // preview peak level after the render

// The embedded 6 s riff (plugin/assets/preview_riff.wav), 48 kHz mono.
AudioFile embeddedPreviewRiff();
// Decodes a WAV held in memory (16/24/32-bit PCM or 32-bit float, first channel only). Throws std::runtime_error.
AudioFile decodeWavMemory(const void* data, std::size_t size);

// Renders `riff` (resampled to `hostRate` first) through `preset` and returns the mono result at `hostRate`,
// normalized to kPreviewNormalizeDbfs. On failure returns empty and sets `error`. Thread-safe given a
// thread-safe `cache` (may be null).
// `levelMatched` (v0.3, LEVEL MATCH on): the preview is NOT peak-normalized; it plays at the preset's own auto trim (applied after
// its output gain), so a candidate sits at the same loudness as the rig it would replace. Only a safety limit applies: a result
// whose peak would exceed -0.1 dBFS is scaled down as a whole.
std::vector<float> renderPreview(const Preset& preset, const AudioFile& riff, double hostRate, CaptureCache* cache, std::string& error,
                                 bool levelMatched = false);

}  // namespace sawblade::plugin
