#pragma once

// JUCE-free mapping between the host-visible parameters and the preset (docs/PRESET_SCHEMA.md).
// The preset is the single source of truth for everything discrete (models, IRs, EQ types and
// frequencies, gate timing, bus comp, alignment, ...); only the continuous controls below are
// parameters. Loading a preset writes its values into the parameters; the saved state is the
// preset with the current parameter values written back into it.

#include <array>
#include <string>

#include <vector>

#include "sawblade/pedal_params.h"
#include "sawblade/pedal_saw_params.h"
#include "sawblade/preset.h"

namespace sawblade::plugin {

// Post-EQ gain parameters are slots, not fixed bands: slot k controls the k-th gain-bearing band
// (peak / low shelf / high shelf, in preset order) of the preset's postEq. High-pass / low-pass
// bands have no gain and are skipped. A slot with no band does nothing.
constexpr int kPostEqSlots = 6;

// v0.2 amp controls: six knobs per path (ids ampA_gain ... ampB_level), 0..10, default 5.
enum AmpKnob : int { kAmpGain = 0, kAmpBass, kAmpMid, kAmpTreble, kAmpPresence, kAmpLevel, kAmpKnobCount };

enum ParamIndex : int {
  kInputGain = 0,
  kOutputGain,
  kGateThreshold,
  kBlend,
  kLevelA,
  kLevelB,
  kAmpFirst,  // path A knobs, then path B knobs (kAmpKnobCount each): see ampParam()
  kPostEqFirst = kAmpFirst + 2 * kAmpKnobCount,
  // Pedal circuits (pedals/CircuitParams.h): the CIRCUIT switch, then one live set per circuit.
  kSawCircuit = kPostEqFirst + kPostEqSlots,
  kHmFirst,
  kMuffFirst = kHmFirst + kHmNumLive,
  kHmxFirst = kMuffFirst + kMuffNumLive,
  kEyeFirst = kHmxFirst + kHmxNumLive,
  kNumParams = kEyeFirst + kEyeNumLive
};

// Parameter index of knob `k` of path `path` (0 = a, 1 = b).
constexpr int ampParam(int path, int k) noexcept { return kAmpFirst + path * kAmpKnobCount + k; }

struct ParamSpec {
  std::string id;    // stable host-facing identifier (never rename once released)
  std::string name;  // display name
  std::string unit;
  double min, max, def;
  std::vector<std::string> choices = {};  // empty = continuous; else the value is a choice index (min 0, max n-1)
};

const ParamSpec& paramSpec(int index);

using ParamValues = std::array<double, kNumParams>;
using SlotBands = std::array<int, kPostEqSlots>;  // slot -> postEq band index, -1 = no band

// Which postEq band each slot controls.
SlotBands postEqSlotBands(const Preset& p);

// The parameter values a preset implies, clamped to the parameter ranges. Slots without a band
// are 0.
// The one rule that keeps the audio thread's parameter reads and the engine baseline identical:
// every parameter value is clamped to its range and snapped to the 1e-4 grid of the saved state.
// The host parameter stores a float (and, on FMA targets, convertFrom0to1 may be off by an ulp),
// but both are far below the grid step, so round4(float param) == snapParam(value) exactly.
double snapParam(double v) noexcept;
ParamValues paramsFromPreset(const Preset& p);

// Writes parameter values into a preset (inverse of paramsFromPreset for the mapped fields).
void applyParams(Preset& p, const ParamValues& v);

// The placeholder file name of a capture slot that holds no capture (the schema requires a `file` even for a
// disabled cab). It is not a file: nothing may display it, read it or write it out as a path.
inline constexpr const char* kNoCaptureFile = "(none)";
// True for a capture slot with no capture: an empty file or the placeholder.
inline bool isNoCapture(const Capture& c) { return c.file.empty() || c.file == kNoCaptureFile; }

// "Init": no blocks on either path, cab disabled. Passes audio through (50/50 blend of two
// identical paths), zero latency, no files needed.
Preset makeInitPreset();

// The preset as the engine and the parameters will see it: continuous controls clamped to the
// parameter ranges, so the engine's baseline equals the parameter values after a load.
Preset clampedToParams(Preset p);

// State serialisation: the preset as JSON with capture paths made absolute (so a state restored
// by a host in another working directory still finds its files).
std::string presetToStateJson(const Preset& p);

}  // namespace sawblade::plugin
