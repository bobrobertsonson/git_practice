#pragma once

// JUCE-free edits on sawblade::Preset for the rig editor (docs/specs/phase10_rig_editor.md, section 4):
// topology, slots, EQ bands, cab, align, gate and bus comp. Pure functions on a Preset; every rule
// of the spec is tested in plugin/tests/test_rig_model.cpp. Nothing here knows a style or a pedal.

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sawblade/chain.h"
#include "sawblade/preset.h"

namespace sawblade::plugin::rig {

// --- 4.1 topology ----------------------------------------------------------------------------------
enum class Topology { Single, SinglePlusTwoPedals, Blend };

// A path's amp is its last block with slot "amp", else its last `nam` block (-1: none). Every other
// block is a pedal slot, whatever its type. The rule lives in core (sawblade::ampIndex, which the amp
// controls of v0.2 use too); this is the same function, not a second copy.
using sawblade::ampIndex;
bool isPedalSlot(const PathPreset& p, int index);
int activePedalSlots(const PathPreset& p);  // pedal-slot blocks that are not bypassed

// Blend iff path B is enabled; else SinglePlusTwoPedals iff path A has >= 2 non-bypassed pedal-slot
// blocks; else Single.
Topology topologyOf(const Preset& p);

// -> Blend: b.enabled = true; blend 0 becomes `blendIfRestored`; a preset at the level-match defaults
// (mode off, law linear) becomes levelMatch auto + constantLoudness (phase 10.1). -> Single / SinglePlusTwoPedals:
// b.enabled = false and blend = 0 (path B's blocks are kept). -> Single bypasses every pedal-slot
// block of A after the first active one; -> SinglePlusTwoPedals un-bypasses the first bypassed
// pedal-slot block of A when fewer than two are active. Nothing is ever deleted.
void setTopology(Preset& p, Topology to, double blendIfRestored);

// --- 4.2 slots -------------------------------------------------------------------------------------
// Inserts at `index` (clamped to [0, size]); false when the path already has kMaxBlocksPerPath blocks.
bool addBlock(PathPreset& p, int index, Block b);
void removeBlock(PathPreset& p, int index);
void moveBlock(PathPreset& p, int from, int to);  // `to` is the final index
void setBypass(PathPreset& p, int index, bool bypass);
constexpr double kBlockGainMinDb = -24.0, kBlockGainMaxDb = 24.0;
// `nam` blocks only (false otherwise); clamps to [-24, +24] dB. Replaces the block's params object.
bool setBlockInputGainDb(PathPreset& p, int index, double db);
// A block id that is unique within the preset: "a3" / "b2" (path 'a' or 'b').
std::string newBlockId(const Preset& p, char path);
// "amp" for the first `nam` block of a path without an amp, "fx" for `eq`, else "pedal".
std::string defaultSlotFor(const PathPreset& p, const std::string& type);
// Builds a block of any registered type by parsing `typeFields` (the type-specific members) through
// BlockRegistry::find(type)->parse. Throws PresetError (naming the problem, e.g. a missing field).
Block makeBlock(const std::string& type, const std::string& id, const std::string& slot,
                const std::filesystem::path& baseDir, const nlohmann::json& typeFields);
// --- v0.2 Task C: BLEND fills an empty path B with a suggested body path ----------------------------------
// The fallback body amp: tone 88689, "EVH 5150iii Ivory FULL Pack", the first high-gain amp of presets/CAPTURE_SHORTLIST.md.
// (No model id is fixed: a cached model of the tone is used, else `sawblade-t3k fetch` picks the tone's default model.)
inline constexpr const char* kFallbackBodyTone = "88689";
// The modeled boost in front of the body amp: pedal.ts, drive 0, tone 5, level 8 (slot "boost").
Block makeTsBoost(const Preset& p);
// A `nam` amp block (slot "amp") for path B holding `model`.
Block makeBodyAmp(const Preset& p, const Capture& model);
// Path B := [TS boost, amp?] when it has no blocks (else untouched); returns whether it filled it. Does not touch
// the topology, blend or level-match fields (setTopology does).
bool fillBodyPath(Preset& p, const std::optional<Capture>& amp);
// Path B's amp replaced by `model` (or added after the first block); the other blocks stay.
void setBodyAmp(Preset& p, const Capture& model);
// A TONE3000 capture of `toneId` in the capture cache (`modelId` empty: the first cached model by file name), as a Capture
// with an absolute path; nullopt if none is cached.
std::optional<Capture> cachedToneCapture(const std::string& toneId, const std::string& modelId = {});

// An `eq` block with one flat peak band at 1 kHz.
nlohmann::json flatEqFields();

std::string captureTitle(const Capture& c);   // source.title, else the file stem
std::string captureCredit(const Capture& c);  // "@creator · licence · VIA TONE3000" / "LOCAL FILE"
std::string blockTitle(const Block& b);
std::string blockCredit(const Block& b);

// --- 4.3 EQ ----------------------------------------------------------------------------------------
enum class EqTarget { PreA, EqA, PreB, EqB, Post };
std::vector<EqBand>& eqBands(Preset& p, EqTarget t);
const std::vector<EqBand>& eqBands(const Preset& p, EqTarget t);
constexpr double kEqFreqMin = 20.0, kEqFreqMax = 20000.0, kEqGainMax = 18.0, kEqQMin = 0.1, kEqQMax = 20.0;
bool hasGain(EqType t);
bool addBand(Preset& p, EqTarget t, EqBand b);  // false at 16 bands
void removeBand(Preset& p, EqTarget t, int index);
// Clamps freq to [20, 20000], gain to [-18, 18] (0 for pass filters), q to [0.1, 20].
void setBandLive(Preset& p, EqTarget t, int index, double freq, double gainDb, double q);
void setBandType(Preset& p, EqTarget t, int index, EqType type);
void setBandEnabled(Preset& p, EqTarget t, int index, bool enabled);

// --- 4.4 cab ---------------------------------------------------------------------------------------
enum class CabSlot { Shared, A, B };
void setCabMode(Preset& p, CabMode m);  // Shared -> PerPath copies `ir` to irA and irB; back: ir = irA
void setCabEnabled(Preset& p, bool on);
void setCabIr(Preset& p, CabSlot which, Capture c);

// --- 4.5 align -------------------------------------------------------------------------------------
constexpr int kAlignMaxSamples = 2400;
void setAlignMode(Preset& p, AlignMode m);
// In Auto (or Off) mode the mode first becomes Manual, seeded with `measured` (Auto) or zeros.
void nudgeAlign(Preset& p, int samples, const AlignResult& measured = {});
void setInvertB(Preset& p, bool invert, const AlignResult& measured = {});

// --- 4.6 gate / comp -------------------------------------------------------------------------------
enum class GateField { Threshold, Hysteresis, Attack, Hold, Release, Range, Ratio, KeyHpf };
struct Range {
  double lo, hi;
};
Range gateRange(GateField f);  // the schema's ranges (core/src/preset.cpp)
// Clamps to the range; KeyHpf is 0 (off) or in [40, 400] (values below 20 become 0, below 40 become 40).
void setGateField(Preset& p, GateField f, double v);
double gateField(const GateParams& g, GateField f);
void setGateEnabled(Preset& p, bool on);
void setGateMode(Preset& p, GateMode m);
void setGateReleaseCurve(Preset& p, GateReleaseCurve c);

enum class CompField { Threshold, Ratio, Knee, Attack, Release, Makeup };
Range compRange(CompField f);
void setCompField(Preset& p, CompField f, double v);
double compField(const BusCompParams& c, CompField f);
void setCompEnabled(Preset& p, bool on);

}  // namespace sawblade::plugin::rig
