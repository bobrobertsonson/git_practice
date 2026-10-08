#pragma once

// v0.2 Task D: the six amp controls of one path on its amp head of the rig page. The head art (RigView, amp_saw / amp_body)
// has six knob positions baked in; this component lays the existing `knob_amp` filmstrip knobs (bound to ampA_* / ampB_*) over
// them, in the art's order GAIN, BASS, MID, TREBLE, LEVEL, PRESENCE, with code-drawn captions, and one read-out line:
//   GAIN 7.0 . capture: Gain 6              the path's amp capture has a gain ladder
//   GAIN 7.0 . drive only (fetching Gain 8) a rung is pending: its model is not loaded yet
//   GAIN 7.0                                no ladder
// v0.3 Task E: a small text tag at the right end of the read-out pill: "STEPS n" when the capture has a gain ladder of n steps, "STEPS -"
// (an em dash) when the capture was checked and has none, nothing while that is not known (and for the states below that disable the knobs).
//   NO AMP IN THIS PATH                     no amp block: the knobs are disabled
//   BODY PATH OFF - turn up BLEND to add one   (body head, path B empty and off): the knobs are disabled
//   BODY PATH OFF - turn up BLEND              (body head, path B off but it has blocks): the knobs are disabled
//   BODY AMP DOWNLOADING... (name)             (body head, path B on with no amp yet, the BLEND fill is fetching one)
//   <reason> - <the one action>                (body head, the fill failed: not logged in / network / no capture / tool missing ...)
// No new art: the filmstrip is drawn at kKnob design px (smaller than the pedal knobs), never redrawn.

#include <array>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "rig/BodyFill.h"
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
  // `fill`: the BLEND fill's state (BodyFill::status()); only the body head with no amp in path B reads it.
  void refresh(const Preset& preset, const SawbladeProcessor::LadderInfo& ladder, const FillStatus& fill = {});
  void refresh();

  bool knobsEnabled() const noexcept { return enabled_; }
  const juce::String& readout() const noexcept { return readout_; }
  // The steps tag now shown ("" = none): see stepsText().
  const juce::String& stepsTag() const noexcept { return stepsTag_; }
  // v0.8 I2: the path's amp capture has no input / output level metadata and calibrated input levels are on ("UNCAL" badge in the pill).
  bool uncalibrated() const noexcept { return uncal_; }
  // steps: n >= 2 -> "STEPS n"; 0 -> "STEPS -" (em dash); anything else (unknown) -> "".
  static juce::String stepsText(int steps);
  // The one line for a path with a working amp: GAIN `gain` with the ladder's state.
  static juce::String gainReadout(double gain, const SawbladeProcessor::LadderInfo& ladder);
  static juce::String noAmpText();    // "NO AMP IN THIS PATH"
  static juce::String bodyOffText();  // path B empty, BLEND off: "BODY PATH OFF - turn up BLEND to add one" (with an em dash)
  // The body head while the BLEND fill fetches the amp: "BODY AMP DOWNLOADING... (name)" (an ellipsis character).
  static juce::String bodyDownloadingText(const juce::String& name);
  // The body head when the fill failed: the reason and the one action that fixes it (see FillReason).
  static juce::String bodyFailedText(FillReason reason);
  // The body head with path B on, no amp, and no fill running (the window was closed during the download, or the amp was dropped
  // because path B was edited): never the plain "NO AMP"; touching BLEND starts the fill again.
  static juce::String bodyMissingText();
  static juce::String bodyOffWithBlocksText();  // path B has blocks, BLEND off: "BODY PATH OFF - turn up BLEND"

  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  SawbladeProcessor& proc_;
  int path_;
  std::array<std::unique_ptr<skin::FilmstripKnob>, kAmpKnobCount> knobs_;
  juce::String readout_, stepsTag_;
  bool enabled_ = true, reason_ = false, uncal_ = false;
};

}  // namespace sawblade::plugin::rig
