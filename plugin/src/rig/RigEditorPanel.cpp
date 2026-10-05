#include "rig/RigEditorPanel.h"

#include <algorithm>
#include <cmath>

#include "SawbladeLookAndFeel.h"
#include "rig/RigWidgets.h"
#include "rig/SlotStrip.h"
#include "skin/FilmstripKnob.h"

namespace sawblade::plugin::rig {
namespace {
using L = SawbladeLookAndFeel;
using skin::FilmstripKnob;
using Kind = FilmstripKnob::Kind;
const juce::String kDot = juce::String::fromUTF8(" \xC2\xB7 ");

void setup(juce::Button& b, const juce::String& text, const juce::String& tip) {
  b.setButtonText(text);
  b.setTitle(text);
  b.setTooltip(tip);
}

void styleLabel(juce::Label& l, const juce::Font& f, juce::Colour c, juce::Justification j = juce::Justification::centredLeft) {
  l.setFont(f);
  l.setColour(juce::Label::textColourId, c);
  l.setJustificationType(j);
  l.setInterceptsMouseClicks(false, false);
}

// A host-parameter knob (blend, levels, gate threshold) with caption and value text; the inspector holds the
// first knob bound to the same parameter (docs/specs/phase10_rig_editor.md section 1).
class BoundKnob : public juce::Component {
 public:
  BoundKnob(SawbladeProcessor& p, int param, const juce::String& caption, Kind kind, juce::Colour arc)
      : knob_(p.parameters(), paramSpec(param).id, paramSpec(param).name, kind, arc) {
    addAndMakeVisible(knob_);
    for (juce::Label* l : {&caption_, &value_}) {
      l->setInterceptsMouseClicks(false, false);
      l->setJustificationType(juce::Justification::centred);
      addAndMakeVisible(*l);
    }
    caption_.setText(caption, juce::dontSendNotification);
    caption_.setFont(L::labelFont(11.0f));
    value_.setFont(L::monoFont(11.0f));
    value_.setColour(juce::Label::textColourId, L::dimText());
    knob_.onValueChange = [this] { updateText(); };
    updateText();
  }
  FilmstripKnob& knob() { return knob_; }
  void updateText() { value_.setText(knob_.getTextFromValue(knob_.getValue()), juce::dontSendNotification); }
  void resized() override {
    auto r = getLocalBounds();
    value_.setBounds(r.removeFromBottom(14));
    caption_.setBounds(r.removeFromBottom(14));
    const int d = juce::jmin(r.getWidth(), r.getHeight());
    knob_.setBounds(juce::Rectangle<int>(d, d).withCentre(r.getCentre()));
  }

