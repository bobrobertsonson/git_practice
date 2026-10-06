#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "ImportDialog.h"
#include "SongInput.h"

namespace sawblade::plugin {

// The MATCH overlay (docs/PLUGIN.md "Record + Match"; layout after design/mockups/FullMatch.dc.html, in the plugin's
// skin; EXPORT NAM has its own panel, ExportPanel). It covers the editor below the top bar and is opened from the
// play-along panel. It holds its own inputs (v0.2.1 Task D): 1 · REFERENCE SONG (pickers + drop + separation status),
// 2 · YOUR DI (REC / STOP + a take picker; IMPORT DI... joins it), then the tools and START MATCH. It only reads and drives the processor's JobRunner / PresetAudition / settings: the
// jobs run in the processor, so closing the screen (or the editor) never stops one, and opening it re-attaches
// to whatever the job directory says. refresh() pulls the job snapshots into the controls (message thread;
// the editor's timer calls it).
class MatchScreen : public juce::Component, public juce::FileDragAndDropTarget {
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

  // Section 1 · REFERENCE SONG: the same pickers, validation and drops as the play-along panel (SongInput.h). handlePicked
  // is what the SONG FILE... / STEMS FOLDER... choosers call with their result (so tests can drive it without a native
  // dialog); it returns whether something was loaded. A drop anywhere on this screen loads the song (section 1 lights up).
  bool handlePicked(song_input::Action a, const juce::File& f);
  void chooseSongFile();
  void chooseStemsFolder();
  bool isInterestedInFileDrag(const juce::StringArray& files) override;
  void fileDragEnter(const juce::StringArray& files, int x, int y) override;
  void fileDragMove(const juce::StringArray& files, int x, int y) override;
  void fileDragExit(const juce::StringArray& files) override;
  void filesDropped(const juce::StringArray& files, int x, int y) override;

  // Section 2 · YOUR DI: IMPORT DI... (a button, and a drop of a WAV / AIFF / FLAC file on the section) copies the file into the takes as
  // a take and chooses it (ImportDialog.h; the same DiImporter as the take band). dropSectionAt: which section a drag at `p` would land
  // in: 2 = the DI list takes it (an importable file over section 2), 1 = the song does.
  int dropSectionAt(const juce::StringArray& files, juce::Point<int> p) const;
  DiImporter& diImporter();

  // Called when the screen closes (CLOSE button).
  std::function<void()> onClose;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MatchScreen)
};

}  // namespace sawblade::plugin
