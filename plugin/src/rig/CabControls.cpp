#include "rig/CabControls.h"

#include <utility>

#include "SawbladeLookAndFeel.h"

namespace sawblade::plugin::rig {
namespace {
using L = SawbladeLookAndFeel;

void setup(juce::Button& b, const juce::String& text, const juce::String& tip) {
  b.setButtonText(text);
  b.setTitle(text);
  b.setTooltip(tip);
}

void styleLabel(juce::Label& l, const juce::Font& f, juce::Colour c) {
  l.setFont(f);
  l.setColour(juce::Label::textColourId, c);
  l.setJustificationType(juce::Justification::centredLeft);
  l.setInterceptsMouseClicks(false, false);
}
}  // namespace

// ---------------------------------------------------------------------------------------------------
// One IR target: heading, the IR's title and credit, CHOOSE... and BROWSE IR.
struct CabControls::IrCard : juce::Component {
  IrCard(const juce::String& heading, juce::Colour accent, std::function<void()> chooseFn, std::function<void()> browseFn) : accent_(accent) {
    styleLabel(head, L::labelFont(11.0f), accent);
    head.setText(heading, juce::dontSendNotification);
    styleLabel(title, L::titleFont(15.0f), L::text());
    styleLabel(credit, L::bodyFont(11.0f), L::dimText());
    for (juce::Label* l : {&head, &title, &credit}) {
      l->setMinimumHorizontalScale(0.8f);
      addAndMakeVisible(*l);
    }
    setup(button, "CHOOSE...", "Choose a .wav impulse response file for " + heading);
    button.setTitle("Choose IR " + heading);
    button.onClick = std::move(chooseFn);
    addAndMakeVisible(button);
    setup(browse, "BROWSE IR", "Browse TONE3000 impulse responses for " + heading);
    browse.setTitle("Browse IR " + heading);
    browse.onClick = std::move(browseFn);
    addChildComponent(browse);
  }
  void set(const Capture& c) {
    title.setText(captureTitle(c), juce::dontSendNotification);
    credit.setText(captureCredit(c), juce::dontSendNotification);
  }
  void paint(juce::Graphics& g) override {
    g.setColour(L::panelDeep());
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
    g.setColour(accent_.withAlpha(0.5f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 8.0f, 1.0f);
  }
  void resized() override {
    auto r = getLocalBounds().reduced(14, 10);
    head.setBounds(r.removeFromTop(16));
    title.setBounds(r.removeFromTop(26));
    credit.setBounds(r.removeFromTop(20));
    r.removeFromTop(8);
    auto row = r.removeFromTop(32);
    button.setBounds(row.removeFromLeft(130));
    row.removeFromLeft(8);
    browse.setBounds(row.removeFromLeft(110));
  }
  juce::Colour accent_;
  juce::Label head, title, credit;
  juce::TextButton button, browse;
};

// ---------------------------------------------------------------------------------------------------
CabControls::CabControls(RigController& c, bool withActions) : controller(c), withActions_(withActions) {
  mode.setItems({{"SHARED", "Cab SHARED", "One cab IR after the blend: LIVE-COMPATIBLE, the no-cab NAM export is exact"},
                 {"PER PATH", "Cab PER PATH", "One IR per path before the sum: STUDIO BLEND, only the with-cab NAM export is exact"}});
  mode.onChange = [this](int i) {
    const CabMode m = i == 0 ? CabMode::Shared : CabMode::PerPath;
    controller.edit([m](Preset& p) { setCabMode(p, m); });
  };
  addAndMakeVisible(mode);
  on = std::make_unique<LedToggle>("CAB ON", "Cabinet impulse response on / off", L::saw());
  on->onClick = [this] {
    const bool v = on->getToggleState();
    controller.edit([v](Preset& p) { setCabEnabled(p, v); });
  };
  addAndMakeVisible(*on);
  const auto browseFn = [this] {
    if (onBrowseIr) onBrowseIr();
  };
  shared = std::make_unique<IrCard>("SHARED IR", L::text(), [this] { choose(CabSlot::Shared); }, browseFn);
  cardA = std::make_unique<IrCard>("PATH A IR (SAW)", L::saw(), [this] { choose(CabSlot::A); }, browseFn);
  cardB = std::make_unique<IrCard>("PATH B IR (BODY)", L::body(), [this] { choose(CabSlot::B); }, browseFn);
  for (auto* k : {shared.get(), cardA.get(), cardB.get()}) {
    addAndMakeVisible(*k);
    k->browse.setVisible(withActions_);
  }
  styleLabel(notice, L::titleFont(14.0f), L::live());
  addAndMakeVisible(notice);
  setup(mic, "MIC POSITIONS", "Open the mic page: pick a speaker, mic, distance and position from the cab's IR pack");
  mic.onClick = [this] {
    if (onMicPositions) onMicPositions();
  };
  addChildComponent(mic);
  mic.setVisible(withActions_);
}

CabControls::~CabControls() = default;

CabControls::IrCard& CabControls::card(CabSlot s) { return s == CabSlot::Shared ? *shared : s == CabSlot::A ? *cardA : *cardB; }
const CabControls::IrCard& CabControls::card(CabSlot s) const { return s == CabSlot::Shared ? *shared : s == CabSlot::A ? *cardA : *cardB; }

bool CabControls::cardVisible(CabSlot s) const { return card(s).isVisible(); }
juce::TextButton& CabControls::chooseButton(CabSlot s) { return card(s).button; }
juce::TextButton& CabControls::browseButton(CabSlot s) { return card(s).browse; }
juce::String CabControls::cardTitle(CabSlot s) const { return card(s).title.getText(); }

void CabControls::choose(CabSlot slot) {
  chooser = std::make_unique<juce::FileChooser>("Choose an impulse response", juce::File(), "*.wav");
  chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, slot](const juce::FileChooser& fc) {
    const juce::File f = fc.getResult();
    if (f == juce::File()) return;
    Capture c;
    c.file = f.getFullPathName().toStdString();
    c.resolvedPath = c.file;
    controller.edit([slot, c](Preset& p) { setCabIr(p, slot, c); });
  });
}

