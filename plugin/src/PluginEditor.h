#pragma once

#include <array>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "SawbladeLookAndFeel.h"

namespace sawblade::plugin {

// Plain functional editor: a preset-load button, read-outs (preset, latency, model/host rate,
// live-compatibility, load state/errors) and one labelled slider per parameter. Looks come from
// SawbladeLookAndFeel only.
class SawbladeEditor : public juce::AudioProcessorEditor, private juce::Timer {
 public:
  explicit SawbladeEditor(SawbladeProcessor& p);
  ~SawbladeEditor() override;

  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  struct Row {
    juce::Label label;
    juce::Slider slider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
  };

  void timerCallback() override;
  void refresh();
  void chooseFile();

  SawbladeProcessor& processor_;
  SawbladeLookAndFeel laf_;
  juce::Label title_, presetName_, info_, message_;
  juce::TextButton loadButton_{"Load preset..."};
  std::array<Row, kNumParams> rows_;
  std::unique_ptr<juce::FileChooser> chooser_;
};

}  // namespace sawblade::plugin
