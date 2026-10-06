#include "rig/PedalPicker.h"

#include <nlohmann/json.hpp>

#include "SawbladeLookAndFeel.h"
#include "rig/RigModel.h"
#include "sawblade/pedal_params.h"

namespace sawblade::plugin::rig {
namespace {
using L = SawbladeLookAndFeel;

// One row: the model's name and its one-line description.
class RowButton : public juce::TextButton {
 public:
  explicit RowButton(const PedalPicker::Model& m) : juce::TextButton(m.name), blurb_(m.blurb) {
    setTitle("Add " + m.name);
    setTooltip("Add " + m.name + " to the board: " + m.blurb);
  }
  void paintButton(juce::Graphics& g, bool over, bool down) override {
    const auto b = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(down ? L::panelDeep().brighter(0.15f) : over ? L::panelDeep().brighter(0.06f) : L::panelDeep());
    g.fillRoundedRectangle(b, 6.0f);
    g.setColour(L::chipBorder());
    g.drawRoundedRectangle(b, 6.0f, 1.0f);
    auto r = getLocalBounds().reduced(12, 5);
    g.setColour(L::text());
    g.setFont(L::labelFont(13.0f));
    g.drawText(getButtonText(), r.removeFromTop(18), juce::Justification::centredLeft);
    g.setColour(L::dimText());
    g.setFont(L::bodyFont(11.0f));
    g.drawFittedText(blurb_, r, juce::Justification::centredLeft, 1, 0.8f);
  }

 private:
  juce::String blurb_;
};
}  // namespace

// The MODELED tab's page.
class PedalPicker::ModelList : public juce::Component {
 public:
  ModelList() {
    for (const Model& m : PedalPicker::modeledModels()) {
      auto row = std::make_unique<RowButton>(m);
      addAndMakeVisible(*row);
      rows_.push_back(std::move(row));
    }
  }
  juce::TextButton& row(int i) { return *rows_[static_cast<std::size_t>(i)]; }
  void resized() override {
    auto r = getLocalBounds();
    for (auto& row : rows_) row->setBounds(r.removeFromTop(kRowH).reduced(0, 2));
  }

 private:
  std::vector<std::unique_ptr<RowButton>> rows_;
};

const std::vector<PedalPicker::Model>& PedalPicker::modeledModels() {
  // Generic descriptors only (docs/specs/v0_4-pedals.md D3).
  static const std::vector<Model> m = {
      {"pedal.hm", "CHAINSAW", "Buzzing, scooped death-metal distortion"},
      {"pedal.hmx", "MODDED SAW", "The chainsaw with extra mid and presence controls"},
      {"pedal.eye", "ONE-KNOB SAW", "A saw tone from one gain knob and a tight switch"},
      {"pedal.muff", "BIG FUZZ", "Thick sustaining fuzz for doom and sludge"},
      {"pedal.ts", "GREEN OVERDRIVE", "Mid-focused overdrive: a tightening boost"},
  };
  return m;
}

Block PedalPicker::makeModeledBlock(const std::string& type) {
  using nlohmann::json;
  json fields = json::object();
  if (type == "pedal.hm") fields = {{"modelVersion", kHmModelVersion}};
  else if (type == "pedal.muff") fields = {{"modelVersion", kMuffModelVersion}};
  else if (type == "pedal.ts") fields = {{"modelVersion", kPedalModelVersion}, {"params", {{"drive", 0}, {"tone", 5}, {"level", 8}}}};
  return makeBlock(type, std::string(), "pedal", {}, fields);
}

PedalPicker::PedalPicker() {
  setTitle("Pedal picker");
  setWantsKeyboardFocus(true);
  setOpaque(false);
  auto list = std::make_unique<ModelList>();
  models_ = list.get();
  for (int i = 0; i < static_cast<int>(modeledModels().size()); ++i)
    models_->row(i).onClick = [this, i] {
      if (onPickModeled) onPickModeled(modeledModels()[static_cast<std::size_t>(i)].type);
    };
  addTab("MODELED", std::move(list));
  close_.setButtonText(juce::String::fromUTF8("\xc3\x97"));
  close_.setTitle("Close the pedal picker");
  close_.setTooltip("Close the picker (Escape)");
  close_.onClick = [this] {
    if (onClose) onClose();
  };
  addAndMakeVisible(close_);
  setSize(kWidth, kHeight);
}

PedalPicker::~PedalPicker() = default;

void PedalPicker::setPath(int path) {
  path_ = path;
  repaint();
}

int PedalPicker::addTab(const juce::String& title, std::unique_ptr<juce::Component> page) {
  items_.push_back({title, "Pedal picker tab " + title, "Show the " + title + " pedals", 1.0f});
  tabs_.setItems(items_);
  tabs_.onChange = [this](int i) { selectTab(i); };
  addAndMakeVisible(*page);
  pages_.push_back(std::move(page));
  if (tabs_.getParentComponent() == nullptr) addAndMakeVisible(tabs_);
  selectTab(tabs_.selected() >= 0 ? tabs_.selected() : 0);
  resized();
  return static_cast<int>(pages_.size()) - 1;
}

void PedalPicker::selectTab(int index) {
  for (int i = 0; i < static_cast<int>(pages_.size()); ++i) pages_[static_cast<std::size_t>(i)]->setVisible(i == index);
  tabs_.setSelected(index);
}

juce::TextButton& PedalPicker::modelButton(int index) { return models_->row(index); }

void PedalPicker::paint(juce::Graphics& g) {
  const auto b = getLocalBounds().toFloat().reduced(1.0f);
  juce::DropShadow(juce::Colours::black.withAlpha(0.8f), 14, {0, 6}).drawForRectangle(g, getLocalBounds().reduced(4));
  g.setColour(L::panel());
  g.fillRoundedRectangle(b, 10.0f);
  g.setColour(path_ == 0 ? L::saw() : L::body());
  g.drawRoundedRectangle(b, 10.0f, 1.5f);
}

void PedalPicker::resized() {
  auto r = getLocalBounds().reduced(10, 8);
  auto header = r.removeFromTop(kHeaderH - 12);
  close_.setBounds(header.removeFromRight(28));
  header.removeFromRight(6);
  tabs_.setBounds(header.removeFromLeft(juce::jmin(header.getWidth(), 120 * juce::jmax(1, static_cast<int>(pages_.size())))));
  r.removeFromTop(6);
  for (auto& p : pages_) p->setBounds(r);
}

bool PedalPicker::keyPressed(const juce::KeyPress& k) {
  if (k == juce::KeyPress::escapeKey) {
    if (onClose) onClose();
    return true;
  }
  return false;
}

}  // namespace sawblade::plugin::rig
