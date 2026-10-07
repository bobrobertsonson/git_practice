#include "rig/PedalPicker.h"

#include <nlohmann/json.hpp>

#include "SawbladeLookAndFeel.h"
#include "rig/RigModel.h"
#include "sawblade/pedal_params.h"

namespace sawblade::plugin::rig {
namespace {
using L = SawbladeLookAndFeel;

// One row: the model's name and its one-line description.
class RowButton : public juce::TextButton, public HasPedalKind {
 public:
  PedalKind pedalKind() const noexcept override { return PedalKind::Modeled; }
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

// One cached pedal capture: the title, "@creator . licence", the CAPTURE badge and the capture outline (cream, 2 px).
class CaptureRow : public juce::TextButton, public HasPedalKind {
 public:
  explicit CaptureRow(const CachedPedal& t) : juce::TextButton(juce::String(t.title)), tone_(t) {
    const juce::String lic = t.license.empty() ? juce::String("unknown licence") : juce::String(t.license);
    credit_ = "@" + juce::String(t.creator.empty() ? "unknown" : t.creator) + juce::String::fromUTF8(" \xc2\xb7 ") + lic.toUpperCase();
    if (t.models.size() >= 2) credit_ += juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(static_cast<int>(t.models.size())) + " SETTINGS";
    setTitle("Add capture " + juce::String(t.title));
    setTooltip("Add the cached capture " + juce::String(t.title) + " (" + credit_ + ") to the board; its level is matched to the path");
  }
  PedalKind pedalKind() const noexcept override { return PedalKind::Capture; }
  const CachedPedal& tone() const noexcept { return tone_; }
  void paintButton(juce::Graphics& g, bool over, bool down) override {
    const auto b = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(down ? L::panelDeep().brighter(0.15f) : over ? L::panelDeep().brighter(0.06f) : L::panelDeep());
    g.fillRoundedRectangle(b, 6.0f);
    paintKindOutline(g, pedalKind(), getLocalBounds().toFloat(), 6.0f, L::chipBorder());
    auto r = getLocalBounds().reduced(12, 5);
    paintKindBadge(g, pedalKind(), juce::Rectangle<float>(static_cast<float>(r.getRight() - 58), static_cast<float>(r.getY()), 58.0f, 15.0f));
    g.setColour(L::text());
    g.setFont(L::labelFont(13.0f));
    g.drawFittedText(getButtonText(), r.removeFromTop(18).withTrimmedRight(64), juce::Justification::centredLeft, 1, 0.7f);
    g.setColour(L::dimText());
    g.setFont(L::bodyFont(11.0f));
    g.drawFittedText(credit_, r, juce::Justification::centredLeft, 1, 0.8f);
  }

 private:
  CachedPedal tone_;
  juce::String credit_;
};

// "SEARCH TONE3000...": opens the capture browser in insert mode.
class SearchRow : public juce::TextButton {
 public:
  SearchRow() : juce::TextButton(juce::String::fromUTF8("SEARCH TONE3000\xe2\x80\xa6")) {
    setTitle("Search TONE3000 for a pedal capture");
    setTooltip("Open the capture browser to find a pedal capture on TONE3000; USE adds it to the board");
  }
  void paintButton(juce::Graphics& g, bool over, bool down) override {
    const auto b = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(down ? L::panelDeep().brighter(0.15f) : over ? L::panelDeep().brighter(0.06f) : L::panelDeep().darker(0.3f));
    g.fillRoundedRectangle(b, 6.0f);
    g.setColour(L::chipBorder());
    g.drawRoundedRectangle(b, 6.0f, 1.0f);
    g.setColour(L::sawText());
    g.setFont(L::labelFont(13.0f));
    g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred);
  }
};
}  // namespace

// The CAPTURES tab's page: a scrolling list.
class PedalPicker::CapturesPage : public juce::Component {
 public:
  CapturesPage() {
    viewport_.setViewedComponent(&list_, false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setScrollBarThickness(8);
    addAndMakeVisible(viewport_);
    addAndMakeVisible(search_);
  }
  void reload() {
    rows_.clear();
    for (const CachedPedal& t : cachedPedalCaptures()) {
      auto row = std::make_unique<CaptureRow>(t);
      row->onClick = [this, tone = t] {
        if (onPick) onPick(tone);
      };
      list_.addAndMakeVisible(*row);
      rows_.push_back(std::move(row));
    }
    search_.onClick = [this] {
      if (onSearch) onSearch();
    };
    resized();
  }
  int count() const { return static_cast<int>(rows_.size()); }
  juce::TextButton& row(int i) { return *rows_[static_cast<std::size_t>(i)]; }
  juce::TextButton& search() { return search_; }
  void resized() override {
    auto r = getLocalBounds();
    search_.setBounds(r.removeFromBottom(kRowH).reduced(0, 2));  // always visible below the list
    viewport_.setBounds(r);
    const int n = count();
    list_.setSize(r.getWidth() - (n * kRowH > r.getHeight() ? 10 : 0), std::max(1, n) * kRowH);
    int y = 0;
    for (auto& row : rows_) {
      row->setBounds(0, y, list_.getWidth(), kRowH);
      row->setBounds(row->getBounds().reduced(0, 2));
      y += kRowH;
    }
  }
  void paint(juce::Graphics& g) override {
    if (rows_.empty()) {
      g.setColour(L::dimText());
      g.setFont(L::bodyFont(12.0f));
      g.drawFittedText("No pedal captures in the cache yet: search TONE3000 below.", viewport_.getBounds().reduced(14, 6), juce::Justification::centredLeft, 3, 0.9f);
    }
  }
  std::function<void(const CachedPedal&)> onPick;
  std::function<void()> onSearch;

 private:
  juce::Viewport viewport_;
  juce::Component list_;
  std::vector<std::unique_ptr<CaptureRow>> rows_;
  SearchRow search_;
};

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
  auto caps = std::make_unique<CapturesPage>();
  captures_ = caps.get();
  captures_->onPick = [this](const CachedPedal& t) {
    if (onPickCapture) onPickCapture(t);
  };
  captures_->onSearch = [this] {
    if (onSearch) onSearch();
  };
  capturesTab_ = addTab("CAPTURES", std::move(caps));
  selectTab(0);
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
  selectTab(0);      // it always opens on MODELED
  reloadCaptures();  // the cache may have grown since the picker was last open
  repaint();
}

void PedalPicker::reloadCaptures() {
  if (captures_ != nullptr) captures_->reload();
}
int PedalPicker::captureRowCount() const { return captures_ != nullptr ? captures_->count() : 0; }
juce::TextButton& PedalPicker::captureRow(int i) { return captures_->row(i); }
juce::TextButton& PedalPicker::searchRow() { return captures_->search(); }

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
