#pragma once

// One table row per pedal circuit drives both the pedal face (PedalFace) and the advanced drawer
// (AdvancedDrawer); a new circuit (7c: pedal.hmx, pedal.eye) is a new row (docs/specs/phase7b, 5.2).
// JUCE-free data. UI-visible strings are generic descriptors (no trademarks).

#include <array>
#include <vector>

#include "CircuitParams.h"

namespace sawblade::plugin {

struct FaceKnob {
  int param;          // ParamIndex, -1 = empty position
  const char* label;  // panel label (upper case)
};

// FOCUS: a two-position switch over a continuous parameter. `threshold` separates the two readings;
// the reading nearer `narrowValue` is NARROW (so it works whichever of the two values is larger).
struct FaceSwitchSpec {
  int param;
  const char* label;
  double wideValue, narrowValue, threshold;
  bool isNarrow(double v) const noexcept { return narrowValue > wideValue ? v > threshold : v < threshold; }
};

struct CircuitFace {
  const char* blockType;                  // "pedal.hm"
  const char* oledName;                   // "CHAINSAW"
  std::array<FaceKnob, 6> knobs;          // positions 1-6: three on the top row, three below
  int clipParam;                          // the CLIP switch (choice parameter)
  FaceSwitchSpec focus;                   // the FOCUS switch
  std::vector<FaceKnob> drawerKnobs;      // up to 10, two rows of five
  std::vector<FaceKnob> drawerSwitches;   // PedalSwitch rows (choice parameters)
};

const CircuitFace& circuitFace(Circuit c);

// Short readings of the clip choices for the OLED and the CLIP switch: SI, LED, ASYM, SOFT.
const char* clipShortName(int clipIndex);

}  // namespace sawblade::plugin
