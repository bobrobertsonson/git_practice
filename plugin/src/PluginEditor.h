#pragma once

#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "SawbladeLookAndFeel.h"
#include "skin/RigView.h"

namespace sawblade::plugin {

// Skinned prototype of the main rig screen (design/mockups/RigReal.dc.html, spec
// docs/specs/phase2_5_skin.md). A fixed 1280 x 800 design laid out in one content component that
// the editor scales with an AffineTransform; the editor is resizable at a fixed 1.6 aspect.
// Reads status() and the APVTS only: no processor, preset or parameter changes.
class SawbladeEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, private juce::Timer {
 public:
  static constexpr int kDesignWidth = 1280, kDesignHeight = 800;

  explicit SawbladeEditor(SawbladeProcessor& p);
  ~SawbladeEditor() override;

  void paint(juce::Graphics&) override;
  void resized() override;

  // Scale of the content component (editor width / 1280).
  double contentScale() const;
  skin::Piece selectedPiece() const;

  // The PLAY ALONG overlay (PlayAlongPanel): closed by default; open / closed is UI state, never saved.
  void setPlayAlongOpen(bool open);
  bool playAlongOpen() const;

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
