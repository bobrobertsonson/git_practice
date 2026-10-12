#include "rig/PedalKind.h"

#include "SawbladeLookAndFeel.h"

namespace sawblade::plugin::rig {
using L = SawbladeLookAndFeel;

juce::String badgeText(PedalKind k) { return k == PedalKind::Capture ? juce::String("CAPTURE") : juce::String(); }

void paintKindBadge(juce::Graphics& g, PedalKind k, juce::Rectangle<float> area) {
  if (k != PedalKind::Capture || area.isEmpty()) return;
  g.setColour(L::capture());
  g.fillRoundedRectangle(area, juce::jmin(4.0f, area.getHeight() * 0.3f));
  g.setColour(L::background());
  g.setFont(L::labelFont(juce::jmax(6.0f, area.getHeight() * 0.62f)));
  g.drawFittedText(badgeText(k), area.toNearestInt(), juce::Justification::centred, 1, 0.6f);
}

void paintKindOutline(juce::Graphics& g, PedalKind k, juce::Rectangle<float> r, float radius, juce::Colour modeledColour) {
  if (k == PedalKind::Capture) {
    g.setColour(L::capture());
    g.drawRoundedRectangle(r.reduced(1.0f), radius, 2.0f);
  } else {
    g.setColour(modeledColour);
    g.drawRoundedRectangle(r.reduced(0.5f), radius, 1.0f);
  }
}

}  // namespace sawblade::plugin::rig
