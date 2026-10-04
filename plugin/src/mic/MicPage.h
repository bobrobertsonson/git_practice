#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../PluginProcessor.h"
#include "MicSession.h"

namespace sawblade::plugin {

// The cab mic placement page (docs/specs/phase9a_mic_page.md section 5, layout from design/mockups/MicPage.dc.html):
// an overlay over the rig + inspector (1280 x 742 design px, below the top bar). Left, the open cab with a dot for every
// position the IR pack has a shot at and a draggable mic that snaps to the nearest dot on mouse-up; right, the IR PACK card,
// the SPEAKER / MIC / DISTANCE / POSITION fields, the selected IR with its magnitude response, and the A/B, NEXT POSITION
// and BLEND 2 MICS controls (a second mic and a MIX fader).
//
// Everything audio goes through the processor's normal preset loader (SawbladeProcessor::loadPreset): the page only builds
// presets (mic::MicSession) and hands them over. Nothing here runs on the audio thread, nothing is a host parameter.
class MicPage : public juce::Component, private juce::Timer {
 public:
  static constexpr int kWidth = 1280, kHeight = 742;
  static constexpr int kMixIntervalMs = 150;  // the MIX fader submits at most this often while dragging

  explicit MicPage(SawbladeProcessor& p);
  ~MicPage() override;

  std::function<void()> onClose;  // the "< RIG" button

  void paint(juce::Graphics&) override;
  void resized() override;

  // Called when the page is shown: adopts the processor's cab, finds the cached pack of the cab's tone, refreshes.
  void open();
  // Message thread, ~4 Hz while visible: follows the processor (a preset loaded from elsewhere, a finished load).
  void refresh();

  // --- introspection (tests, screenshots) ---
  mic::MicSession& session() { return session_; }
  const mic::MicSession& session() const { return session_; }
  int dotCount() const;
  juce::Point<float> dotCentre(int dot) const;      // page coordinates
  juce::Point<float> micCentre(int mic) const;      // page coordinates (the mic's snapped position)
  juce::Component& stage();                         // the cab image with its dots and mics (mouse target)
  juce::Slider& mixSlider();
  juce::Button* buttonTitled(const juce::String& title);
  juce::String cabImageName() const;                // "4x12" / "2x12"
  juce::String statusText() const;
  bool responseShown() const;

  // Loads a local folder of WAVs as the pack. False (and a message in the status line) if it has none.
  bool loadFolder(const std::filesystem::path& dir);
  // Runs `sawblade-t3k pack` for the cab's TONE3000 tone (a cached manifest is used as is).
  void loadPack();
  bool packLoading() const;

 private:
  struct Impl;
  void timerCallback() override;
  void submit(std::optional<Preset> p);  // loads the preset through the processor and marks it pending
  std::unique_ptr<Impl> impl_;
  SawbladeProcessor& processor_;
  mic::MicSession session_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MicPage)
};

}  // namespace sawblade::plugin
