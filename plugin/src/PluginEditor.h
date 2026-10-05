#pragma once

#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "SawbladeLookAndFeel.h"
#include "rig/AmpHead.h"
#include "rig/RigEditorPanel.h"
#include "skin/RigView.h"

namespace sawblade::plugin {

class MicPage;
class PresetBrowser;
class AbCompare;
class ExportPanel;

// Skinned prototype of the main rig screen (design/mockups/RigReal.dc.html, spec
// docs/specs/phase2_5_skin.md). A fixed 1280 x 800 design laid out in one content component that
// the editor scales with an AffineTransform; the editor is resizable at a fixed 1.6 aspect.
// Reads status() and the APVTS; the only things it changes are the preset file chooser's load and the
// play-along (PlayAlongPanel / folder drop), which act on the processor's PlayAlong object.
class SawbladeEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, private juce::Timer {
 public:
  static constexpr int kDesignWidth = 1280, kDesignHeight = 800;

  explicit SawbladeEditor(SawbladeProcessor& p);
  ~SawbladeEditor() override;

  void paint(juce::Graphics&) override;
  void resized() override;
  void parentHierarchyChanged() override;  // Standalone: window title "Sawblade - version . sha . dirty flag"

  // Scale of the content component (editor width / 1280).
  double contentScale() const;
  skin::Piece selectedPiece() const;

  // The PLAY ALONG overlay (PlayAlongPanel): closed by default; open / closed is UI state, never saved.
  void setPlayAlongOpen(bool open);
  bool playAlongOpen() const;

  // The RIG overlay (rig::RigEditorPanel): every blend feature of the engine. Closed by default; open / closed
  // and the active tab are UI state, never saved.
  void setRigEditorOpen(bool open);
  bool rigEditorOpen() const;
  rig::RigEditorPanel& rigEditor();

  // v0.2 Task D: the amp controls over the amp head of path 0 = SAW / 1 = BODY, the rig controller (BLEND fill undo), a
  // synchronous refresh of the main screen (the timer does it at 4 Hz), and Cmd / Ctrl + Z for the undo.
  rig::AmpHead& ampHead(int path);
  rig::RigController& rigController();
  void refreshNow();
  bool keyPressed(const juce::KeyPress& k) override;

  // The cab mic page (mic/MicPage): an overlay over the rig + inspector, opened by double-clicking the cab; UI state, never saved.
  void setMicPageOpen(bool open);
  bool micPageOpen() const;
  MicPage& micPage();

  // The preset browser overlay (presets/PresetBrowser), opened from the top-bar preset selector, and the A/B compare slots.
  void setBrowserOpen(bool open);
  bool browserOpen() const;
  PresetBrowser& browser();
  AbCompare& abCompare();

  // The MATCH overlay (MatchScreen): opened from the play-along panel's button (Standalone only).
  void openMatchScreen();
  bool matchScreenOpen() const;
  // The EXPORT NAM panel (ExportPanel): opened from the top bar and the play-along panel, Standalone and plugin alike.
  void openExportPanel();
  bool exportPanelOpen() const;
  ExportPanel& exportPanel();

  // The Settings overlay (settings::SettingsPanel, phase 11) and the About box over it; UI state, never saved.
  void setSettingsOpen(bool open);
  bool settingsOpen() const;
  bool aboutOpen() const;

  // Dropping a folder (of stems) anywhere on the editor loads it as the song and opens the panel.
  bool isInterestedInFileDrag(const juce::StringArray& files) override;
  void filesDropped(const juce::StringArray& files, int x, int y) override;

 private:
  class Content;
  void timerCallback() override;
  SawbladeProcessor& processor_;
  int tick_ = 0;

  SawbladeLookAndFeel laf_;
  std::unique_ptr<Content> content_;
  juce::TooltipWindow tooltip_{this, 600};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SawbladeEditor)
};

}  // namespace sawblade::plugin
