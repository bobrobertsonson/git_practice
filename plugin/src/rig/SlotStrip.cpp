#include "rig/SlotStrip.h"

#include <algorithm>

#include "sawblade/block_registry.h"

namespace sawblade::plugin::rig {
namespace {
using L = SawbladeLookAndFeel;

int indexOfId(const PathPreset& p, const std::string& id) {
  for (std::size_t i = 0; i < p.blocks.size(); ++i)
    if (p.blocks[i].id == id) return static_cast<int>(i);
  return -1;
}

juce::String kindText(const PathPreset& path, int index, const Block& b) {
  juce::String slot = b.slot.empty() ? (index == ampIndex(path) ? "amp" : "pedal") : b.slot;
  juce::String type = (b.type == "nam" || b.type == "eq") ? juce::String(b.type).toUpperCase() : juce::String(b.type);
  return slot.toUpperCase() + juce::String::fromUTF8(" \xC2\xB7 ") + type;
}

}  // namespace

// --- one block card --------------------------------------------------------------------------------
class SlotStrip::Card : public juce::Component {
 public:
  Card(SlotStrip& s, const PathPreset& path, int index, juce::Colour accent)
      : strip_(s), id_(path.blocks[static_cast<std::size_t>(index)].id), accent_(accent) {
    const Block& b = path.blocks[static_cast<std::size_t>(index)];
    const juce::String name = juce::String(b.id);
    setTitle("Block " + name);
    kind_.setText(kindText(path, index, b), juce::dontSendNotification);
    kind_.setFont(L::labelFont(10.0f));
    kind_.setColour(juce::Label::textColourId, accent);
    title_.setText(blockTitle(b), juce::dontSendNotification);
    title_.setFont(L::labelFont(13.0f));
    title_.setJustificationType(juce::Justification::topLeft);
    credit_.setText(blockCredit(b), juce::dontSendNotification);
    credit_.setFont(L::bodyFont(10.0f));
    credit_.setColour(juce::Label::textColourId, L::dimText());
    credit_.setJustificationType(juce::Justification::topLeft);
    for (juce::Label* l : {&kind_, &title_, &credit_}) {
      l->setInterceptsMouseClicks(false, false);
      l->setMinimumHorizontalScale(0.8f);
      addAndMakeVisible(*l);
    }
    title_.setTooltip(blockTitle(b));

    bypass_ = std::make_unique<LedToggle>("BYPASS", "Bypass this block: skipped, adds no latency (rebuilds the chain)", accent);
    bypass_->setTitle("Bypass " + name);
    bypass_->setToggleState(b.bypass, juce::dontSendNotification);
    bypass_->onClick = [this] {
      const bool on = bypass_->getToggleState();
      strip_.controller_.edit([this, on](Preset& p) {
        PathPreset& pp = strip_.pathOf(p);
        if (const int i = indexOfId(pp, id_); i >= 0) setBypass(pp, i, on);
      });
    };
    addAndMakeVisible(*bypass_);

    if (const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get())) {
      input_ = std::make_unique<PresetKnob>(
          strip_.controller_, "INPUT", skin::FilmstripKnob::Kind::Pedal, accent,
          skin::FilmstripKnob::Range{kBlockGainMinDb, kBlockGainMaxDb, 0.0, 1, "dB"},
          [this](Preset& p, double v) {
            PathPreset& pp = strip_.pathOf(p);
            if (const int i = indexOfId(pp, id_); i >= 0) setBlockInputGainDb(pp, i, v);
          },
          /*live=*/true);
      input_->knob().setTitle("Input gain " + name);
      input_->knob().setTooltip("Input gain into the model, -24 to +24 dB, changes live (drag, shift = fine, double-click = 0)");
      input_->setValueFromPreset(nam->inputGainDb);
      addAndMakeVisible(*input_);
    }

    auto mk = [this](juce::TextButton& t, const juce::String& text, const juce::String& title, const juce::String& tip) {
      t.setButtonText(text);
      t.setTitle(title);
      t.setTooltip(tip);
      addAndMakeVisible(t);
    };
    mk(left_, juce::String::fromUTF8("\xE2\x80\xB9"), "Move " + name + " left", "Move this block earlier in the chain");
    mk(right_, juce::String::fromUTF8("\xE2\x80\xBA"), "Move " + name + " right", "Move this block later in the chain");
    mk(remove_, juce::String::fromUTF8("\xE2\x9C\x95"), "Remove " + name, "Remove this block from the chain");
    left_.setEnabled(index > 0);
    right_.setEnabled(index + 1 < static_cast<int>(path.blocks.size()));
    left_.onClick = [this] { move(-1); };
    right_.onClick = [this] { move(+1); };
    remove_.onClick = [this] {
      strip_.controller_.edit([this](Preset& p) {
        PathPreset& pp = strip_.pathOf(p);
        if (const int i = indexOfId(pp, id_); i >= 0) removeBlock(pp, i);
      });
    };
    dimmed_ = b.bypass;
  }

