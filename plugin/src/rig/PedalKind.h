#pragma once

// v0.4 Task B: modeled and captured pedals must look different at a glance. A capture is a `nam` block in a pedal slot (an amp capture is
// not a pedal and gets no badge). Every pedal widget (a board tile, a rig-editor slot card, a picker row) exposes its kind through
// HasPedalKind and draws the badge / outline with the one shared paint helper below.

#include <juce_gui_basics/juce_gui_basics.h>

namespace sawblade::plugin::rig {

enum class PedalKind { Modeled, Capture };

// Mixin of the widgets that stand for a pedal.
class HasPedalKind {
 public:
  virtual ~HasPedalKind() = default;
  virtual PedalKind pedalKind() const noexcept = 0;
};

// The badge text of a kind ("CAPTURE"; empty for a modeled pedal, which has none).
juce::String badgeText(PedalKind k);
// The one badge style (filled rounded label, background-coloured text) in `colour`. paintKindBadge is this with L::capture(); v0.8 I2
// uses it for the small "UNCAL" mark of a capture whose metadata has no input / output level.
void paintBadge(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour colour);
// The filled "CAPTURE" badge of a capture (nothing is drawn for a modeled pedal), in `area`.
void paintKindBadge(juce::Graphics& g, PedalKind k, juce::Rectangle<float> area);
// The outline of a pedal widget: a capture's is 2 px in the capture colour (L::capture()), a modeled pedal's is `modeledColour` at 1 px.
void paintKindOutline(juce::Graphics& g, PedalKind k, juce::Rectangle<float> r, float radius, juce::Colour modeledColour);

}  // namespace sawblade::plugin::rig
