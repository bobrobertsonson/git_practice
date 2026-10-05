#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

namespace sawblade::plugin {

// The PLAY ALONG overlay (docs/specs/phase5_2_playalong_plugin.md): docked along the bottom of the
// 1280 x 800 design surface, toggled by the PLAY ALONG button in the top bar, closed by default (open /
// closed is UI state and is not saved). Every control reads and writes the processor's PlayAlong object
// (settings, transport commands); nothing audio-related lives here. refresh() pulls the engine state into
// the controls (message thread, called from the editor's timer).
class PlayAlongPanel : public juce::Component {
 public:
  // The play-along controls take the top kPlayAlongHeight; the record / take band (REC, take list, MATCH, EXPORT NAM)
  // sits below them.
  static constexpr int kPlayAlongHeight = 170, kRecordHeight = 112, kHeight = kPlayAlongHeight + kRecordHeight, kWidth = 1280;

  explicit PlayAlongPanel(SawbladeProcessor& p);
  ~PlayAlongPanel() override;

  void paint(juce::Graphics&) override;
  void resized() override;
  void refresh();
  // The MATCH / EXPORT NAM buttons open the match screen (the editor wires these). In plugin mode (not
  // Standalone) they only show a note: open the Standalone app.
  std::function<void()> onMatch, onExport;
  // Opens the folder picker (asynchronous); a chosen folder is loaded as a user-initiated load.
  void chooseFolder();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayAlongPanel)
};

}  // namespace sawblade::plugin