  const std::string& id() const { return id_; }
  juce::Button& bypassButton() { return *bypass_; }
  juce::Button& leftButton() { return left_; }
  juce::Button& rightButton() { return right_; }
  juce::Button& removeButton() { return remove_; }
  PresetKnob* inputKnob() { return input_.get(); }
  bool dimmed() const { return dimmed_; }

  void paint(juce::Graphics& g) override {
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(L::panel());
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour((dimmed_ ? L::chipBorder() : accent_.withAlpha(0.55f)));
    g.drawRoundedRectangle(r, 6.0f, 1.0f);
    if (dimmed_) {  // a bypassed block stays visible, just quieter
      g.setColour(L::background().withAlpha(0.45f));
      g.fillRoundedRectangle(r, 6.0f);
    }
  }

  void resized() override {
    auto r = getLocalBounds().reduced(8);
    kind_.setBounds(r.removeFromTop(14));
    r.removeFromTop(2);
    title_.setBounds(r.removeFromTop(44));
    credit_.setBounds(r.removeFromTop(36));
    r.removeFromTop(2);
    bypass_->setBounds(r.removeFromTop(24));
    auto bottom = r.removeFromBottom(26);
    const int w = bottom.getWidth() / 3;
    left_.setBounds(bottom.removeFromLeft(w).reduced(1));
    right_.setBounds(bottom.removeFromLeft(w).reduced(1));
    remove_.setBounds(bottom.reduced(1));
    r.removeFromBottom(4);
    if (input_) input_->setBounds(r.withTrimmedTop(4));
  }

 private:
  void move(int dir) {
    strip_.controller_.edit([this, dir](Preset& p) {
      PathPreset& pp = strip_.pathOf(p);
      if (const int i = indexOfId(pp, id_); i >= 0) moveBlock(pp, i, i + dir);
    });
  }

  SlotStrip& strip_;
  std::string id_;
  juce::Colour accent_;
  bool dimmed_ = false;
  juce::Label kind_, title_, credit_;
  std::unique_ptr<LedToggle> bypass_;
  std::unique_ptr<PresetKnob> input_;
  juce::TextButton left_, right_, remove_;
};

// --- the strip -------------------------------------------------------------------------------------
SlotStrip::SlotStrip(RigController& c, int path, const juce::String& title, juce::Colour accent)
    : controller_(c), path_(path), title_(title), accent_(accent) {
  setTitle("Lane " + title);
  heading_.setText(title, juce::dontSendNotification);
  heading_.setFont(L::titleFont(15.0f));
  heading_.setColour(juce::Label::textColourId, accent);
  heading_.setInterceptsMouseClicks(false, false);
  addAndMakeVisible(heading_);
  add_.setButtonText("+ ADD");
  add_.setTitle("Add block to " + title);
  add_.setTooltip("Add a block to this path: a NAM capture, an EQ, or any other registered block type");
  add_.onClick = [this] { showAddMenu(); };
  addAndMakeVisible(add_);
  emptySlot_.setButtonText("+ 2ND PEDAL");
  emptySlot_.setTitle("Add the second pedal to " + title);
  emptySlot_.setTooltip("SINGLE + 2 PEDALS: this path has no second pedal yet; add one");
  emptySlot_.onClick = [this] { showAddMenu(); };
  addChildComponent(emptySlot_);
}

SlotStrip::~SlotStrip() = default;

juce::Component& SlotStrip::card(int i) { return *cards_[static_cast<std::size_t>(i)]; }

void SlotStrip::refresh(const Preset& p, bool emptyPedalSlot) {
  const PathPreset& pp = path_ == 0 ? p.a : p.b;
  if (pp.blocks == shown_ && emptyPedalSlot == shownEmpty_) return;
  shown_ = pp.blocks;
  shownEmpty_ = emptyPedalSlot;
  cards_.clear();
  for (int i = 0; i < static_cast<int>(pp.blocks.size()); ++i) {
    cards_.push_back(std::make_unique<Card>(*this, pp, i, accent_));
    addAndMakeVisible(*cards_.back());
  }
  add_.setEnabled(static_cast<int>(pp.blocks.size()) < kMaxBlocksPerPath);
  emptySlot_.setVisible(emptyPedalSlot && add_.isEnabled());
  resized();
  repaint();
}

