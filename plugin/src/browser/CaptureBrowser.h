#pragma once

// The capture browser: a full-editor overlay (like PlayAlongPanel) listing TONE3000 captures for the selected
// rig piece, with PREVIEW and USE. Layout reference: design/mockups/FullBrowse.dc.html. All logic lives in
// BrowserController; this class only shows its state. Fixed 1280 x 800 design size.

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "BrowserController.h"

namespace sawblade::plugin {

class CaptureBrowser : public juce::Component, private juce::Timer {
 public:
  static constexpr int kWidth = 1280, kHeight = 800;

  CaptureBrowser(SawbladeProcessor& p, BrowserSettings& settings, Slot slot);
  ~CaptureBrowser() override;

  void paint(juce::Graphics&) override;
  void resized() override;

  BrowserController& controller() { return *ctl_; }
  std::function<void()> onClose;  // the "< RIG" button

  // The footer note of the filter column.
  static const char* licenceNote();

 private:
  struct Impl;
  void timerCallback() override;
  std::unique_ptr<BrowserController> ctl_;
  std::unique_ptr<Impl> impl_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CaptureBrowser)
};

}  // namespace sawblade::plugin
