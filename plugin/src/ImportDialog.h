#pragma once

#include <functional>
#include <memory>
#include <string>

#include <juce_gui_basics/juce_gui_basics.h>

#include "DiImport.h"
#include "PluginProcessor.h"
#include "SongInput.h"

namespace sawblade::plugin {

// The IMPORT DI... dialog (v0.2.1 Task B): a small modal component laid over the editor (not a native alert, so it works in every
// host and a test can drive it with the mouse). It asks, for the file just chosen:
//   - stereo files only: which channel is the guitar: LEFT (default, the matcher's mono / left DI), RIGHT or SUM;
//   - "same performance as the song (my own recording)", default OFF. A DI played along to the song is a different performance and
//     is matched on tone only. A bounce of the recording the song was made from is a matched pair: then, and only then,
//   - "DI starts at [m:ss.mmm] in the song" (default 0:00.000; hint "a DI bounced from the start of the song: leave 0:00") and a
//     "don't know" checkbox (no offset: the matcher searches the whole song) are used, so they are enabled only with that box on.
// IMPORT decodes and writes the take on a worker thread (never the audio thread), shows the one-line reason when the file is silent
// or clipped, and on success calls onImported(take name) and closes. CANCEL / Esc closes (not while importing).
class ImportDialog : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer {
 public:
  ImportDialog(SawbladeProcessor& p, const juce::File& file, const di_import::Probe& probe);
  ~ImportDialog() override;

  // Adds the dialog over `parent` (filling it), owned by `slot`. CLOSE hides it at once and destroys it on the next message-loop turn.
  static void show(juce::Component& parent, std::unique_ptr<ImportDialog>& slot, SawbladeProcessor& p, const juce::File& file, const di_import::Probe& probe,
                   std::function<void(const std::string&)> onImported);

  void paint(juce::Graphics&) override;
  void resized() override;
  bool keyPressed(const juce::KeyPress&) override;
  void mouseDown(const juce::MouseEvent&) override {}  // the backdrop swallows clicks: the dialog is modal
  void close();
  // Modal: a drag over the dialog is taken (and ignored), so it cannot fall through to the editor and replace the song.
  bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
  void filesDropped(const juce::StringArray&, int, int) override {}

  // What the controls say now (tests read these; the controls are real components found by title).
  bool importing() const;

  std::function<void(const std::string&)> onImported;  // the new take's name
  std::function<void()> onClose;

 private:
  void timerCallback() override;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImportDialog)
};

// What a view (the take band, the MATCH screen) owns to start an import: the native chooser, the dialog and the rejection message.
// Both views use this one class, so a pick or a drop behaves the same wherever it comes from.
class DiImporter {
 public:
  DiImporter(SawbladeProcessor& p, juce::Component& view);
  ~DiImporter();

  // Called on the message thread: a file was refused before the dialog (wrong type, unreadable, > 2 channels): the one-line reason.
  std::function<void(const juce::String&)> onRejected;
  // A take was imported: its name. (The importer has already chosen it for MATCH.)
  std::function<void(const std::string&)> onImported;

  static song_input::ChooserSpec chooserSpec(bool mac = song_input::kIsMac) { return song_input::importChooserSpec(mac); }
  void choose();                              // the native chooser (files only)
  bool handlePicked(const juce::File& f);     // validates, opens the dialog; false (and onRejected) when refused; empty file = cancelled
  static bool isImportableDrop(const juce::StringArray& files);  // a WAV / AIFF / FLAC file among them
  bool handleDrop(const juce::StringArray& files);               // the first importable file; true if the dialog opened
  ImportDialog* dialog() { return dialog_.get(); }

 private:
  SawbladeProcessor& proc_;
  juce::Component& view_;
  std::unique_ptr<juce::FileChooser> chooser_;
  std::unique_ptr<ImportDialog> dialog_;
};

}  // namespace sawblade::plugin
