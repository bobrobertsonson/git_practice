#pragma once

// JUCE-free host-parameter side of the modeled pedal circuits (docs/specs/phase7b_chainsaw_pedal.md,
// section 5.1; phase 7c adds MODDED SAW = pedal.hmx and ONE-KNOB SAW = pedal.eye, Part 2 of
// docs/specs/phase7c_chainsaw_family.md). Each circuit (CHAINSAW = pedal.hm, BIG FUZZ = pedal.muff,
// MODDED SAW = pedal.hmx, ONE-KNOB SAW = pedal.eye) has its own parameter set;
// the parameters control the FIRST circuit block of the preset (path a in block order, then path b)
// and are inert when the preset has none. `sawCircuit` is the CIRCUIT switch: the one parameter whose
// change rebuilds (the processor swaps the block type through loadPreset; see PluginProcessor).
// Every other circuit parameter is live: it reaches the running block through Chain::setBlockLiveParams.

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

#include "PresetMapping.h"

namespace sawblade::plugin {

enum class Circuit : int { Chainsaw = 0, BigFuzz = 1, ModdedSaw = 2, OneKnobSaw = 3 };
constexpr int kNumCircuits = 4;
constexpr int kMaxCircuitLive = std::max(std::max(static_cast<int>(kHmNumLive), static_cast<int>(kMuffNumLive)),
                                         std::max(static_cast<int>(kHmxNumLive), static_cast<int>(kEyeNumLive)));

struct CircuitInfo {
  const char* blockType;   // registry type
  const char* choiceName;  // text of the `sawCircuit` choice (UI-visible: generic descriptor)
  int firstParam;          // ParamIndex of the first live parameter of this circuit
  int numParams;           // number of live parameters (the block type's liveParams size)
};
const CircuitInfo& circuitInfo(Circuit c);
std::optional<Circuit> circuitForBlockType(std::string_view blockType);

struct CircuitSlot {
  int path;   // 0 = a, 1 = b
  int block;  // index in the path's blocks
  Circuit circuit;
  bool operator==(const CircuitSlot&) const = default;
};

// The first circuit block of the preset, if any.
std::optional<CircuitSlot> findCircuitBlock(const Preset& p);

// Spec of one parameter in [kSawCircuit, kNumParams): id, display name, unit, range, default, choices.
ParamSpec circuitParamSpec(int index);

// Sets sawCircuit from the block type and the active set from the block (defaults everywhere else,
// including the other circuit's set and everything when the preset has no circuit block).
void circuitParamsFromPreset(const Preset& p, ParamValues& v);
// Writes the values of the block's own circuit set into the block. Never changes the block type.
void applyCircuitParams(Preset& p, const ParamValues& v);

// The preset with its first circuit block replaced by `target`'s block: level <-> volume, mix,
// tightness and clip are carried over where the target has them (the one-knob circuit has no mix and
// no clip: dropped), everything else is at the new block's defaults. Returns the
// preset unchanged if it has no circuit block or the block is already `target`.
Preset switchCircuit(const Preset& p, Circuit target);

// The live-parameter vector (core order: HmLive / MuffLive) of a circuit from the parameter values,
// or from a block's own params. Both return the count; RT-safe (no allocation).
int circuitLiveValues(Circuit c, const ParamValues& v, float* out) noexcept;
int blockLiveValues(const Block& b, float* out) noexcept;

// Field access of the block parameters by live index (double precision, no float round trip).
double hmField(const HmParams& p, int liveIndex);
void setHmField(HmParams& p, int liveIndex, double v);
double muffField(const MuffParams& p, int liveIndex);
void setMuffField(MuffParams& p, int liveIndex, double v);
double hmxField(const HmxParams& p, int liveIndex);
void setHmxField(HmxParams& p, int liveIndex, double v);
double eyeField(const EyeParams& p, int liveIndex);
void setEyeField(EyeParams& p, int liveIndex, double v);

}  // namespace sawblade::plugin
