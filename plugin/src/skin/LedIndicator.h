#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sawblade::plugin::skin {

// The led_orange sprite (off frame; on frame when lit) with the glow drawn in code: a radial
// gradient (#ff6a1a, alpha ~0.6 -> 0) over ~3x the LED diameter. The component is 3x the LED, the
// sprite sits in the middle, so the glow can spill out of the sprite's own bounds.
class LedIndicator : public juce::Component {
 public:
  static constexpr float kGlowFactor = 3.0f;

  LedIndicator();
  void setOn(bool on);
  bool isOn() const noexcept { return on_; }
  // Component bounds for an LED sprite of `diameter` px centred at `centre` (parent coordinates).
  static juce::Rectangle<int> boundsFor(juce::Point<float> centre, float diameter);
  // The LED sprite's own rectangle, in this component's coordinates.
  juce::Rectangle<float> spriteBounds() const;
  void paint(juce::Graphics&) override;

 private:
  bool on_ = false;
};

}  // namespace sawblade::plugin::skin
