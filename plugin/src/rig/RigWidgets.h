#pragma once

// Small widgets of the rig editor, all in the SawbladeLookAndFeel palette: a segmented button group, an
// LED-style toggle, and a preset-only knob (an unbound FilmstripKnob with caption and value text).

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "SawbladeLookAndFeel.h"
#include "rig/RigController.h"
#include "skin/FilmstripKnob.h"

namespace sawblade::plugin::rig {

// A row of mutually exclusive buttons (topology, tabs, EQ target, cab mode, ...).
class Segmented : public juce::Component {
 public:
  struct Item {
    juce::String text, title, tooltip;
    float weight = 1.0f;  // relative width
  };
  Segmented() = default;
  void setItems(const std::vector<Item>& items, juce::Colour onColour = juce::Colour(0xff6b2f12));
  int size() const noexcept { return static_cast<int>(buttons_.size()); }
  int selected() const noexcept { return selected_; }
  // Highlights item `i` (-1: none). With `notify` the onChange callback runs as for a click.
  void setSelected(int i, bool notify = false);
  juce::TextButton& button(int i) { return *buttons_[static_cast<std::size_t>(i)]; }
  void setItemEnabled(int i, bool on) { button(i).setEnabled(on); }
  std::function<void(int)> onChange;
  void resized() override;

 private:
  std::vector<std::unique_ptr<juce::TextButton>> buttons_;
  std::vector<float> weights_;
  int selected_ = -1;
};

// "● TEXT": a toggle that lights its LED when on.
class LedToggle : public juce::Button {
 public:
  LedToggle(const juce::String& text, const juce::String& tooltip, juce::Colour onColour = SawbladeLookAndFeel::saw());
  void paintButton(juce::Graphics&, bool over, bool down) override;

 private:
  juce::Colour on_;
};

// A knob for a preset-only value; it drags exactly like a main-page knob (it is the same FilmstripKnob). Structural knobs (the
// default) apply on a throttle while dragged (at most one rebuild per RigController::kDebounceMs) and once on mouse-up; wheel and typed
// values are debounced too. Live knobs call the controller's live edit on every change. The editor's refresh never writes into a
// knob that is being dragged or has an edit pending.
class PresetKnob : public juce::Component {
 public:
  using Apply = std::function<void(Preset&, double)>;
  using Format = std::function<juce::String(double)>;
  // The value the model keeps for a knob value (clamps, dead zones such as KEY HPF's 0..40 Hz); after a gesture ends the knob shows it.
  using Normalise = std::function<double(double)>;

  PresetKnob(RigController& c, const juce::String& caption, skin::FilmstripKnob::Kind kind, juce::Colour arc,
             const skin::FilmstripKnob::Range& range, Apply apply, bool live = false, Format format = {}, Normalise normalise = {});
  ~PresetKnob() override;

  skin::FilmstripKnob& knob() noexcept { return knob_; }
  const skin::FilmstripKnob& knob() const noexcept { return knob_; }
  // Shows the value the preset holds (no edit is submitted). Ignored while the user is turning the
  // knob or an edit of this knob is pending.
  void setValueFromPreset(double v);
  double value() const { return knob_.getValue(); }
  juce::String valueText() const { return value_.getText(); }
  void setCaption(const juce::String& s) { caption_.setText(s, juce::dontSendNotification); }

  // Gesture hooks (v0.3 Task A; Task D's one-undo-step-per-gesture builds on them): `onGestureBegin` runs when the
  // mouse goes down on the knob (or a wheel / double-click edit starts), `onGestureEnd` after the last edit of the
  // gesture has been handed to the controller.
  std::function<void()> onGestureBegin, onGestureEnd;

  void resized() override;

 private:
  void updateText();
  void submit(bool debounced);
  void finishGesture();
  void showNormalised();

  RigController& controller_;
  skin::FilmstripKnob knob_;
  juce::Label caption_, value_;
  Apply apply_;
  Format format_;
  Normalise normalise_;
  bool live_ = false, dragging_ = false, updating_ = false;
  double shown_ = 0.0;
};

}  // namespace sawblade::plugin::rig
