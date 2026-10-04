#pragma once

#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "SkinAssets.h"

namespace sawblade::plugin::skin {

// A rotary slider drawn from a filmstrip. Bound to one APVTS parameter through a SliderAttachment.
// Vertical drag (full range over 250 px, shift = x0.1), double-click resets to the parameter
// default, the mouse wheel works. Draws a code-made drop shadow and value arc around the sprite.
class FilmstripKnob : public juce::Slider {
 public:
  enum class Kind { Amp, Pedal };

  FilmstripKnob(juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& displayName,
                Kind kind, juce::Colour arcColour);
  ~FilmstripKnob() override;

  // Frame shown for a normalised value: round(v * (frames - 1)), v clamped to 0..1.
  static int frameForProportion(double v, int frames);

  const juce::String& paramId() const noexcept { return paramId_; }
  double proportion() { return valueToProportionOfLength(getValue()); }
  int currentFrame();

  void paint(juce::Graphics&) override;
  void mouseDown(const juce::MouseEvent&) override;
  void mouseDrag(const juce::MouseEvent&) override;
  juce::String getTextFromValue(double v) override;

  static constexpr int kPixelsForFullRange = 250;
  static constexpr double kFineFactor = 0.1;

 private:
  const Filmstrip& strip() const;

  juce::AudioProcessorValueTreeState& apvts_;
  juce::String paramId_;
  Kind kind_;
  juce::Colour arc_;
  float lastY_ = 0.0f;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment_;
};

}  // namespace sawblade::plugin::skin
