#include "PluginEditor.h"

#include <cmath>

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;
}

SawbladeEditor::SawbladeEditor(SawbladeProcessor& p) : juce::AudioProcessorEditor(p), processor_(p) {
  setLookAndFeel(&laf_);

  title_.setText("Sawblade", juce::dontSendNotification);
  title_.setFont(L::titleFont());
  addAndMakeVisible(title_);
  for (juce::Label* l : {&presetName_, &info_, &message_}) {
    l->setFont(L::bodyFont());
    addAndMakeVisible(*l);
  }
  info_.setFont(L::smallFont());
  info_.setColour(juce::Label::textColourId, L::dimText());
  message_.setFont(L::smallFont());

  loadButton_.onClick = [this] { chooseFile(); };
  addAndMakeVisible(loadButton_);

  for (int i = 0; i < kNumParams; ++i) {
    Row& r = rows_[static_cast<std::size_t>(i)];
    const ParamSpec& s = paramSpec(i);
    r.label.setText(s.name, juce::dontSendNotification);
    r.label.setFont(L::bodyFont());
    addAndMakeVisible(r.label);
    addAndMakeVisible(r.slider);
    r.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor_.parameters(), s.id, r.slider);
  }

  setSize(L::windowWidth, L::margin * 2 + L::rowHeight * (5 + kNumParams) + 40);
  refresh();
  startTimerHz(4);
}

SawbladeEditor::~SawbladeEditor() {
  stopTimer();
  setLookAndFeel(nullptr);
}

void SawbladeEditor::paint(juce::Graphics& g) { g.fillAll(L::background()); }

void SawbladeEditor::resized() {
  auto area = getLocalBounds().reduced(L::margin);
  auto top = area.removeFromTop(L::rowHeight + 4);
  loadButton_.setBounds(top.removeFromRight(130));
  title_.setBounds(top);
  presetName_.setBounds(area.removeFromTop(L::rowHeight));
  info_.setBounds(area.removeFromTop(L::rowHeight * 2));
  message_.setBounds(area.removeFromTop(L::rowHeight * 2));
  for (auto& r : rows_) {
    auto row = area.removeFromTop(L::rowHeight);
    r.label.setBounds(row.removeFromLeft(L::labelWidth));
    r.slider.setBounds(row);
  }
}

void SawbladeEditor::timerCallback() { refresh(); }

void SawbladeEditor::refresh() {
  const auto st = processor_.status();
  presetName_.setText("Preset: " + juce::String(st.presetName), juce::dontSendNotification);

  const double ms = st.hostRate > 0.0 ? 1000.0 * st.latencySamples / st.hostRate : 0.0;
  juce::String info;
  info << "Latency: " << st.latencySamples << " samples (" << juce::String(ms, 2) << " ms)    "
       << (st.hostRate > 0.0 ? "Rate: host " + juce::String(st.hostRate, 0) + " Hz, models " + juce::String(st.modelRate, 0) + " Hz" +
                                   (st.resampling ? " (resampling)" : "")
                             : juce::String("Rate: not running"))
       << "\n"
       << "Live-compatible blend (shared cab): " << (st.liveCompatible ? "yes" : "no")
       << "    Alignment delay: " << (st.info.alignDelay[0] + st.info.alignDelay[1]) << " samples";
  info_.setText(info, juce::dontSendNotification);

  if (st.loading) {
    message_.setColour(juce::Label::textColourId, L::warning());
    message_.setText("Loading...", juce::dontSendNotification);
  } else if (!st.error.empty()) {
    message_.setColour(juce::Label::textColourId, L::error());
    message_.setText(juce::String(st.error), juce::dontSendNotification);
  } else if (!st.info.warnings.empty()) {
    message_.setColour(juce::Label::textColourId, L::warning());
    message_.setText(juce::String(st.info.warnings.front()), juce::dontSendNotification);
  } else {
    message_.setText({}, juce::dontSendNotification);
  }

  const SlotBands bands = processor_.postEqSlots();
  for (int k = 0; k < kPostEqSlots; ++k) {
    Row& r = rows_[static_cast<std::size_t>(kPostEqFirst + k)];
    const bool used = bands[static_cast<std::size_t>(k)] >= 0;
    r.slider.setEnabled(used);
    r.label.setEnabled(used);
  }
}

void SawbladeEditor::chooseFile() {
  chooser_ = std::make_unique<juce::FileChooser>("Load a Sawblade preset", juce::File(), "*.json");
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                        [this](const juce::FileChooser& fc) {
                          const juce::File f = fc.getResult();
                          if (f == juce::File()) return;
                          processor_.loadPresetFile(std::filesystem::path(f.getFullPathName().toStdString()));
                        });
}

}  // namespace sawblade::plugin
