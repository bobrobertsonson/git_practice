#include "LedIndicator.h"

#include "SkinAssets.h"

namespace sawblade::plugin::skin {

LedIndicator::LedIndicator() {
  setInterceptsMouseClicks(false, false);
  setTitle("Pedal LED");
}

void LedIndicator::setOn(bool on) {
  if (on_ == on) return;
  on_ = on;
  repaint();
}

juce::Rectangle<int> LedIndicator::boundsFor(juce::Point<float> centre, float diameter) {
  const float s = diameter * kGlowFactor;
  return juce::Rectangle<float>(0, 0, s, s).withCentre(centre).getSmallestIntegerContainer();
}

juce::Rectangle<float> LedIndicator::spriteBounds() const {
  const float d = static_cast<float>(getWidth()) / kGlowFactor;
  return juce::Rectangle<float>(0, 0, d, d).withCentre(getLocalBounds().toFloat().getCentre());
}

void LedIndicator::paint(juce::Graphics& g) {
  const auto sprite = spriteBounds();
  const auto c = sprite.getCentre();
  if (on_) {
    const float r = getWidth() * 0.5f;
    juce::ColourGradient glow(juce::Colour(0xffff6a1a).withAlpha(0.6f), c.x, c.y, juce::Colour(0xffff6a1a).withAlpha(0.0f), c.x + r, c.y, true);
    glow.addColour(0.35, juce::Colour(0xffff6a1a).withAlpha(0.3f));
    g.setGradientFill(glow);
    g.fillEllipse(c.x - r, c.y - r, r * 2, r * 2);
  }
  const Filmstrip& fs = SkinAssets::get().ledOrange();
  if (fs.valid()) {
    fs.drawFrame(g, on_ ? 1 : 0, sprite);
  } else {
    g.setColour(on_ ? juce::Colour(0xffff6a1a) : juce::Colour(0xff5a1c05));
    g.fillEllipse(sprite.reduced(sprite.getWidth() * 0.15f));
  }
}

}  // namespace sawblade::plugin::skin
