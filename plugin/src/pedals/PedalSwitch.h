#pragma once

#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "CircuitFaces.h"

namespace sawblade::plugin {

// A small code-drawn lever switch bound to one parameter through juce::ParameterAttachment. A press
// cycles to the next position (wrapping), the mouse wheel steps (no wrap). Two flavours:
//  * choice parameter: one position per choice (the value is the choice index);
//  * FOCUS: two positions (WIDE / NARROW) over a continuous parameter: pressing writes the wide or the
//    narrow value; a value reads as NARROW when it is on the narrow side of the threshold.
// The value text (SI, WIDE, ...) is drawn above or below the lever; title and tooltip carry the control
// name and the current value text.
class PedalSwitch : public juce::Component, public juce::SettableTooltipClient {
 public:
  enum class TextSide { Below, Above };

  // `texts` empty: the choice names, upper case.
  PedalSwitch(juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& name,
              std::vector<juce::String> texts = {});
  PedalSwitch(juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& name, const FaceSwitchSpec& focus);
  ~PedalSwitch() override;

  const juce::String& paramId() const noexcept { return paramId_; }
  const juce::String& controlName() const noexcept { return name_; }
  int numPositions() const noexcept { return static_cast<int>(texts_.size()); }
  int position() const;
  juce::String valueText() const;            // the current position's text
  void setPosition(int index);               // writes the parameter (one complete gesture)
  void step(int direction, bool wrap);       // +1 / -1

  // Layout: height of the text strip, drawn above or below the square lever area; 0 = no text.
  void setValueText(TextSide side, int heightPx, float fontPx);
  // Dark label above the lever area of the drawer variant (e.g. "MODE"); empty for none.
  void setCaption(const juce::String& caption);

  void paint(juce::Graphics&) override;
  void mouseDown(const juce::MouseEvent&) override;
  void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

 private:
  void init(juce::AudioProcessorValueTreeState& apvts);
  void update();
  juce::Rectangle<float> leverBox() const;

  juce::String paramId_, name_;
  std::vector<juce::String> texts_;
  std::vector<double> values_;       // value written for each position
  bool focus_ = false;
  FaceSwitchSpec spec_{};
  double value_ = 0.0;
  TextSide side_ = TextSide::Below;
  int textH_ = 0;
  float fontPx_ = 8.0f;
  juce::String caption_;
  std::unique_ptr<juce::ParameterAttachment> attachment_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalSwitch)
};

}  // namespace sawblade::plugin