void CabControls::refresh(const Preset& p) {
  const bool perPath = p.cab.mode == CabMode::PerPath;
  mode.setSelected(perPath ? 1 : 0);
  on->setToggleState(p.cab.enabled, juce::dontSendNotification);
  shared->setVisible(!perPath);
  cardA->setVisible(perPath);
  cardB->setVisible(perPath);
  shared->set(p.cab.ir);
  cardA->set(p.cab.irA);
  cardB->set(p.cab.irB);
  // Same rule as the mode chip and exportExactness: a cab-less rig is live-compatible whatever the cab mode.
  if (perPath && p.cab.enabled) {
    notice.setText("STUDIO BLEND: only the with-cab NAM export is exact", juce::dontSendNotification);
    notice.setColour(juce::Label::textColourId, L::studio());
  } else {
    notice.setText("LIVE-COMPATIBLE: the no-cab NAM export is exact", juce::dontSendNotification);
    notice.setColour(juce::Label::textColourId, L::live());
  }
}

void CabControls::resized() {
  auto r = getLocalBounds().reduced(16, 8);
  auto row = r.removeFromTop(32);
  mode.setBounds(row.removeFromLeft(300));
  row.removeFromLeft(16);
  on->setBounds(row.removeFromLeft(120));
  r.removeFromTop(16);
  shared->setBounds(r.removeFromTop(120).removeFromLeft(440));
  cardA->setBounds(shared->getBounds());
  cardB->setBounds(shared->getBounds().translated(456, 0));
  r.removeFromTop(16);
  notice.setBounds(r.removeFromTop(28));
  if (withActions_) {
    r.removeFromTop(16);
    mic.setBounds(r.removeFromTop(34).removeFromLeft(180));
  }
}

}  // namespace sawblade::plugin::rig
