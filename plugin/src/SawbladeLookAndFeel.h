#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sawblade::plugin {

// The ONLY place that decides how the plugin looks: colours, fonts, metrics and the drawing of
// plain widgets. The skinned prototype editor (docs/specs/phase2_5_skin.md) takes its palette from
// design/mockups/RigReal.dc.html; the sprite-based controls live in plugin/src/skin/. Replacing the
// look means replacing this class and the skin classes, nothing else.
class SawbladeLookAndFeel : public juce::LookAndFeel_V4 {
 public:
  SawbladeLookAndFeel() {
    setColour(juce::ResizableWindow::backgroundColourId, background());
    setColour(juce::Label::textColourId, text());
    setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1b1916));
    setColour(juce::TextButton::textColourOffId, text());
    setColour(juce::TextButton::textColourOnId, text());
    setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff6b2f12));
    setColour(juce::TooltipWindow::backgroundColourId, panel());
    setColour(juce::TooltipWindow::textColourId, text());
    setColour(juce::TooltipWindow::outlineColourId, rule());
  }

  // Palette (RigReal)
  static juce::Colour background() { return juce::Colour(0xff0b0a09); }
  static juce::Colour panel() { return juce::Colour(0xff121110); }
  static juce::Colour panelDeep() { return juce::Colour(0xff1a1714); }
  static juce::Colour rule() { return juce::Colour(0xff2a2622); }
  static juce::Colour chipBorder() { return juce::Colour(0xff3a352d); }
  static juce::Colour saw() { return juce::Colour(0xffff6a1a); }
  static juce::Colour sawText() { return juce::Colour(0xffff8a3d); }
  static juce::Colour body() { return juce::Colour(0xff4f8fd0); }
  static juce::Colour bodyText() { return juce::Colour(0xff8fb8e6); }
  static juce::Colour text() { return juce::Colour(0xffe8e1d2); }
  static juce::Colour dimText() { return juce::Colour(0xffa39a8a); }
  static juce::Colour placeholderText() { return juce::Colour(0xff8a8273); }
  static juce::Colour live() { return juce::Colour(0xff7fe08f); }
  static juce::Colour liveBorder() { return juce::Colour(0xff2f5a37); }
  static juce::Colour studio() { return juce::Colour(0xffe0b341); }
  static juce::Colour warning() { return juce::Colour(0xffe0b341); }
  static juce::Colour error() { return juce::Colour(0xffe05a4f); }

  // Fonts (JUCE defaults; no font embedding)
  static juce::Font wordmarkFont() { return juce::Font(juce::FontOptions(30.0f, juce::Font::bold)); }
  static juce::Font titleFont(float h = 18.0f) { return juce::Font(juce::FontOptions(h, juce::Font::bold)); }
  static juce::Font labelFont(float h = 12.0f) { return juce::Font(juce::FontOptions(h, juce::Font::bold)); }
  static juce::Font monoFont(float h = 13.0f) { return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), h, juce::Font::plain)); }
  static juce::Font bodyFont(float h = 14.0f) { return juce::Font(juce::FontOptions(h)); }

  // Plain buttons: dark plate, thin border, rounded 4 px, dimmed when disabled.
  void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down) override {
    auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    auto fill = bg;
    if (down) fill = fill.brighter(0.15f);
    else if (over) fill = fill.brighter(0.06f);
    const float a = b.isEnabled() ? 1.0f : 0.5f;
    g.setColour(fill.withMultipliedAlpha(a));
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour((bg == juce::Colour(0xff1b1916) ? chipBorder() : bg.brighter(0.35f)).withMultipliedAlpha(a));
    g.drawRoundedRectangle(r, 4.0f, 1.0f);
  }
  juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override { return labelFont(juce::jmin(14.0f, 0.42f * static_cast<float>(buttonHeight))); }
  void drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool) override {
    g.setFont(getTextButtonFont(b, b.getHeight()));
    g.setColour(b.findColour(b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                    .withMultipliedAlpha(b.isEnabled() ? 1.0f : 0.5f));
    g.drawText(b.getButtonText(), b.getLocalBounds().reduced(6, 0), juce::Justification::centred, true);
  }
};

}  // namespace sawblade::plugin