 private:
  FilmstripKnob knob_;
  juce::Label caption_, value_;
};

struct Page : juce::Component {
  virtual void refresh(const Preset& p, const SawbladeProcessor::Status& st, Topology topo) = 0;
  static void group(juce::Graphics& g, const juce::String& text, int x, int y) {
    g.setColour(L::dimText());
    g.setFont(L::labelFont(11.0f));
    g.drawText(text, x, y, 300, 14, juce::Justification::centredLeft);
  }
};

// ---------------------------------------------------------------------------------------------------
// CHAIN
struct ChainPage : Page {
  ChainPage(RigController& c, std::function<void(const juce::String&)> message)
      : laneA(c, 0, juce::String::fromUTF8("A \xC2\xB7 SAW"), L::saw()), laneB(c, 1, juce::String::fromUTF8("B \xC2\xB7 BODY"), L::body()) {
    laneA.onMessage = message;
    laneB.onMessage = message;
    addAndMakeVisible(laneA);
    addAndMakeVisible(laneB);
    off.setText("BLEND OFF - choose BLEND above to use path B", juce::dontSendNotification);
    styleLabel(off, L::labelFont(13.0f), L::dimText(), juce::Justification::centred);
    addChildComponent(off);
    offTitle.setText(juce::String::fromUTF8("B \xC2\xB7 BODY"), juce::dontSendNotification);
    styleLabel(offTitle, L::titleFont(15.0f), L::body().withAlpha(0.6f));
    addChildComponent(offTitle);
  }
  void refresh(const Preset& p, const SawbladeProcessor::Status&, Topology topo) override {
    laneA.refresh(p, topo == Topology::SinglePlusTwoPedals && activePedalSlots(p.a) < 2);
    laneB.refresh(p, false);
    blendOn = topo == Topology::Blend;
    laneB.setVisible(blendOn);
    off.setVisible(!blendOn);
    offTitle.setVisible(!blendOn);
    repaint();
  }
  void paint(juce::Graphics& g) override {
    if (!blendOn) {
      g.setColour(L::panelDeep());
      g.fillRoundedRectangle(laneB.getBounds().toFloat(), 8.0f);
    }
  }
  void resized() override {
    auto r = getLocalBounds().reduced(16, 6);
    const int h = (r.getHeight() - 12) / 2;
    laneA.setBounds(r.removeFromTop(h));
    r.removeFromTop(12);
    laneB.setBounds(r);
    off.setBounds(laneB.getBounds());
    offTitle.setBounds(laneB.getX() + 14, laneB.getY() + 6, 300, 22);
  }
  SlotStrip laneA, laneB;
  juce::Label off, offTitle;
  bool blendOn = true;
};

// ---------------------------------------------------------------------------------------------------
// EQ
struct EqPage : Page {
  EqPage(RigController& c, SawbladeProcessor& p) : graph(c), proc(p), controller(c) {
    target.setItems({{"A PRE", "EQ target A PRE", "Path A EQ before its blocks"},
                     {"A POST", "EQ target A POST", "Path A EQ after its blocks"},
                     {"B PRE", "EQ target B PRE", "Path B EQ before its blocks"},
                     {"B POST", "EQ target B POST", "Path B EQ after its blocks"},
                     {"POST", "EQ target POST", "The post EQ: after the blend and the cab"}});
    target.setSelected(4);
    target.onChange = [this](int i) { setTarget(static_cast<EqTarget>(i)); };
    addAndMakeVisible(target);
    addAndMakeVisible(graph);
    graph.onChanged = [this] { readout.setText(graph.readout(), juce::dontSendNotification); updateButtons(); };
    styleLabel(readout, L::monoFont(14.0f), L::text());
    addAndMakeVisible(readout);
    setup(addBand_, "+ BAND", "Add a peak band at 1 kHz (or double-click the graph where you want it)");
    setup(remove_, "REMOVE BAND", "Remove the selected band");
    enable_ = std::make_unique<LedToggle>("BAND ON", "Enable / disable the selected band (a rebuild, like double-clicking its node)");
    addBand_.onClick = [this] {
      const EqTarget t = graph.target();
      const int at = graph.numBands();
      graph.select(at);
      controller.edit([t](Preset& p) { addBand(p, t, EqBand{}); });
    };
    remove_.onClick = [this] {
      const EqTarget t = graph.target();
      const int i = graph.selected();
      if (i >= 0) controller.edit([t, i](Preset& p) { removeBand(p, t, i); });
    };
    enable_->onClick = [this] {
      const EqTarget t = graph.target();
      const int i = graph.selected();
      const bool on = enable_->getToggleState();
      if (i >= 0) controller.edit([t, i, on](Preset& p) { setBandEnabled(p, t, i, on); });
    };
    addAndMakeVisible(addBand_);
    addAndMakeVisible(remove_);
    addAndMakeVisible(*enable_);
    styleLabel(hint, L::bodyFont(12.0f), L::dimText());
    hint.setMinimumHorizontalScale(0.8f);
    addAndMakeVisible(hint);
    updateHint();
  }
  void setTarget(EqTarget t) {
    graph.setTarget(t);
    target.setSelected(static_cast<int>(t));
    updateHint();
    graph.refresh(controller.view(), proc.status().modelRate);
  }
  void updateHint() {
    hint.setText(graph.target() == EqTarget::Post
                     ? "POST EQ: the gain of the first six gain bands is a host parameter (POST EQ knobs on the right, automatable). "
                       "Frequency, Q, type and on/off are saved in the preset."
                     : "Path EQ: all values are saved in the preset. Frequency, Q and gain changes are live; type, on/off, add and remove rebuild the chain.",
                 juce::dontSendNotification);
  }
  void updateButtons() {
    const int i = graph.selected();
    remove_.setEnabled(i >= 0);
    enable_->setEnabled(i >= 0);
    if (i >= 0 && i < graph.numBands()) enable_->setToggleState(graph.band(i).enabled, juce::dontSendNotification);
    addBand_.setEnabled(graph.numBands() < ParametricEq::kMaxBands);
  }
  void refresh(const Preset& p, const SawbladeProcessor::Status& st, Topology topo) override {
    const bool single = topo != Topology::Blend;
    target.setItemEnabled(2, !single);
    target.setItemEnabled(3, !single);
    if (single && (graph.target() == EqTarget::PreB || graph.target() == EqTarget::EqB)) setTarget(EqTarget::Post);
    graph.refresh(p, st.modelRate);
  }
  void resized() override {
    auto r = getLocalBounds().reduced(16, 6);
    target.setBounds(r.removeFromTop(30).removeFromLeft(520));
    r.removeFromTop(10);
    auto top = r.removeFromTop(EqGraph::kHeight);
    graph.setBounds(top.removeFromLeft(EqGraph::kWidth));
    top.removeFromLeft(16);
    addBand_.setBounds(top.removeFromTop(32));
    top.removeFromTop(8);
    remove_.setBounds(top.removeFromTop(32));
    top.removeFromTop(8);
    enable_->setBounds(top.removeFromTop(32));
    r.removeFromTop(8);
    readout.setBounds(r.removeFromTop(24).removeFromLeft(EqGraph::kWidth));
    r.removeFromTop(6);
    hint.setBounds(r.removeFromTop(36));
  }
  Segmented target;
  EqGraph graph;
  juce::Label readout, hint;
  juce::TextButton addBand_, remove_;
  std::unique_ptr<LedToggle> enable_;
  SawbladeProcessor& proc;
  RigController& controller;
};

// ---------------------------------------------------------------------------------------------------
// BLEND
struct BlendPage : Page {
  BlendPage(RigController& c, SawbladeProcessor& p)
      : controller(c), proc(p), blend(p, kBlend, "BLEND", Kind::Amp, L::saw()), levelA(p, kLevelA, "SAW LEVEL", Kind::Pedal, L::saw()),
        levelB(p, kLevelB, "BODY LEVEL", Kind::Pedal, L::body()) {
    blend.knob().setTooltip("Blend between the SAW and BODY paths (host automatable)");
    for (juce::Component* k : std::initializer_list<juce::Component*>{&blend, &levelA, &levelB}) addAndMakeVisible(*k);
    styleLabel(blendRead, L::monoFont(15.0f), L::text(), juce::Justification::centred);
    addAndMakeVisible(blendRead);
    blend.knob().onValueChange = [this] { blend.updateText(); updateBlendText(); };
    for (int k = 0; k < 2; ++k) {
      const juce::String path = k == 0 ? "SAW" : "BODY";
      mute[k] = std::make_unique<LedToggle>("M", "Mute path " + path + " (monitoring only, never saved)", L::error());
      mute[k]->setTitle("Mute " + path);
      solo[k] = std::make_unique<LedToggle>("S", "Solo path " + path + " (monitoring only, never saved)", L::live());
      solo[k]->setTitle("Solo " + path);
      mute[k]->onClick = [this, k] { controller.setMute(k, mute[k]->getToggleState()); };
      solo[k]->onClick = [this, k] { controller.setSolo(k, solo[k]->getToggleState()); };
      addAndMakeVisible(*mute[k]);
      addAndMakeVisible(*solo[k]);
    }
    align.setItems({{"AUTO", "Align AUTO", "Measure the offset between the paths when the chain is built"},
                    {"MANUAL", "Align MANUAL", "Use the stored delay and polarity"},
                    {"OFF", "Align OFF", "No alignment beyond latency compensation"}});
    align.onChange = [this](int i) {
      const AlignMode m = i == 0 ? AlignMode::Auto : i == 1 ? AlignMode::Manual : AlignMode::Off;
      controller.edit([m](Preset& p) { setAlignMode(p, m); });
    };
    addAndMakeVisible(align);
    styleLabel(alignRead, L::monoFont(14.0f), L::text());
    addAndMakeVisible(alignRead);
    const int steps[4] = {-10, -1, 1, 10};
    for (int i = 0; i < 4; ++i) {
      const juce::String t = (steps[i] > 0 ? "+" : "") + juce::String(steps[i]);
      setup(nudge[i], t, "Move the BODY path " + juce::String(std::abs(steps[i])) + " sample(s) " + (steps[i] > 0 ? "later" : "earlier") +
                             " (switches AUTO to MANUAL, seeded with the measured values)");
      nudge[i].onClick = [this, n = steps[i]] { controller.nudgeAlign(n); };
      addAndMakeVisible(nudge[i]);
    }
    invert = std::make_unique<LedToggle>("INVERT B", "Flip the polarity of the BODY path (switches AUTO to MANUAL)", L::studio());
    invert->onClick = [this] { controller.setInvertB(invert->getToggleState()); };
    addAndMakeVisible(*invert);
    setup(remeasure, "RE-MEASURE", "Measure the alignment again and store it as manual values");
    remeasure.onClick = [this] { controller.remeasure(); };
    addAndMakeVisible(remeasure);
    setup(matchLevels, "MATCH LEVELS", "Measure both paths with a guitar-shaped probe and store the level trims that equalize them (manual)");
    matchLevels.onClick = [this] { controller.matchLevels(); };
    addAndMakeVisible(matchLevels);
    law.setItems({{"LINEAR", "Blend law LINEAR", "Linear crossfade: the loudness can change with BLEND", 1.0f},
                  {"CONSTANT", "Blend law CONSTANT", "Equal-power crossfade with make-up gain: the loudness stays the same at every BLEND", 1.35f}});
    law.onChange = [this](int i) { controller.setBlendLaw(i == 0 ? BlendLaw::Linear : BlendLaw::ConstantLoudness); };
    addAndMakeVisible(law);
    for (juce::Label* l : {&trimReadA, &trimReadB}) {
      styleLabel(*l, L::monoFont(11.0f), L::dimText(), juce::Justification::centred);
      addAndMakeVisible(*l);
    }
    trimReadA.setTitle("Saw level trim read-out");
    trimReadB.setTitle("Body level trim read-out");
  }
  // "+4.2 dB auto" / "+4.2 dB manual" / "0.0 dB off", plus the player's offset when it is not zero.
  static juce::String trimText(LevelMatchMode mode, double trim, double userOffsetDb) {
    const juce::String modeName = mode == LevelMatchMode::Auto ? "auto" : mode == LevelMatchMode::Manual ? "manual" : "off";
    juce::String s = (trim >= 0.05 ? "+" : "") + juce::String(trim, 1) + " dB " + modeName;
    if (std::abs(userOffsetDb) >= 0.05) s << kDot << (userOffsetDb > 0.0 ? "+" : "") << juce::String(userOffsetDb, 1) << " dB";
    return s;
  }
  void updateBlendText() {
    const double b = blend.knob().getValue();
    blendRead.setText("SAW " + juce::String(juce::roundToInt((1.0 - b) * 100.0)) + " / BODY " + juce::String(juce::roundToInt(b * 100.0)),
                      juce::dontSendNotification);
  }
  static juce::String alignText(int delay, bool inv, const juce::String& corr) {
    juce::String s = juce::String::fromUTF8("\xCE\x94 ") + juce::String(delay) + " smp" + kDot + juce::String::fromUTF8("\xC3\x98 ") + (inv ? "INVERTED" : "NORMAL");
    if (corr.isNotEmpty()) s << kDot << "corr " << corr;
    return s;
  }
  void refresh(const Preset& p, const SawbladeProcessor::Status& st, Topology topo) override {
    const bool blendOn = topo == Topology::Blend;
    blend.setEnabled(blendOn);
    levelB.setEnabled(blendOn);
    blend.knob().setEnabled(blendOn);
    levelB.knob().setEnabled(blendOn);
    updateBlendText();
    blend.updateText();
    levelA.updateText();
    levelB.updateText();
    for (int k = 0; k < 2; ++k) {
      mute[k]->setToggleState(controller.muted(k), juce::dontSendNotification);
      solo[k]->setToggleState(controller.solo(k), juce::dontSendNotification);
    }
    mute[1]->setEnabled(blendOn);
    solo[1]->setEnabled(blendOn);
    align.setSelected(p.align.mode == AlignMode::Auto ? 0 : p.align.mode == AlignMode::Manual ? 1 : 2);
    if (st.alignMeasuring) {
      alignRead.setText("measuring" + juce::String::fromUTF8("\xE2\x80\xA6"), juce::dontSendNotification);
    } else if (p.align.mode == AlignMode::Auto) {
      alignRead.setText(alignText(st.info.align.delaySamplesB, st.info.align.invertB, juce::String(st.info.align.peakCorrelation, 2)), juce::dontSendNotification);
    } else if (p.align.mode == AlignMode::Manual) {
      alignRead.setText(alignText(p.align.delaySamplesB, p.align.invertB, {}), juce::dontSendNotification);
    } else {
      alignRead.setText(juce::String::fromUTF8("\xE2\x80\x94"), juce::dontSendNotification);
    }
    const bool alignable = blendOn && p.align.mode != AlignMode::Off;
    for (auto& n : nudge) n.setEnabled(alignable);
    invert->setEnabled(alignable);
    invert->setToggleState(p.align.mode == AlignMode::Manual ? p.align.invertB : (p.align.mode == AlignMode::Auto ? st.info.align.invertB : false),
                           juce::dontSendNotification);
    const bool busy = st.alignMeasuring || st.levelsMeasuring;
    remeasure.setEnabled(blendOn && !st.loading && !busy && p.a.enabled && p.b.enabled);
    remeasure.setButtonText(st.alignMeasuring ? "measuring" + juce::String::fromUTF8("\xE2\x80\xA6") : "RE-MEASURE");
    matchLevels.setEnabled(blendOn && !st.loading && !busy && p.a.enabled && p.b.enabled);
    matchLevels.setButtonText(st.levelsMeasuring ? "measuring" + juce::String::fromUTF8("\xE2\x80\xA6") : "MATCH LEVELS");
    law.setSelected(p.blendLaw == BlendLaw::Linear ? 0 : 1);
    law.setEnabled(blendOn);
    const LevelMatchMode mode = p.levelMatch.mode;
    const double trimA = mode == LevelMatchMode::Auto ? st.info.trimDb[0] : mode == LevelMatchMode::Manual ? p.levelMatch.trimADb : 0.0;
    const double trimB = mode == LevelMatchMode::Auto ? st.info.trimDb[1] : mode == LevelMatchMode::Manual ? p.levelMatch.trimBDb : 0.0;
    trimReadA.setText(trimText(mode, trimA, levelA.knob().getValue()), juce::dontSendNotification);
    trimReadB.setText(blendOn ? trimText(mode, trimB, levelB.knob().getValue()) : juce::String::fromUTF8("\xE2\x80\x94"),
                      juce::dontSendNotification);
  }
  void paint(juce::Graphics& g) override {
    group(g, "BLEND", 16, 12);
    group(g, "PATH LEVELS", 410, 12);
    group(g, "ALIGN", 16, 250);
    g.setColour(L::rule());
    g.drawHorizontalLine(240, 16.0f, static_cast<float>(getWidth() - 16));
  }
  void resized() override {
    blend.setBounds(16, 34, 150, 170);
    blendRead.setBounds(16, 206, 220, 24);
    levelA.setBounds(410, 40, 110, 120);
    levelB.setBounds(550, 40, 110, 120);
    mute[0]->setBounds(412, 170, 50, 28);
    solo[0]->setBounds(468, 170, 50, 28);
    mute[1]->setBounds(552, 170, 50, 28);
    solo[1]->setBounds(608, 170, 50, 28);
    align.setBounds(16, 274, 300, 32);
    alignRead.setBounds(332, 274, 560, 32);
    for (int i = 0; i < 4; ++i) nudge[i].setBounds(16 + i * 58, 326, 54, 32);
    invert->setBounds(260, 326, 130, 32);
    remeasure.setBounds(402, 326, 140, 32);
    matchLevels.setBounds(552, 326, 150, 32);
    law.setBounds(176, 44, 216, 32);  // LINEAR | CONSTANT sized to their text, clear of PATH LEVELS (x 410)
    trimReadA.setBounds(400, 204, 130, 16);
    trimReadB.setBounds(540, 204, 130, 16);
  }
  RigController& controller;
  SawbladeProcessor& proc;
  BoundKnob blend, levelA, levelB;
  juce::Label blendRead, alignRead, trimReadA, trimReadB;
  std::unique_ptr<LedToggle> mute[2], solo[2], invert;
  Segmented align, law;
  juce::TextButton nudge[4], remeasure, matchLevels;
};

// ---------------------------------------------------------------------------------------------------
// CAB
struct CabPage : Page {
  struct IrCard : juce::Component {
    IrCard(const juce::String& heading, juce::Colour accent, std::function<void()> choose) : accent_(accent) {
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
      button.onClick = std::move(choose);
      addAndMakeVisible(button);
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
      button.setBounds(r.removeFromTop(32).removeFromLeft(130));
    }
    juce::Colour accent_;
    juce::Label head, title, credit;
    juce::TextButton button;
  };

