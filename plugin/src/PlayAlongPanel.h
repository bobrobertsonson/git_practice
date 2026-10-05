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
class PlayAlongPanel : public juce::Component, public juce::FileDragAndDropTarget {
 public:
  // The play-along controls take the top kPlayAlongHeight; the record / take band (REC, take list, MATCH, EXPORT NAM)
  // sits below them.
  static constexpr int kPlayAlongHeight = 170, kRecordHeight = 112, kHeight = kPlayAlongHeight + kRecordHeight, kWidth = 1280;

  explicit PlayAlongPanel(SawbladeProcessor& p);
  ~PlayAlongPanel() override;

  void paint(juce::Graphics&) override;
  void resized() override;
  void refresh();
  // The top bar's MATCH button: the panel (with its record + match band) is already open; in plugin mode this shows the
  // same "open the Standalone app" note as the panel's MATCH button, in Standalone it only brings the band up to date.
  void showMatchArea();
  // MATCH opens the match screen and EXPORT NAM opens the export panel (the editor wires both). In plugin mode (not
  // Standalone) MATCH only shows a note: open the Standalone app; EXPORT NAM works in a host too.
  std::function<void()> onMatch, onExport;
  // The two explicit pickers (asynchronous), each a one-purpose native chooser: a song file (files only, audio
  // filter) and a stems folder (directories only, no filter). A combined files+directories chooser with a type
  // filter is what the macOS panel greyed the .wav out of; see docs/PLUGIN.md. The result is a user-initiated load.
  enum class ChooserAction { SongFile, StemsFolder };
  struct ChooserSpec {
    juce::String title, filter;  // filter: ';'-separated "*.ext" wildcards, empty = none
    int flags;                   // juce::FileBrowserComponent flags
  };
#if JUCE_MAC
  static constexpr bool kIsMac = true;
#else
  static constexpr bool kIsMac = false;
#endif
  // Pure: testable without a native dialog. mac: the song chooser uses the filter "*" (no allowed-types list, the
  // panel delegate accepts everything) and handlePicked validates the pick instead.
  static ChooserSpec chooserSpec(ChooserAction a, bool mac = kIsMac);
  // A chosen file / folder: validated (a non-song file or a non-folder is refused with a message in the status label and
  // the current song is kept, never passed to loadSong), then loaded. Returns whether it was loaded. Empty file = cancelled.
  bool handlePicked(ChooserAction a, const juce::File& f);
  void chooseSongFile();
  void chooseStemsFolder();

  // File drops, shared with the editor (which accepts the same drops anywhere outside this panel): a song file or a
  // folder of stems is loaded as a user-initiated load. loadDroppedFiles returns whether something was loaded.
  static bool isLoadableDrop(const juce::StringArray& files);
  static bool loadDroppedFiles(SawbladeProcessor& proc, const juce::StringArray& files);
  bool isInterestedInFileDrag(const juce::StringArray& files) override;
  void filesDropped(const juce::StringArray& files, int x, int y) override;

 private:
  void launchChooser(ChooserAction a);
  struct Impl;
  std::unique_ptr<Impl> impl_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayAlongPanel)
};

}  // namespace sawblade::plugin
