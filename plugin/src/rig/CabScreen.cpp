#include "rig/CabScreen.h"

#include "SawbladeLookAndFeel.h"
#include "skin/SkinAssets.h"

namespace sawblade::plugin::rig {
namespace {
using L = SawbladeLookAndFeel;
constexpr int kHeaderH = 46;
constexpr int kArtW = 300;  // the cab render on the right (existing art, no new asset)
}  // namespace

CabScreen::CabScreen(SawbladeProcessor&, RigController& c) : ctl_(c), controls_(c, /*withActions=*/true) {
  setTitle("Cab page");
  setOpaque(true);
  back_.setButtonText(juce::String::fromUTF8("\xe2\x80\xb9 RIG"));
  back_.setTitle("Close the cab page");
  back_.setTooltip("Back to the rig");
  back_.onClick = [this] {
    if (onClose) onClose();
  };
  addAndMakeVisible(back_);
  title_.setText("CAB", juce::dontSendNotification);
  title_.setFont(L::titleFont(20.0f));
  title_.setColour(juce::Label::textColourId, L::text());
  title_.setInterceptsMouseClicks(false, false);
  addAndMakeVisible(title_);
  hint_.setText("The cab's impulse response(s) after the blend (SHARED) or one per path before it (PER PATH).", juce::dontSendNotification);
  hint_.setFont(L::bodyFont(12.0f));
  hint_.setColour(juce::Label::textColourId, L::dimText());
  hint_.setMinimumHorizontalScale(0.8f);
  hint_.setInterceptsMouseClicks(false, false);
  addAndMakeVisible(hint_);
  controls_.onBrowseIr = [this] {
    if (onBrowseIr) onBrowseIr();
  };
  controls_.onMicPositions = [this] {
    if (onMicPositions) onMicPositions();
  };
  addAndMakeVisible(controls_);
  setSize(kWidth, kHeight);
  refresh();
}

CabScreen::~CabScreen() = default;

void CabScreen::refresh() { controls_.refresh(ctl_.view()); }

void CabScreen::paint(juce::Graphics& g) {
  g.fillAll(L::background());
  g.setColour(L::panel());
  g.fillRect(0, 0, kWidth, kHeaderH);
  g.setColour(L::rule());
  g.drawHorizontalLine(kHeaderH, 0.0f, static_cast<float>(kWidth));
  const juce::Image& img = skin::SkinAssets::get().panel(skin::Panel::Cab4x12);
  if (img.isValid()) {
    const auto dest = juce::Rectangle<float>(static_cast<float>(kWidth - 16 - kArtW), static_cast<float>(kHeaderH + 24), static_cast<float>(kArtW),
                                             static_cast<float>(kArtW));
    juce::DropShadow(juce::Colours::black.withAlpha(0.75f), 18, {0, 14}).drawForRectangle(g, dest.toNearestInt().reduced(2));
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    g.drawImage(img, dest);
  }
}

void CabScreen::resized() {
  back_.setBounds(16, 6, 90, 34);
  title_.setBounds(122, 8, 120, 30);
  hint_.setBounds(250, 8, kWidth - 250 - 16, 30);
  controls_.setBounds(0, kHeaderH + 16, kWidth - kArtW - 32, kHeight - kHeaderH - 16);
}

}  // namespace sawblade::plugin::rig
