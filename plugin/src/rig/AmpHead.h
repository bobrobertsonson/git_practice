#pragma once

// v0.2 Task D: the six amp controls of one path on its amp head of the rig page. The head art (RigView, amp_saw / amp_body)
// has six knob positions baked in; this component lays the existing `knob_amp` filmstrip knobs (bound to ampA_* / ampB_*) over
// them, in the art's order GAIN, BASS, MID, TREBLE, LEVEL, PRESENCE, with code-drawn captions, and one read-out line:
//   GAIN 7.0 . capture: Gain 6              the path's amp capture has a gain ladder
//   GAIN 7.0 . drive only (fetching Gain 8) a rung is pending: its model is not loaded yet
//   GAIN 7.0                                no ladder
//   NO AMP IN THIS PATH                     no amp block: the knobs are disabled
//   BODY PATH OFF - turn up BLEND to add one   (body head, path B off): the knobs are disabled
// No new art: the filmstrip is drawn at kKnob design px (smaller than the pedal knobs), never redrawn.

#include <array>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "skin/FilmstripKnob.h"

namespace sawblade::plugin::rig {

class AmpHead : public juce::Component {
 public:
  static constexpr int kKnob = 30;  // design px, the filmstrip's draw size
  // path: 0 = SAW head (ampA_*), 1 = BODY head (ampB_*).
  AmpHead(SawbladeProcessor& p, int path);
  ~AmpHead() override;

  int path() const noexcept { return path_; }
  // The knob of amp control `k` (an AmpKnob: kAmpGain ... kAmpLevel).
  skin::FilmstripKnob& knob(int k) { return *knobs_[static_cast<std::size_t>(k)]; }
  static const char* caption(int k);  // "GAIN" "BASS" "MID" "TREBLE" "PRESENCE" "LEVEL"

  // The state for `preset` (the rig as shown) and `ladder` (what the processor reports for this path). The no-argument
  // form takes both from the processor.
  void refresh(const Preset& preset, const SawbladeProcessor::LadderInfo& ladder);
  void refresh();

  bool knobsEnabled() const noexcept { return enabled_; }
  const juce::String& readout() const noexcept { return readout_; }
  // The one line for a path with a working amp: GAIN `gain` with the ladder's state.
  static juce::String gainReadout(double gain, const SawbladeProcessor::LadderInfo& ladder);
  static juce::String noAmpText();    // "NO AMP IN THIS PATH"
  static juce::String bodyOffText();  // "BODY PATH OFF - turn up BLEND to add one" (with an em dash)

  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  SawbladeProcessor& proc_;
  int path_;
  std::array<std::unique_ptr<skin::FilmstripKnob>, kAmpKnobCount> knobs_;
  juce::String readout_;
  bool enabled_ = true, reason_ = false;
};

}  // namespace sawblade::plugin::rig