void SlotStrip::paint(juce::Graphics& g) {
  g.setColour(L::panelDeep());
  g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
  if (cards_.empty()) {
    g.setColour(L::dimText());
    g.setFont(L::bodyFont(12.0f));
    g.drawText("Empty path: use + ADD to place a block (a NAM capture, an EQ ...)", 14, 40, getWidth() - 120, 20, juce::Justification::centredLeft);
  }
}

void SlotStrip::resized() {
  heading_.setBounds(14, 6, 300, 22);
  auto r = getLocalBounds().reduced(10, 0);
  r.removeFromTop(32);
  r.removeFromBottom(10);
  const int n = static_cast<int>(cards_.size()) + (emptySlot_.isVisible() ? 1 : 0);
  add_.setBounds(r.removeFromRight(kAddWidth).withHeight(32));
  r.removeFromRight(kCardGap);
  const int avail = r.getWidth();
  const int cardW = n > 0 ? std::clamp((avail - kCardGap * (n - 1)) / n, kMinCard, kMaxCard) : kMaxCard;
  int x = r.getX();
  for (auto& c : cards_) {
    c->setBounds(x, r.getY(), cardW, r.getHeight());
    x += cardW + kCardGap;
  }
  emptySlot_.setBounds(x, r.getY(), cardW, 44);
}

void SlotStrip::showAddMenu() {
  juce::PopupMenu m;
  const auto names = BlockRegistry::instance().typeNames();
  int id = 1;
  for (const auto& n : names) {
    juce::String text = n == "nam" ? "nam  (NAM capture, choose a .nam file...)" : n == "eq" ? "eq  (flat parametric EQ band)" : juce::String(n);
    m.addItem(id++, text);
  }
  m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_), [this, names](int chosen) {
    if (chosen >= 1 && chosen <= static_cast<int>(names.size())) addType(names[static_cast<std::size_t>(chosen - 1)]);
  });
}

void SlotStrip::addType(const juce::String& type) {
  if (type == "nam") {
    chooser_ = std::make_unique<juce::FileChooser>("Choose a NAM capture", juce::File(), "*.nam");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
      const juce::File f = fc.getResult();
      if (f != juce::File()) addBlockOfType("nam", f);
    });
    return;
  }
  addBlockOfType(type.toStdString(), juce::File());
}

void SlotStrip::addBlockOfType(const std::string& type, const juce::File& namFile) {
  nlohmann::json fields = nlohmann::json::object();
  std::filesystem::path base = std::filesystem::current_path();
  if (type == "nam") {
    fields = {{"model", {{"file", namFile.getFullPathName().toStdString()}}}};
    base = namFile.getParentDirectory().getFullPathName().toStdString();
  } else if (type == "eq") {
    fields = flatEqFields();
  }
  const char which = path_ == 0 ? 'a' : 'b';
  const Preset current = controller_.view();
  if (static_cast<int>((path_ == 0 ? current.a : current.b).blocks.size()) >= kMaxBlocksPerPath) {
    if (onMessage) onMessage("max 8 blocks per path");
    return;
  }
  // Validate first (a bad block must not trigger a rebuild); the id is chosen again on the edit base.
  Block probe;
  try {
    const Preset v = controller_.view();
    const PathPreset& pp = path_ == 0 ? v.a : v.b;
    probe = makeBlock(type, newBlockId(v, which), defaultSlotFor(pp, type), base, fields);
  } catch (const std::exception& e) {
    if (onMessage) onMessage(juce::String("Cannot add ") + type + ": " + e.what());
    return;
  }
  controller_.edit([this, probe, which](Preset& p) mutable {
    PathPreset& pp = pathOf(p);
    Block b = probe;
    b.id = newBlockId(p, which);
    b.slot = defaultSlotFor(pp, b.type);
    // A pedal goes in front of the amp, an amp (or the first block) at the end.
    const int amp = ampIndex(pp);
    const int at = (b.slot != "amp" && amp >= 0) ? amp : static_cast<int>(pp.blocks.size());
    addBlock(pp, at, std::move(b));
  });
}

}  // namespace sawblade::plugin::rig
