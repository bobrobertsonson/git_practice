#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../PluginProcessor.h"
#include "../skin/FilmstripKnob.h"
#include "PedalSwitch.h"

namespace sawblade::plugin {

// The ADVANCED drawer of the pedal (docs/specs/phase7b_chainsaw_pedal.md, 5.4): the deep controls of the
// active circuit (up to two rows of five knobs, then a row of switches). It slides out from behind the
// pedal to the right, over the pedalboard (ComponentAnimator, 180 ms), aligned with the pedal's vertical
// span. UI state only: never saved, no parameter. Like the face it owns one control set per circuit and
// shows the active one.
class AdvancedDrawer : public juce::Component, private juce::ChangeListener {
 public:
  static constexpr int kAnimationMs = 180;
  static constexpr int kGap = 12;        // between the pedal and the drawer
  static constexpr int kKnobPx = 64;

  explicit AdvancedDrawer(SawbladeProcessor& p);
  ~AdvancedDrawer() override;

  // Where the drawer lives (parent coordinates): the pedal's bounds and the area it may extend over
  // (its right edge is the limit).
  void setAnchor(juce::Rectangle<int> pedal, juce::Rectangle<int> limit);
  juce::Rectangle<int> openBounds() const noexcept { return open_; }

  bool isOpen() const noexcept { return wantOpen_; }
  void setOpen(bool open, bool animate = true);
  void toggle() { setOpen(!wantOpen_); }
  void finishAnimation();   // jumps to the final state of the running animation (tests, resizes)

  // Re-reads the processor: the active circuit's set, value read-outs.
  void refresh();
  std::optional<Circuit> activeCircuit() const noexcept { return active_; }
  juce::String title() const;

  void paint(juce::Graphics&) override;
  void resized() override;
  bool keyPressed(const juce::KeyPress&) override;

 private:
  struct Cell {
    std::unique_ptr<skin::FilmstripKnob> knob;
    std::unique_ptr<juce::Label> name, read;
  };
  void changeListenerCallback(juce::ChangeBroadcaster*) override;
  void showActive();
  void updateReadouts();

  SawbladeProcessor& proc_;
  juce::ComponentAnimator animator_;
  juce::Rectangle<int> open_;
  bool wantOpen_ = false;
  std::optional<Circuit> active_;
  std::array<std::vector<Cell>, kNumCircuits> cells_;
  std::array<std::vector<std::unique_ptr<PedalSwitch>>, kNumCircuits> switches_;
  juce::TextButton close_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AdvancedDrawer)
};

}  // namespace sawblade::plugin
