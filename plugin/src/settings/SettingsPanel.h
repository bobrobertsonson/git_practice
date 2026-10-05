#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "Settings.h"

namespace sawblade::plugin::settings {

// The Settings overlay (docs/specs/phase11_settings.md sections 3 and 4): docked over the rig area of
// the 1280 x 800 design, closed by default (open / closed is UI state, never saved). Tools, TONE3000
// login, capture cache, separation, takes, appearance, the first-run checklist and a link to the About
// box. Reads and writes the Settings object it is given (the editor passes Settings::shared()).
class SettingsPanel : public juce::Component {
 public:
  static constexpr int kWidth = 940, kHeight = 742;

  SettingsPanel(SawbladeProcessor& p, Settings& s);
  ~SettingsPanel() override;

  void paint(juce::Graphics&) override;
  void resized() override;
  bool keyPressed(const juce::KeyPress&) override;

  // Shows the panel and re-evaluates it. firstRun expands the checklist and shows DONE.
  void open(bool firstRun = false);
  // Hides it; if this was the first run (Settings::isFirstRun) the settings file is written now.
  void close();
  void refresh();

  std::function<void()> onClosed;  // after close()
  std::function<void()> onAbout;   // the footer's About Sawblade... button

  bool checklistExpanded() const;
  bool loginRunning() const;
  // Checklist lights for tests: row 0 tools, 1 logged in, 2 captures; 0 unknown, 1 ok, 2 warning, 3 problem.
  int checklistLight(int row) const;

  // Once per process: true for the first caller (the editor that shows the first-run checklist).
  static bool claimFirstRunShow();
  static void resetFirstRunShownForTests();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsPanel)
};

}  // namespace sawblade::plugin::settings
