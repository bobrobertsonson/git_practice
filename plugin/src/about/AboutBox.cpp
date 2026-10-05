#include "AboutBox.h"

#include "BinaryData.h"
#include "BuildInfo.h"
#include "CaptureList.h"
#include "SawbladeLookAndFeel.h"

namespace sawblade::plugin::about {
namespace {
using L = SawbladeLookAndFeel;
juce::String ju(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");

constexpr int kCardW = 840, kCardH = 700, kRowH = 54;

const char* kLicenceNote =
    "Built with JUCE 8 under the AGPLv3 for personal, non-commercial use. Sawblade is not sold. Giving a binary to anyone else requires "
    "publishing the source under the AGPLv3 or a JUCE licence (docs/THIRD_PARTY.md).";

class StatusDot : public juce::Component {
 public:
  bool ok = false;
  void paint(juce::Graphics& g) override {
    const auto c = ok ? L::live() : L::error();
    const auto r = getLocalBounds().toFloat().reduced(3.0f);
    g.setColour(c.withAlpha(0.25f));
    g.fillEllipse(r.expanded(2.0f));
    g.setColour(c);
    g.fillEllipse(r);
  }
};

// One capture row: dot, "Slot - title", creator / licence line, link.
struct Row : public juce::Component {
  StatusDot dot;
  juce::Label title, meta;
  juce::HyperlinkButton link;
  explicit Row(const CaptureRow& c) {
    dot.ok = c.onDisk;
    dot.setTitle(c.onDisk ? "on disk" : "missing");
    addAndMakeVisible(dot);
    title.setText(ju(c.slot) + ": " + ju(c.title), juce::dontSendNotification);
    title.setFont(L::titleFont(13.0f));
    title.setColour(juce::Label::textColourId, L::text());
    title.setMinimumHorizontalScale(0.8f);
    juce::String m;
    if (c.hasSource) {
      m = (c.creator.empty() ? juce::String("creator unknown") : "by " + ju(c.creator)) + kDot +
          (c.license.empty() ? juce::String("licence: unknown") : "licence: " + ju(c.license));
      if (c.nonCommercial) m += kDot + "non-commercial";
    } else {
      m = "local file, no TONE3000 metadata";
    }
    if (!c.onDisk) m += kDot + "file missing";
    meta.setText(m, juce::dontSendNotification);
    meta.setFont(L::bodyFont(12.0f));
    meta.setColour(juce::Label::textColourId, c.nonCommercial ? L::warning() : L::dimText());
    meta.setMinimumHorizontalScale(0.8f);
    for (juce::Label* l : {&title, &meta}) {
      l->setInterceptsMouseClicks(false, false);
      addAndMakeVisible(*l);
    }
    if (!c.url.empty()) {
      link.setButtonText(ju(c.url));
      link.setURL(juce::URL(ju(c.url)));
      link.setTitle("Open " + ju(c.title) + " in the browser");
      link.setTooltip("Open this capture's TONE3000 page in your browser");
      link.setFont(L::monoFont(11.0f), false);
      link.setJustificationType(juce::Justification::centredLeft);
      link.setColour(juce::HyperlinkButton::textColourId, L::bodyText());
      addAndMakeVisible(link);
    }
  }
  void resized() override {
    dot.setBounds(2, 6, 20, 20);
    title.setBounds(28, 2, getWidth() - 28, 20);
    meta.setBounds(28, 20, getWidth() - 28, 16);
    link.setBounds(28, 36, getWidth() - 28, 16);
  }
};
}  // namespace

struct AboutBox::Impl {
  SawbladeProcessor& proc;
  settings::Settings& settings;
  juce::Image icon;
  juce::Label name, version, licence, capHead, thirdHead, noCaptures;
  juce::Viewport capView;
  juce::Component capList;
  std::vector<std::unique_ptr<Row>> rows;
  juce::TextEditor third;
  juce::TextButton closeBtn;

