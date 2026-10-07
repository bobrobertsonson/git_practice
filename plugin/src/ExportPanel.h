#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ExportSettings.h"
#include "PluginProcessor.h"

namespace sawblade::plugin {

// The EXPORT NAM panel (docs/specs/phase12_export_in_plugin.md; layout after design/mockups/FullExport.dc.html, in the
// plugin's skin). It covers the editor below the top bar and is opened from the top bar and from the play-along panel,
// in the Standalone app and in a host alike. Three views: CONFIGURE (rig summary, mode / size / DI / comp / folder,
// what goes into the model, credits, TRAIN EXPORT or RESUME), TRAINING (progress bar, epoch N / M, best ESR, ETA,
// CANCEL) and RESULT (the acceptance report, REVEAL, OPEN FOLDER, A/B LISTEN, the sidecar, the licence note).
//
// It only reads and drives the processor's JobRunner and settings: the job runs in the processor, so closing the
// panel or the editor never stops it, and opening it re-attaches to whatever the job directory says. It never touches
// the audio thread. refresh() pulls the job snapshot into the controls (message thread; the editor's timer calls it).
class ExportPanel : public juce::Component {
 public:
  enum class View { Configure, Training, Result };
  static constexpr int kWidth = 1280, kHeight = 742;

  explicit ExportPanel(SawbladeProcessor& p);
  ~ExportPanel() override;

  void open();
  void close();
  bool isOpen() const { return isVisible(); }
  View view() const;
  // The settings as the controls show them (the processor's, which the plugin state saves).
  ExportSettings settings() const;
  void refresh();
  // EXPORT NOTES (v0.4 Task E): the stages of the rig that are NOT in the trained model, with hardware settings, as the
  // notes box shows them (and COPY puts on the clipboard). Before training: computed by the plugin (ExportNotes.h) for
  // the mode and DROP COMP shown; after training: the run report's `exportNotes` (v0.4M) when it has one of the version
  // this build knows, else the plugin's own with a "(computed by the plugin)" line.
  juce::String notesText() const;
  bool notesFromReport() const;
  // v0.6: when the finished run's report has `exportNotes.deviceProfiles.anagram`, a GENERIC / ANAGRAM switch appears above the
  // notes box; ANAGRAM shows that profile (blocks, positions, settings) and COPY copies what is shown. anagramNotesFile() is
  // the path of the run's `<name>.anagram_notes.txt` ("" = no profile).
  bool anagramNotesAvailable() const;
  bool anagramNotesShown() const;
  juce::String anagramNotesFile() const;

  void paint(juce::Graphics&) override;
  void resized() override;

  // Called when the panel closes (the RIG button).
  std::function<void()> onClose;
  // Hooks the tests replace: REVEAL (File::revealToUser), OPEN FOLDER and A/B LISTEN (File::startAsProcess).
  std::function<void(const juce::File&)> reveal, openFolder, openFile;
  // The COPY button's clipboard (default: juce::SystemClipboard::copyTextToClipboard); tests replace it.
  std::function<void(const juce::String&)> copyToClipboard;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExportPanel)
};

}  // namespace sawblade::plugin
