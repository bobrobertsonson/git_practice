#pragma once

// The CAB page (v0.4 Task D): the cab has its own page, opened by the CAB button next to RIG in the top bar and by the cab chip on
// the main page. A full overlay below the top bar (1280 x 742 design px, like the mic page), in the editor's mutually exclusive
// overlay group. It holds the shared cab controls (CabControls: mode SHARED / PER PATH, CAB ON, the IR cards), a BROWSE IR button
// per IR target (the capture browser for the cab), the LIVE / STUDIO notice and MIC POSITIONS (the mic page, whose close comes back
// here). The cab has no level control (no field in CabPreset); nothing here adds DSP or schema.

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "rig/CabControls.h"
#include "rig/RigController.h"

namespace sawblade::plugin::rig {

class CabScreen : public juce::Component {
 public:
  static constexpr int kWidth = 1280, kHeight = 742;

  CabScreen(SawbladeProcessor& p, RigController& c);
  ~CabScreen() override;

  std::function<void()> onClose;          // the "< RIG" button
  std::function<void()> onBrowseIr;       // any BROWSE IR button
  std::function<void()> onMicPositions;   // MIC POSITIONS

  // Pulls the preset into the controls (the editor's tick while the page is visible; also when it opens).
  void refresh();

  CabControls& controls() noexcept { return controls_; }
  juce::Button& backButton() noexcept { return back_; }

  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  RigController& ctl_;
  juce::TextButton back_;
  juce::Label title_, hint_;
  CabControls controls_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CabScreen)
};

}  // namespace sawblade::plugin::rig
