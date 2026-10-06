#pragma once

#include <optional>
#include <string>
#include <vector>

#include "sawblade/capture_cache.h"
#include "sawblade/preset.h"

// Level-matched auditioning (v0.3 Task B; docs/PRESET_SCHEMA.md "Level matching").
//
// Convention (one place, used by the plugin, tonerender, scripts/compute_trims and the tests):
//   * Reference signal: referenceDi() (reference_di.h), 48 kHz mono, rendered through the whole preset with renderPreset (so the
//     NAM models' rate, the cab, post EQ, bus comp and the output gain are in; the auto trim itself is not).
//   * Loudness: BS.1770-4 integrated loudness (loudness.h; K-weighting, 400 ms blocks, -70 LUFS absolute and -10 LU relative
//     gates) of the mono output, as the 10.1 level match in chain.cpp measures: the signal is the left channel, the right
//     channel is digital silence (weight 1.0 per channel, no mono-to-stereo up-mix).
//   * Target: kAutoTrimTargetLufs. autoTrimDb = target - measured, clamped to +-kMaxAutoTrimDb, applied as a plain gain after
//     the OUTPUT knob (Chain::setAutoTrimDb). So the preset as stored (its own output.gainDb included) plays at the target;
//     moving the OUTPUT knob afterwards moves the level relative to that.
//   * The NAM export, the matcher and the trained chain always use the UN-trimmed chain: the trim is not in LiveParams, not in
//     the chain's own gains, and renderPreset ignores it unless RenderOptions::applyAutoTrim is set.
namespace sawblade {

constexpr double kAutoTrimTargetLufs = -18.0;
// Version of the whole recipe (reference signal id, loudness measure, target, hash inputs): part of the staleness hash.
constexpr int kAutoTrimVersion = 1;
// Capture-swap make-up (a slot's NamBlockParams::makeupDb) is limited to +-this.
constexpr double kMaxSlotMakeupDb = 24.0;

// The captures the preset needs (NAM models of enabled paths, the cab IRs when the cab is enabled) that are not on this machine
// (locateCapture: the file next to the preset, or the TONE3000 capture cache). Each entry is "<json path>: <file>". Empty = it
// can be rendered here. Used by compute_trims to report "skipped: capture not cached".
std::vector<std::string> missingCaptures(const Preset& p);

// Hash (sha256 hex) of everything that affects the level of the preset on the reference DI: the preset as serialised, minus the
// name, notes, category, version / schema, the stored trim and hash, and, for captures, minus everything that is not identity
// (a TONE3000 capture is its provider + id + modelId, any other its file name + sha256; titles, urls, creators, licences,
// absolute paths do not count). Includes the output gain (the trim is relative to the output gain the preset was measured with)
// and the recipe version.
std::string autoTrimHash(const Preset& p);
// The preset carries a trim whose hash matches its current content.
bool autoTrimFresh(const Preset& p);

// Integrated loudness of the reference DI through `p` as it is (output gain included, auto trim not). nullopt: the render is
// silent or too quiet to measure. Throws RenderError (missing capture, bad preset). Not real-time safe; takes seconds.
// With `applyTrim` the preset's own autoTrimDb is applied (RenderOptions::applyAutoTrim): the "after" side of the loudness table.
std::optional<double> measureReferenceLufs(const Preset& p, CaptureCache* cache = nullptr, bool applyTrim = false);

struct AutoTrimResult {
  double trimDb = 0.0;
  double lufs = 0.0;     // measured without the trim
  std::string hash;      // autoTrimHash(p)
};
// target - measured, with the hash it is valid for. nullopt when the preset is silent.
std::optional<AutoTrimResult> computeAutoTrim(const Preset& p, CaptureCache* cache = nullptr);
// computeAutoTrim, written into p.autoTrimDb / p.autoTrimHash. False (p unchanged) when silent.
bool stampAutoTrim(Preset& p, CaptureCache* cache = nullptr);
// stampAutoTrim only when the preset's trim is missing or stale. True when p now has a fresh trim.
bool ensureAutoTrim(Preset& p, CaptureCache* cache = nullptr);

// --- capture-swap make-up -----------------------------------------------------------------------------------------------
// Loudness on the reference DI of path `path` (0 = a, 1 = b) alone: the other path is disabled and the blend is hard to this
// path (linear law, level match and alignment off), everything else - the path's blocks and EQ, the shared cab, post EQ, bus
// comp - as in `p`. nullopt when the path is disabled or silent.
std::optional<double> measurePathLufs(const Preset& p, int path, CaptureCache* cache = nullptr);

// The make-up, in dB, for block `blockIndex` of path `path` that keeps the path's loudness unchanged when the preset changes from
// `before` to `after` (the same preset with another capture in that slot, make-up 0): measurePathLufs(before) -
// measurePathLufs(after), clamped to +-kMaxSlotMakeupDb. nullopt when either side cannot be measured (the caller then keeps 0).
std::optional<double> slotMakeupDb(const Preset& before, const Preset& after, int path, CaptureCache* cache = nullptr);

// Copy of `p` with the nam block `blockIndex` of `path` carrying `makeupDb` (the block is left alone when it is not a nam block).
Preset withSlotMakeup(const Preset& p, int path, int blockIndex, double makeupDb);
// The make-up a nam block carries (0 for any other block or an index out of range).
double slotMakeupOf(const Preset& p, int path, int blockIndex);

}  // namespace sawblade
