#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "LedIndicator.h"

namespace sawblade::plugin::skin {

// Toggle button drawn from the footswitch sprite: frame 0 (up); while the mouse is down the same
// frame is drawn 2 px lower and 25% darker (the sprite's frame 1 shows the cap sunk inside the
// collar, which reads wrongly at this size, so the pressed look is made in code as specified).
// Toggling flips the paired LED. Prototype: a visual bypass only, no parameter behind it.
class FootswitchButton : public juce::Button {
 public:
  explicit FootswitchButton(const juce::String& name);
  void setPairedLed(LedIndicator* led);
  LedIndicator* pairedLed() const noexcept { return led_; }
  static constexpr int kPressOffsetPx = 2;

  void paintButton(juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
  void clicked() override;

 private:
  LedIndicator* led_ = nullptr;
  juce::Image dark_;
};

}  // namespace sawblade::plugin::skin