  CabPage(RigController& c) : controller(c) {
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
    shared = std::make_unique<IrCard>("SHARED IR", L::text(), [this] { choose(CabSlot::Shared); });
    cardA = std::make_unique<IrCard>("PATH A IR (SAW)", L::saw(), [this] { choose(CabSlot::A); });
    cardB = std::make_unique<IrCard>("PATH B IR (BODY)", L::body(), [this] { choose(CabSlot::B); });
    for (auto* k : {shared.get(), cardA.get(), cardB.get()}) addAndMakeVisible(*k);
    styleLabel(notice, L::titleFont(14.0f), L::live());
    addAndMakeVisible(notice);
  }
  void choose(CabSlot slot) {
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
  void refresh(const Preset& p, const SawbladeProcessor::Status&, Topology) override {
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
  void resized() override {
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
  }
  RigController& controller;
  Segmented mode;
  std::unique_ptr<LedToggle> on;
  std::unique_ptr<IrCard> shared, cardA, cardB;
  juce::Label notice;
  std::unique_ptr<juce::FileChooser> chooser;
};

// ---------------------------------------------------------------------------------------------------
// GATE
struct GatePage : Page {
  GatePage(RigController& c, SawbladeProcessor& p) : controller(c), threshold(p, kGateThreshold, "THRESHOLD", Kind::Pedal, L::saw()) {
    on = std::make_unique<LedToggle>("GATE ON", "Noise gate on / off, keyed on the DI before the split");
    on->onClick = [this] {
      const bool v = on->getToggleState();
      controller.edit([v](Preset& p) { setGateEnabled(p, v); });
    };
    addAndMakeVisible(*on);
    mode.setItems({{"GATE", "Gate mode GATE", "Close fully (down to RANGE) below the threshold"},
                   {"EXPANDER", "Gate mode EXPANDER", "Expand downward below the threshold (RATIO)"}});
    mode.onChange = [this](int i) {
      const GateMode m = i == 0 ? GateMode::Gate : GateMode::Expander;
      controller.edit([m](Preset& p) { setGateMode(p, m); });
    };
    addAndMakeVisible(mode);
    curve.setItems({{"ONE-POLE", "Release curve ONE-POLE", "Exponential release"}, {"LINEAR dB", "Release curve LINEAR dB", "Constant dB per ms release"}});
    curve.onChange = [this](int i) {
      const GateReleaseCurve m = i == 0 ? GateReleaseCurve::OnePole : GateReleaseCurve::LinearDb;
      controller.edit([m](Preset& p) { setGateReleaseCurve(p, m); });
    };
    addAndMakeVisible(curve);
    addAndMakeVisible(threshold);
    threshold.knob().setTooltip("Gate threshold in dB (host automatable); LEARN sets it from your quiet input");
    auto add = [&](GateField f, const char* caption, const char* tip, FilmstripKnob::Range r, PresetKnob::Format fmt = {}) {
      knobs.push_back(std::make_unique<PresetKnob>(controller, caption, Kind::Pedal, L::saw(), r,
                                                   [f](Preset& p, double v) { setGateField(p, f, v); }, false, std::move(fmt)));
      knobs.back()->knob().setTooltip(juce::String(tip) + " (drag to turn, shift = fine, double-click = reset)");
      fields.push_back(f);
      addAndMakeVisible(*knobs.back());
    };
    add(GateField::Hysteresis, "HYSTERESIS", "Hysteresis: the gate closes this far below the threshold", {0.0, 60.0, 6.0, 1, "dB"});
    add(GateField::Attack, "ATTACK", "Opening time", {0.01, 1000.0, 0.5, 2, "ms", 5.0});
    add(GateField::Hold, "HOLD", "Minimum open time", {0.0, 10000.0, 20.0, 0, "ms", 100.0});
    add(GateField::Release, "RELEASE", "Closing time", {0.01, 10000.0, 60.0, 1, "ms", 100.0});
    add(GateField::Range, "RANGE", "Attenuation when closed", {-120.0, 0.0, -90.0, 0, "dB"});
    add(GateField::Ratio, "RATIO", "Expander ratio (EXPANDER mode only)", {1.5, 10.0, 4.0, 1, ":1"});
    add(GateField::KeyHpf, "KEY HPF", "High-pass on the detector key only (bottom position = off, else 40 to 400 Hz)", {0.0, 400.0, 0.0, 0, "Hz"},
        [](double v) { return v < 20.0 ? juce::String("OFF") : juce::String(juce::roundToInt(std::max(v, 40.0))) + " Hz"; });
    setup(learn, "LEARN", "Measure your quiet input for 1 s (do not play) and set the threshold 6 dB above it");
    learn.onClick = [this] { controller.learnGate(); };
    addAndMakeVisible(learn);
    styleLabel(learnText, L::monoFont(12.0f), L::dimText());
    addAndMakeVisible(learnText);
  }
  void refresh(const Preset& p, const SawbladeProcessor::Status&, Topology) override {
    on->setToggleState(p.gate.enabled, juce::dontSendNotification);
    mode.setSelected(p.gate.mode == GateMode::Gate ? 0 : 1);
    curve.setSelected(p.gate.releaseCurve == GateReleaseCurve::OnePole ? 0 : 1);
    threshold.updateText();
    for (std::size_t i = 0; i < knobs.size(); ++i) knobs[i]->setValueFromPreset(gateField(p.gate, fields[i]));
    const bool expander = p.gate.mode == GateMode::Expander;
    knobs[5]->setEnabled(expander);  // RATIO
    knobs[5]->setAlpha(expander ? 1.0f : 0.4f);
    learnText.setText(controller.learnStatus(), juce::dontSendNotification);
    learn.setEnabled(!controller.learning());
  }
  void paint(juce::Graphics& g) override {
    group(g, "RELEASE CURVE", 400, 20);
    group(g, "THRESHOLD AND TIMING", 16, 86);
    group(g, "INPUT LEARN", 16, 330);
  }
  void resized() override {
    on->setBounds(16, 12, 130, 32);
    mode.setBounds(160, 12, 220, 32);
    curve.setBounds(400, 38, 240, 32);
    threshold.setBounds(16, 110, 120, 130);
    const int cell = 116;
    for (std::size_t i = 0; i < knobs.size(); ++i) {
      const int col = static_cast<int>(i) % 3, row = static_cast<int>(i) / 3;
      knobs[i]->setBounds(150 + col * cell, 110 + row * 136, cell - 8, 128);
    }
    learn.setBounds(16, 354, 110, 32);
    learnText.setBounds(140, 354, 600, 32);
  }
  RigController& controller;
  BoundKnob threshold;
  std::unique_ptr<LedToggle> on;
  Segmented mode, curve;
  std::vector<std::unique_ptr<PresetKnob>> knobs;
  std::vector<GateField> fields;
  juce::TextButton learn;
  juce::Label learnText;
};

// ---------------------------------------------------------------------------------------------------
// COMP
struct CompPage : Page {
  explicit CompPage(RigController& c) : controller(c) {
    on = std::make_unique<LedToggle>("COMP ON", "Bus compressor on / off (after the post EQ)");
    on->onClick = [this] {
      const bool v = on->getToggleState();
      controller.edit([v](Preset& p) { setCompEnabled(p, v); });
    };
    addAndMakeVisible(*on);
    auto add = [&](CompField f, const char* caption, const char* tip, FilmstripKnob::Range r) {
      knobs.push_back(std::make_unique<PresetKnob>(controller, caption, Kind::Pedal, L::saw(), r,
                                                   [f](Preset& p, double v) { setCompField(p, f, v); }));
      knobs.back()->knob().setTooltip(juce::String(tip) + " (drag to turn, shift = fine, double-click = reset)");
      fields.push_back(f);
      addAndMakeVisible(*knobs.back());
    };
    add(CompField::Threshold, "THRESHOLD", "Compressor threshold", {-80.0, 0.0, -12.0, 1, "dB"});
    add(CompField::Ratio, "RATIO", "Compression ratio", {1.0, 100.0, 2.0, 1, ":1", 4.0});
    add(CompField::Knee, "KNEE", "Soft knee width", {0.0, 48.0, 6.0, 1, "dB"});
    add(CompField::Attack, "ATTACK", "Attack time", {0.01, 1000.0, 10.0, 2, "ms", 10.0});
    add(CompField::Release, "RELEASE", "Release time", {1.0, 10000.0, 100.0, 0, "ms", 100.0});
    add(CompField::Makeup, "MAKEUP", "Makeup gain", {-24.0, 48.0, 0.0, 1, "dB"});
    styleLabel(warn, L::titleFont(13.0f), L::warning());
    addChildComponent(warn);
  }
  void refresh(const Preset& p, const SawbladeProcessor::Status&, Topology) override {
    on->setToggleState(p.busComp.enabled, juce::dontSendNotification);
    for (std::size_t i = 0; i < knobs.size(); ++i) knobs[i]->setValueFromPreset(compField(p.busComp, fields[i]));
    const double rel = knobs[4]->knob().isMouseButtonDown() ? knobs[4]->value() : p.busComp.releaseMs;
    const bool slow = rel > kBusCompMaxTrainableReleaseMs;
    warn.setText("release > 150 ms: not NAM-trainable", juce::dontSendNotification);
    warn.setVisible(slow);
  }
  void paint(juce::Graphics& g) override { group(g, "BUS COMPRESSOR", 16, 66); }
  void resized() override {
    on->setBounds(16, 12, 130, 32);
    for (std::size_t i = 0; i < knobs.size(); ++i) {
      const int col = static_cast<int>(i) % 3, row = static_cast<int>(i) / 3;
      knobs[i]->setBounds(16 + col * 124, 90 + row * 140, 116, 128);
    }
    warn.setBounds(16, 380, 500, 24);
  }
  RigController& controller;
  std::unique_ptr<LedToggle> on;
  std::vector<std::unique_ptr<PresetKnob>> knobs;
  std::vector<CompField> fields;
  juce::Label warn;
};

}  // namespace

// ---------------------------------------------------------------------------------------------------
struct RigEditorPanel::Impl {
  Impl(RigEditorPanel& o, SawbladeProcessor& p, RigController& c) : owner(o), proc(p), ctl(c) {}

