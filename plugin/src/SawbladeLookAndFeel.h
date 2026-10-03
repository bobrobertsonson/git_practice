#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sawblade::plugin {

// The ONLY place that decides how the plugin looks: colours, fonts, metrics and any custom
// drawing. SawbladeEditor lays components out using the metrics below and never hard-codes a
// colour or a font, so the hardware-style design that replaces this generic one is a swap of this
// class (a subclass of juce::LookAndFeel_V4, or a new LookAndFeel) and a rewrite of the editor's
// layout, with no change anywhere else.
class SawbladeLookAndFeel : public juce::LookAndFeel_V4 {
 public:
  SawbladeLookAndFeel() {
    setColour(juce::ResizableWindow::backgroundColourId, background());
    setColour(juce::Label::textColourId, text());
    setColour(juce::Slider::thumbColourId, accent());
    setColour(juce::Slider::trackColourId, accent().withAlpha(0.6f));
    setColour(juce::Slider::backgroundColourId, juce::Colour(0xff3a3d42));
    setColour(juce::Slider::textBoxTextColourId, text());
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0xff55595f));
    setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a3d42));
    setColour(juce::TextButton::textColourOffId, text());
  }

  // Palette
  static juce::Colour background() { return juce::Colour(0xff24262a); }
  static juce::Colour text() { return juce::Colour(0xffe6e6e6); }
  static juce::Colour dimText() { return juce::Colour(0xff9a9ea5); }
  static juce::Colour accent() { return juce::Colour(0xffd9772b); }
  static juce::Colour warning() { return juce::Colour(0xffe0b341); }
  static juce::Colour error() { return juce::Colour(0xffe05a4f); }

  // Fonts
  static juce::FontOptions titleFont() { return juce::FontOptions(20.0f, juce::Font::bold); }
  static juce::FontOptions bodyFont() { return juce::FontOptions(14.0f); }
  static juce::FontOptions smallFont() { return juce::FontOptions(12.0f); }

  // Layout metrics (pixels)
  static constexpr int windowWidth = 560;
  static constexpr int margin = 12;
  static constexpr int rowHeight = 26;
  static constexpr int labelWidth = 150;
};

}  // namespace sawblade::plugin
