#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

namespace sawblade::plugin {

// The MATCH overlay (docs/PLUGIN.md "Record + Match"; layout after design/mockups/FullMatch.dc.html, in the plugin's
// skin; EXPORT NAM has its own panel, ExportPanel). It covers the editor below the top bar and is opened from the
// play-along panel. It only reads and drives the processor's JobRunner / PresetAudition / settings: the
// jobs run in the processor, so closing the screen (or the editor) never stops one, and opening it re-attaches
// to whatever the job directory says. refresh() pulls the job snapshots into the controls (message thread;
// the editor's timer calls it).
class MatchScreen : public juce::Component {
 public:
  static constexpr int kWidth = 1280, kHeight = 742;

  explicit MatchScreen(SawbladeProcessor& p);
  ~MatchScreen() override;

  void open();
  void close();
  bool isOpen() const { return isVisible(); }
  void refresh();

  void paint(juce::Graphics&) override;
  void resized() override;

  // Called when the screen closes (CLOSE button).
  std::function<void()> onClose;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MatchScreen)
};

}  // namespace sawblade::plugin
