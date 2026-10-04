#pragma once

#include <array>
#include <memory>
#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../PluginProcessor.h"
#include "../skin/FilmstripKnob.h"
#include "PedalSwitch.h"

namespace sawblade::plugin {

// The live controls of the STOCKHOLM SYNDROME pedal (docs/specs/phase7b_chainsaw_pedal.md, 5.3): a
// transparent component laid exactly over the SAW pedal render. It owns one set of controls per circuit
// row (6 knobs, a CLIP and a FOCUS switch), shows the active circuit's set and hides the others, plus
// the CIRCUIT switch, label chips over the baked captions and the OLED overlay. Shown only while the
// preset has a circuit block (the parameters control the first one); otherwise hidden and the baked
// render stays. Background clicks fall through to the pedal piece (single click selects, double-click
// opens the drawer).
class PedalFace : public juce::Component {
 public:
  // Pedal geometry (millimetres, +y up, origin at the centre of the render); the render spans
  // kPedalMmHeight over its image height (the same mapping RigView uses for the footswitches).
  static constexpr float kPedalMmHeight = 205.0f;
  static constexpr float kKnobMm = 34.0f;       // knob component size (sprite frame is larger, see FilmstripKnob)
  static constexpr float kSwitchMm = 11.0f;     // lever size
  static constexpr float kChipMmHeight = 5.5f;
  static juce::Point<float> knobMm(int position);    // 0..5
  static juce::Point<float> switchMm(int position);  // 0 = CIRCUIT, 1 = CLIP, 2 = FOCUS

  explicit PedalFace(SawbladeProcessor& p);
  ~PedalFace() override;

  // Re-reads the processor: visibility, the active circuit's set, the OLED text. Called by the
  // editor's tick (and by tests).
  void refresh();
  std::optional<Circuit> activeCircuit() const noexcept { return active_; }

  // OLED text: the preset name (upper case) and `<circuit> · <clip> · <focus>`, from the parameters (the clip
  // field is omitted for a circuit without a CLIP switch).
  juce::String oledLine1() const;
  juce::String oledLine2() const;

  // Accessors (tests): the controls of a circuit row.
  skin::FilmstripKnob* knob(Circuit c, int position) { return knobs_[static_cast<size_t>(c)][static_cast<size_t>(position)].get(); }  // null: an empty position
  PedalSwitch* clipSwitch(Circuit c) { return clip_[static_cast<size_t>(c)].get(); }  // null: the circuit has no CLIP switch
  PedalSwitch& focusSwitch(Circuit c) { return *focus_[static_cast<size_t>(c)]; }
  PedalSwitch& circuitSwitch() { return *circuit_; }

  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  juce::Point<float> toPx(juce::Point<float> mm) const;
  float pxPerMm() const;
  void showActive();

  SawbladeProcessor& proc_;
  std::array<std::array<std::unique_ptr<skin::FilmstripKnob>, 6>, kNumCircuits> knobs_;
  std::array<std::unique_ptr<PedalSwitch>, kNumCircuits> clip_, focus_;
  std::unique_ptr<PedalSwitch> circuit_;
  std::optional<Circuit> active_;
  juce::String presetName_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalFace)
};

}  // namespace sawblade::plugin