  Impl(SawbladeProcessor& p, settings::Settings& s) : proc(p), settings(s) {}
};

AboutBox::AboutBox(SawbladeProcessor& p, settings::Settings& s) : impl_(std::make_unique<Impl>(p, s)) {
  auto& i = *impl_;
  setTitle("About Sawblade");
  setWantsKeyboardFocus(true);
  i.icon = juce::ImageFileFormat::loadFrom(BinaryData::icon_256_png, static_cast<size_t>(BinaryData::icon_256_pngSize));

  i.name.setText("SAWBLADE", juce::dontSendNotification);
  i.name.setFont(L::wordmarkFont());
  i.name.setColour(juce::Label::textColourId, L::saw());
  i.version.setText(juce::String("Sawblade ") + kVersion + kDot + kGitHash + kDot + "built " + kBuildDate, juce::dontSendNotification);
  i.version.setTitle("Version");
  i.version.setFont(L::monoFont(13.0f));
  i.version.setColour(juce::Label::textColourId, L::text());
  i.licence.setText(kLicenceNote, juce::dontSendNotification);
  i.licence.setFont(L::bodyFont(13.0f));
  i.licence.setColour(juce::Label::textColourId, L::dimText());
  i.licence.setMinimumHorizontalScale(1.0f);
  i.capHead.setText("CAPTURES IN THIS PRESET", juce::dontSendNotification);
  i.thirdHead.setText("THIRD-PARTY", juce::dontSendNotification);
  for (juce::Label* l : {&i.capHead, &i.thirdHead}) {
    l->setFont(L::labelFont(12.0f));
    l->setColour(juce::Label::textColourId, L::saw());
  }
  for (juce::Label* l : {&i.name, &i.version, &i.licence, &i.capHead, &i.thirdHead, &i.noCaptures}) {
    l->setInterceptsMouseClicks(false, false);
    addAndMakeVisible(*l);
  }

  const auto captures = listCaptures(p.currentPreset());
  for (const auto& c : captures) {
    i.rows.push_back(std::make_unique<Row>(c));
    i.capList.addAndMakeVisible(*i.rows.back());
  }
  i.noCaptures.setText("This preset uses no captures.", juce::dontSendNotification);
  i.noCaptures.setFont(L::bodyFont(12.0f));
  i.noCaptures.setColour(juce::Label::textColourId, L::dimText());
  i.noCaptures.setVisible(captures.empty());
  i.capList.setSize(kCardW - 80, std::max(1, static_cast<int>(captures.size())) * kRowH);
  i.capView.setViewedComponent(&i.capList, false);
  i.capView.setScrollBarsShown(true, false);
  i.capView.setTitle("Captures in this preset");
  addAndMakeVisible(i.capView);

  i.third.setMultiLine(true, true);
  i.third.setReadOnly(true);
  i.third.setCaretVisible(false);
  i.third.setScrollbarsShown(true);
  i.third.setTitle("Third-party licences");
  i.third.setTooltip("docs/THIRD_PARTY.md");
  i.third.setFont(L::monoFont(11.0f));
  i.third.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff141210));
  i.third.setColour(juce::TextEditor::textColourId, L::text());
  i.third.setColour(juce::TextEditor::outlineColourId, L::chipBorder());
  i.third.setText(juce::String::fromUTF8(BinaryData::THIRD_PARTY_md, BinaryData::THIRD_PARTY_mdSize), false);
  i.third.moveCaretToTop(false);
  addAndMakeVisible(i.third);

  i.closeBtn.setButtonText("CLOSE");
  i.closeBtn.setTitle("CLOSE");
  i.closeBtn.setTooltip("Close the About box");
  i.closeBtn.onClick = [this] { close(); };
  addAndMakeVisible(i.closeBtn);
  setSize(1280, 800);
}

AboutBox::~AboutBox() = default;

void AboutBox::show(juce::Component& parent, std::unique_ptr<AboutBox>& slot, SawbladeProcessor& p, settings::Settings& s) {
  slot = std::make_unique<AboutBox>(p, s);
  AboutBox* raw = slot.get();
  parent.addAndMakeVisible(*raw);
  raw->setBounds(parent.getLocalBounds());
  raw->toFront(true);
  juce::Component::SafePointer<juce::Component> sp(&parent);
  std::unique_ptr<AboutBox>* slotPtr = &slot;
  raw->onClose = [sp, slotPtr, raw] {
    raw->setVisible(false);
    juce::MessageManager::callAsync([sp, slotPtr, raw] {
      if (sp != nullptr && slotPtr->get() == raw) slotPtr->reset();
    });
  };
  if (raw->isShowing()) raw->grabKeyboardFocus();
}

void AboutBox::close() {
  if (onClose) onClose();
  else setVisible(false);
}

bool AboutBox::keyPressed(const juce::KeyPress& k) {
  if (k == juce::KeyPress::escapeKey) {
    close();
    return true;
  }
  return false;
}

void AboutBox::paint(juce::Graphics& g) {
  g.fillAll(juce::Colours::black.withAlpha(0.7f));
  const auto card = juce::Rectangle<int>(kCardW, kCardH).withCentre(getLocalBounds().getCentre());
  g.setColour(L::panel());
  g.fillRoundedRectangle(card.toFloat(), 8.0f);
  g.setColour(L::saw().withAlpha(0.6f));
  g.drawRoundedRectangle(card.toFloat().reduced(0.5f), 8.0f, 1.0f);
  if (impl_->icon.isValid()) g.drawImage(impl_->icon, juce::Rectangle<float>(static_cast<float>(card.getX() + 30), static_cast<float>(card.getY() + 26), 112.0f, 112.0f), juce::RectanglePlacement::centred);
  g.setColour(L::rule());
  const int x = card.getX() + 40, w = card.getWidth() - 80;
  g.fillRect(x, card.getY() + 206, w, 1);
}

void AboutBox::resized() {
  auto& i = *impl_;
  const auto card = juce::Rectangle<int>(kCardW, kCardH).withCentre(getLocalBounds().getCentre());
  const int x = card.getX() + 40, w = card.getWidth() - 80;
  int y = card.getY();
  i.name.setBounds(card.getX() + 160, y + 40, 400, 40);
  i.version.setBounds(card.getX() + 160, y + 84, w - 120, 22);
  i.licence.setBounds(card.getX() + 160, y + 112, w - 120, 78);
  i.capHead.setBounds(x, y + 216, 400, 18);
  i.capView.setBounds(x, y + 238, w, 170);
  i.capList.setSize(w - i.capView.getScrollBarThickness(), std::max(1, static_cast<int>(i.rows.size())) * kRowH);
  for (size_t r = 0; r < i.rows.size(); ++r) i.rows[r]->setBounds(0, static_cast<int>(r) * kRowH, i.capList.getWidth(), kRowH);
  i.noCaptures.setBounds(x, y + 240, w, 24);
  i.thirdHead.setBounds(x, y + 418, 400, 18);
  i.third.setBounds(x, y + 440, w, 190);
  i.closeBtn.setBounds(card.getRight() - 40 - 110, card.getBottom() - 56, 110, 34);
}

}  // namespace sawblade::plugin::about
