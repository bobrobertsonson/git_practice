#include "FootswitchButton.h"

#include "SkinAssets.h"

namespace sawblade::plugin::skin {

FootswitchButton::FootswitchButton(const juce::String& name) : juce::Button(name) {
  setClickingTogglesState(true);
  setToggleState(true, juce::dontSendNotification);  // engaged
  setTitle(name);
  setTooltip(name + " (prototype: visual bypass only, no DSP behind it)");
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void FootswitchButton::setPairedLed(LedIndicator* led) {
  led_ = led;
  if (led_ != nullptr) led_->setOn(getToggleState());
}

void FootswitchButton::clicked() {
  if (led_ != nullptr) led_->setOn(getToggleState());
}

void FootswitchButton::paintButton(juce::Graphics& g, bool, bool isButtonDown) {
  const Filmstrip& fs = SkinAssets::get().footswitch();
  auto dest = getLocalBounds().toFloat().withTrimmedBottom(static_cast<float>(kPressOffsetPx));
  if (isButtonDown) dest = dest.translated(0.0f, static_cast<float>(kPressOffsetPx));

  if (!fs.valid()) {  // code-drawn fallback
    g.setColour(isButtonDown ? juce::Colour(0xff555555) : juce::Colour(0xff9a9a9a));
    g.fillEllipse(dest.reduced(2.0f));
    return;
  }
  if (!isButtonDown) {
    fs.drawFrame(g, 0, dest);
    return;
  }
  if (!dark_.isValid()) {  // frame 0 with RGB x0.75, alpha kept: darkened inside the sprite alpha only
    dark_ = fs.frameImage(0);
    juce::Image::BitmapData bd(dark_, juce::Image::BitmapData::readWrite);
    for (int y = 0; y < bd.height; ++y)
      for (int x = 0; x < bd.width; ++x) {
        auto* p = reinterpret_cast<juce::PixelARGB*>(bd.getPixelPointer(x, y));  // premultiplied
        p->setARGB(p->getAlpha(), static_cast<juce::uint8>(p->getRed() * 3 / 4), static_cast<juce::uint8>(p->getGreen() * 3 / 4),
                   static_cast<juce::uint8>(p->getBlue() * 3 / 4));
      }
  }
  g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
  g.drawImage(dark_, dest.getX(), dest.getY(), dest.getWidth(), dest.getHeight(), 0, 0, dark_.getWidth(), dark_.getHeight());
}

}  // namespace sawblade::plugin::skin