  RigEditorPanel& owner;
  SawbladeProcessor& proc;
  RigController& ctl;
  Segmented topology, tabs;
  juce::Label status;
  std::unique_ptr<ChainPage> chain;
  std::unique_ptr<EqPage> eq;
  std::unique_ptr<BlendPage> blend;
  std::unique_ptr<CabPage> cab;
  std::unique_ptr<GatePage> gate;
  std::unique_ptr<CompPage> comp;
  Tab current = Tab::Chain;
  juce::String transient;
  juce::uint32 transientUntil = 0;
  juce::String lastLearn;
  juce::uint32 learnShownAt = 0;

  Page& page(Tab t) {
    switch (t) {
      case Tab::Chain: return *chain;
      case Tab::Eq: return *eq;
      case Tab::Blend: return *blend;
      case Tab::Cab: return *cab;
      case Tab::Gate: return *gate;
      case Tab::Comp: return *comp;
    }
    return *chain;
  }

  void message(const juce::String& m) {
    transient = m;
    transientUntil = juce::Time::getMillisecondCounter() + 8000;
  }

  void build() {
    topology.setItems({{"SINGLE", "Topology SINGLE", "One path (SAW): the BODY path is off, its blocks are kept", 1.0f},
                       {"SINGLE + 2 PEDALS", "Topology SINGLE + 2 PEDALS", "One path with two pedal slots in front of the amp", 1.6f},
                       {"BLEND", "Topology BLEND", "Two paths blended (SAW and BODY)", 1.0f}});
    topology.onChange = [this](int i) { ctl.setTopology(i == 0 ? Topology::Single : i == 1 ? Topology::SinglePlusTwoPedals : Topology::Blend); };
    owner.addAndMakeVisible(topology);
    tabs.setItems({{"CHAIN", "Tab CHAIN", "Blocks of each path"},
                   {"EQ", "Tab EQ", "Graphical EQs"},
                   {"BLEND", "Tab BLEND", "Blend, path levels, mute / solo and alignment"},
                   {"CAB", "Tab CAB", "Cabinet impulse response(s)"},
                   {"GATE", "Tab GATE", "Noise gate / expander"},
                   {"COMP", "Tab COMP", "Bus compressor"}});
    tabs.onChange = [this](int i) { owner.setTab(static_cast<Tab>(i)); };
    owner.addAndMakeVisible(tabs);
    styleLabel(status, L::bodyFont(12.0f), L::dimText());
    status.setMinimumHorizontalScale(0.8f);
    owner.addAndMakeVisible(status);

    chain = std::make_unique<ChainPage>(ctl, [this](const juce::String& m) { message(m); });
    eq = std::make_unique<EqPage>(ctl, proc);
    blend = std::make_unique<BlendPage>(ctl, proc);
    cab = std::make_unique<CabPage>(ctl);
    gate = std::make_unique<GatePage>(ctl, proc);
    comp = std::make_unique<CompPage>(ctl);
    for (Tab t : {Tab::Chain, Tab::Eq, Tab::Blend, Tab::Cab, Tab::Gate, Tab::Comp}) owner.addChildComponent(page(t));
    tabs.setSelected(0);
    page(Tab::Chain).setVisible(true);
  }
};

RigEditorPanel::RigEditorPanel(SawbladeProcessor& p, RigController& c) : impl_(std::make_unique<Impl>(*this, p, c)) {
  setTitle("Rig editor");
  setOpaque(true);
  impl_->build();
  setSize(kWidth, kHeight);
  refresh();
}

RigEditorPanel::~RigEditorPanel() = default;

void RigEditorPanel::paint(juce::Graphics& g) {
  g.fillAll(L::background());
  g.setColour(L::panel());
  g.fillRect(0, 0, kWidth, 88);
  g.setColour(L::rule());
  g.drawHorizontalLine(88, 0.0f, static_cast<float>(kWidth));
}

void RigEditorPanel::resized() {
  impl_->topology.setBounds(16, 12, 400, 30);
  impl_->status.setBounds(430, 12, kWidth - 430 - 16, 30);
  impl_->tabs.setBounds(16, 50, 560, 30);
  const auto content = juce::Rectangle<int>(0, 96, kWidth, kHeight - 96);
  for (Tab t : {Tab::Chain, Tab::Eq, Tab::Blend, Tab::Cab, Tab::Gate, Tab::Comp}) impl_->page(t).setBounds(content);
}

void RigEditorPanel::setTab(Tab t) {
  impl_->current = t;
  for (int i = 0; i < kNumTabs; ++i) impl_->page(static_cast<Tab>(i)).setVisible(i == static_cast<int>(t));
  impl_->tabs.setSelected(static_cast<int>(t));
  refresh();
}

RigEditorPanel::Tab RigEditorPanel::tab() const noexcept { return impl_->current; }
juce::Button& RigEditorPanel::tabButton(Tab t) { return impl_->tabs.button(static_cast<int>(t)); }
juce::Button& RigEditorPanel::topologyButton(Topology t) { return impl_->topology.button(static_cast<int>(t)); }
juce::Button& RigEditorPanel::eqTargetButton(EqTarget t) { return impl_->eq->target.button(static_cast<int>(t)); }
juce::Button& RigEditorPanel::cabModeButton(CabMode m) { return impl_->cab->mode.button(m == CabMode::Shared ? 0 : 1); }
void RigEditorPanel::setEqTarget(EqTarget t) { impl_->eq->setTarget(t); }
EqGraph& RigEditorPanel::eqGraph() { return impl_->eq->graph; }
juce::String RigEditorPanel::statusText() const { return impl_->status.getText(); }
juce::String RigEditorPanel::eqReadoutText() const { return impl_->eq->readout.getText(); }
RigController& RigEditorPanel::controller() noexcept { return impl_->ctl; }

void RigEditorPanel::refresh() {
  Impl& m = *impl_;
  m.ctl.sync();
  const Preset p = m.ctl.view();
  const auto st = m.proc.status();
  const Topology topo = m.ctl.topology();
  m.topology.setSelected(topo == Topology::Single ? 0 : topo == Topology::SinglePlusTwoPedals ? 1 : 2);

  // status line: a recent message, loading, error, first warning
  const juce::uint32 now = juce::Time::getMillisecondCounter();
  juce::String text;
  juce::Colour colour = L::dimText();
  if (m.transient.isNotEmpty() && static_cast<juce::int32>(m.transientUntil - now) > 0) {
    text = m.transient;
    colour = L::error();
  } else if (st.loading) {
    text = "Loading...";
    colour = L::warning();
  } else if (!st.error.empty()) {
    text = juce::String(st.error);
    colour = L::error();
  } else if (!st.info.warnings.empty()) {
    text = juce::String(st.info.warnings.front());
    colour = L::warning();
  }
  m.status.setText(text, juce::dontSendNotification);
  m.status.setColour(juce::Label::textColourId, colour);

  m.page(m.current).refresh(p, st, topo);
}

}  // namespace sawblade::plugin::rig
