#include "PluginEditor.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "MatchScreen.h"
#include "PlayAlongPanel.h"
#include "about/AboutBox.h"
#include "settings/SettingsPanel.h"
#include "browser/BrowserSettings.h"
#include "browser/CaptureBrowser.h"
#include "pedals/AdvancedDrawer.h"
#include "pedals/PedalFace.h"
#include "rig/RigController.h"
#include "mic/MicPage.h"
#include "presets/AbCompare.h"
#include "presets/PresetBrowser.h"
#include "skin/FilmstripKnob.h"

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;
using skin::FilmstripKnob;
using skin::Piece;

const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");

struct SelectionInfo {
  const char* kind;  // "SAW PEDAL"
  const char* name;  // static block name in this prototype
  juce::uint32 colour;
};
SelectionInfo selectionInfo(Piece p) {
  switch (p) {
    case Piece::SawAmp: return {"SAW AMP", "SAW HEAD", 0xffff6a1a};
    case Piece::BodyAmp: return {"BODY AMP", "BODY HEAD", 0xff4f8fd0};
    case Piece::Cab: return {"SHARED CAB", "4x12 CAB", 0xffe8e1d2};
    case Piece::SawPedal: return {"SAW PEDAL", "STOCKHOLM SYNDROME", 0xffff6a1a};
    case Piece::BodyPedal: return {"BODY PEDAL", "TIGHTEN", 0xff4f8fd0};
  }
  return {"", "", 0xffffffff};
}

const char* kNotAvailable = " (not available in this prototype)";

// Knob placement table: which parameter, panel label, size class, arc colour.
struct KnobDef {
  int param;
  const char* label;
  FilmstripKnob::Kind kind;
  juce::uint32 arc;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// The 1280 x 800 design surface.
class SawbladeEditor::Content : public juce::Component {
 public:
  static constexpr int kTopBar = 58, kRigW = skin::RigView::kWidth, kInspX = kRigW, kInspW = kDesignWidth - kRigW;

  explicit Content(SawbladeProcessor& p) : processor_(p) {
    setOpaque(true);

    // --- top bar
    wordmark_.setText("SAWBLADE", juce::dontSendNotification);
    wordmark_.setFont(L::wordmarkFont());
    wordmark_.setColour(juce::Label::textColourId, L::saw());
    wordmark_.setInterceptsMouseClicks(false, false);
    wordmark_.setTransform(juce::AffineTransform::rotation(-0.026f, 95.0f, 21.0f));
    addAndMakeVisible(wordmark_);

    configure(prev_, juce::String::fromUTF8("\xe2\x80\xb9"), "Previous preset in the browser's list", false);
    configure(next_, juce::String::fromUTF8("\xe2\x80\xba"), "Next preset in the browser's list", false);
    configure(ab_, "A", "A/B compare: switch between two versions of the sound (right-click: copy A to B, B to A, reset)", false);
    ab_.setTitle("A/B compare");
    ab_.onClick = [this] {
      abCompare_.toggle();
      ab_.setButtonText(abCompare_.label());
    };
    ab_.addMouseListener(this, false);
    prev_.onClick = [this] { stepPreset(-1); };
    next_.onClick = [this] { stepPreset(+1); };
    configure(match_, "MATCH", "Match to a reference", true);
    configure(export_, "EXPORT NAM", "Export as NAM model", true);
    configure(playAlong_, "PLAY ALONG", "Show / hide the play-along panel: a backing track from separated stems to play over", false);
    playAlong_.setClickingTogglesState(true);
    playAlong_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff6b2f12));
    playAlong_.onClick = [this] { setPlayAlongOpen(playAlong_.getToggleState()); };
    configure(settingsBtn_, juce::String::fromUTF8("\xe2\x9a\x99"), "Settings: tool paths, TONE3000 login, cache", false);
    settingsBtn_.setTitle("Settings");
    settingsBtn_.setClickingTogglesState(true);
    settingsBtn_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff6b2f12));
    settingsBtn_.onClick = [this] { setSettingsOpen(settingsBtn_.getToggleState()); };
    configure(rigButton_, "RIG", "Show / hide the rig editor: topology, blocks, EQs, blend and alignment, cab, gate and compressor", false);
    rigButton_.setClickingTogglesState(true);
    rigButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff6b2f12));
    rigButton_.onClick = [this] { setRigEditorOpen(rigButton_.getToggleState()); };
    match_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a1a0e));
    match_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffffb27a));
    export_.setColour(juce::TextButton::buttonColourId, L::saw());
    export_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff140a04));

    presetButton_.setTitle("Preset");
    presetButton_.setTooltip("Open the preset browser");
    presetButton_.onClick = [this] { setBrowserOpen(!presetBrowser_->isVisible()); };
    addAndMakeVisible(presetButton_);

    for (juce::Label* c : {&latChip_, &modeChip_}) {
      c->setFont(L::monoFont(12.0f));
      c->setJustificationType(juce::Justification::centred);
      c->setColour(juce::Label::backgroundColourId, juce::Colour(0xff141210));
      c->setColour(juce::Label::outlineColourId, L::chipBorder());
      c->setBorderSize({0, 4, 0, 4});
      c->setMinimumHorizontalScale(0.7f);
      addAndMakeVisible(*c);
    }
    latChip_.setTitle("Latency");
    latChip_.setTooltip("Reported plugin latency (CPU meter: not available in this prototype)");
    modeChip_.setTitle("Blend mode");
    modeChip_.setTooltip("LIVE: both paths share one cab, the no-cab NAM export is exact. STUDIO: per-path cabs, only the with-cab export is exact.");

    // --- rig
    rig_.onSelect = [this](Piece) { updateSelection(); };
    addAndMakeVisible(rig_);
    message_.setFont(L::bodyFont(12.0f));
    message_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(message_);

    // --- inspector
    selKind_.setFont(L::labelFont(11.0f));
    selKind_.setColour(juce::Label::textColourId, L::dimText());
    selName_.setFont(L::titleFont(18.0f));
    blendLabel_.setText("BLEND", juce::dontSendNotification);
    blendLabel_.setFont(L::labelFont(13.0f));
    blendRead_.setFont(L::monoFont(14.0f));
    thr_.setFont(L::monoFont(12.0f));
    thr_.setColour(juce::Label::textColourId, L::dimText());
    thr_.setJustificationType(juce::Justification::centredRight);
    matchTitle_.setText("MATCH vs ORIGINAL", juce::dontSendNotification);
    matchTitle_.setFont(L::labelFont(11.0f));
    matchTitle_.setColour(juce::Label::textColourId, L::dimText());
    matchValue_.setText(juce::String::fromUTF8("\xe2\x80\x94"), juce::dontSendNotification);
    matchValue_.setFont(L::monoFont(13.0f));
    for (juce::Label* l : {&selKind_, &selName_, &blendLabel_, &blendRead_, &thr_, &matchTitle_, &matchValue_}) {
      l->setInterceptsMouseClicks(false, false);
      addAndMakeVisible(*l);
    }
    configure(browse_, "BROWSE CAPTURES", "Browse TONE3000 captures", false);
    browse_.onClick = [this] { openBrowser(); };
    configure(learn_, "LEARN GATE", "Learn the gate threshold: stay silent for 1 s, the threshold is set 6 dB above your noise", false);
    learn_.onClick = [this] {
      rigController_->learnGate();
      learnShownUntil_ = juce::Time::getMillisecondCounter() + 9000;
      updateReadouts();
    };

    auto addKnob = [this](const KnobDef& d) -> FilmstripKnob& {
      const ParamSpec& s = paramSpec(d.param);
      auto k = std::make_unique<FilmstripKnob>(processor_.parameters(), s.id, s.name, d.kind, juce::Colour(d.arc));
      addAndMakeVisible(*k);
      knobs_[static_cast<size_t>(d.param)] = std::move(k);
      return *knobs_[static_cast<size_t>(d.param)];
    };
    addKnob({kBlend, "BLEND", FilmstripKnob::Kind::Amp, 0xffff6a1a}).onValueChange = [this] { updateReadouts(); };
    for (const KnobDef& d : kMaster) addKnob(d);
    for (int k = 0; k < kPostEqSlots; ++k) addKnob({kPostEqFirst + k, nullptr, FilmstripKnob::Kind::Pedal, 0xffff6a1a});
    knobs_[kGateThreshold]->onValueChange = [this] { updateReadouts(); };

    // The live pedal controls: the face over the SAW pedal render, the advanced drawer to its right.
    face_ = std::make_unique<PedalFace>(processor_);
    addChildComponent(*face_);
    drawer_ = std::make_unique<AdvancedDrawer>(processor_);
    addChildComponent(*drawer_);
    auto& sawPedal = rig_.piece(Piece::SawPedal);
    sawPedal.setTooltip(sawPedal.getTitle() + " (click to select, double-click for the advanced controls)");
    sawPedal.onDoubleClick = [this](Piece) { drawer_->toggle(); };

    panel_ = std::make_unique<PlayAlongPanel>(processor_);
    panel_->setVisible(false);
    addChildComponent(*panel_);  // on top of the rig and the inspector

    rigController_ = std::make_unique<rig::RigController>(processor_);
    rigPanel_ = std::make_unique<rig::RigEditorPanel>(processor_, *rigController_);
    rigPanel_->setVisible(false);
    addChildComponent(*rigPanel_);  // last child; opening either overlay brings it to the front
    micPage_ = std::make_unique<MicPage>(processor_);
    micPage_->setVisible(false);
    micPage_->onClose = [this] { setMicPageOpen(false); };
    rig_.onCabOpen = [this] { setMicPageOpen(true); };
    addChildComponent(*micPage_);
    presetBrowser_ = std::make_unique<PresetBrowser>(processor_);
    presetBrowser_->setVisible(false);
    presetBrowser_->onClose = [this] { setBrowserOpen(false); };
    presetBrowser_->onLoadFile = [this] { chooseFile(); };
    addChildComponent(*presetBrowser_);  // last child: on top of everything below the top bar
    screen_ = std::make_unique<MatchScreen>(processor_);
    addChildComponent(*screen_);  // last child: covers everything below the top bar
    panel_->onMatch = [this] { openMatchScreen(false); };
    panel_->onExport = [this] { openMatchScreen(true); };

    settingsPanel_ = std::make_unique<settings::SettingsPanel>(processor_, settings::Settings::shared());
    settingsPanel_->setVisible(false);
    settingsPanel_->onClosed = [this] { settingsBtn_.setToggleState(false, juce::dontSendNotification); };
    settingsPanel_->onAbout = [this] { about::AboutBox::show(*this, about_, processor_, settings::Settings::shared()); };
    addChildComponent(*settingsPanel_);  // on top of the play-along panel

    setSize(kDesignWidth, kDesignHeight);  // lays everything out (resized() needs all children to exist)
    updateSelection();
    refresh();
    if (settings::Settings::shared().isFirstRun() && settings::SettingsPanel::claimFirstRunShow()) setSettingsOpen(true, true);
  }

  void paint(juce::Graphics& g) override {
    g.fillAll(L::background());
    // top bar
    g.setColour(L::panel());
    g.fillRect(0, 0, kDesignWidth, kTopBar);
    g.setColour(L::rule());
    g.drawHorizontalLine(kTopBar - 1, 0.0f, static_cast<float>(kDesignWidth));
    // inspector
    g.setColour(L::panel());
    g.fillRect(kInspX, kTopBar, kInspW, kDesignHeight - kTopBar);
    g.setColour(L::rule());
    g.drawVerticalLine(kInspX, static_cast<float>(kTopBar), static_cast<float>(kDesignHeight));
    g.fillRect(kInspX + 16, kTopBar + 106 + 36, kInspW - 32, 1);  // divider under BROWSE CAPTURES

    g.setColour(L::dimText());
    g.setFont(L::labelFont(11.0f));
    g.drawText("MASTER", kInspX + 16, kMasterY - 22, 120, 14, juce::Justification::centredLeft);
    g.drawText("POST EQ", kInspX + 16, kEqY - 22, 120, 14, juce::Justification::centredLeft);
    g.setColour(L::text());
    g.setFont(L::labelFont(11.0f));
    for (const KnobDef& d : kMaster) drawLabel(g, *knobs_[static_cast<size_t>(d.param)], d.label);
    for (int k = 0; k < kPostEqSlots; ++k)
      drawLabel(g, *knobs_[static_cast<size_t>(kPostEqFirst + k)], ("EQ" + std::to_string(k + 1)).c_str());

    // match box
    g.setColour(L::panelDeep());
    g.fillRoundedRectangle(matchBox().toFloat(), 6.0f);
  }

  void resized() override {
    // Top bar: left to right wordmark, preset selector, A/B; right to left EXPORT, MATCH, mode chip, latency chip.
    constexpr int y = 12, h = 34;
    wordmark_.setBounds(18, 8, 190, 42);
    int x = 226;
    prev_.setBounds(x, y, 34, h);
    presetButton_.setBounds(x + 34, y, 170, h);
    next_.setBounds(x + 34 + 170, y, 34, h);
    x += 34 + 170 + 34 + 12;
    ab_.setBounds(x, y, 52, h);
    playAlong_.setBounds(x + 52 + 12, y, 104, h);
    rigButton_.setBounds(x + 52 + 12 + 104 + 12, y, 64, h);
    settingsBtn_.setBounds(x + 52 + 12 + 104 + 12 + 64 + 12, y, 34, h);
    int r = kDesignWidth - 18;
    export_.setBounds(r - 130, y, 130, h);
    r -= 130 + 12;
    match_.setBounds(r - 90, y, 90, h);
    r -= 90 + 12;
    modeChip_.setBounds(r - 96, y + 2, 96, 30);
    r -= 96 + 12;
    latChip_.setBounds(r - 136, y + 2, 136, 30);

    rig_.setBounds(0, kTopBar, kRigW, skin::RigView::kHeight);
    {
      const auto pedal = rig_.piece(Piece::SawPedal).getBounds() + rig_.getPosition();
      face_->setBounds(pedal);
      drawer_->setAnchor(pedal, rig_.getBounds().withTrimmedRight(24));
    }
    panel_->setBounds(0, kDesignHeight - PlayAlongPanel::kHeight, PlayAlongPanel::kWidth, PlayAlongPanel::kHeight);
    screen_->setBounds(0, kTopBar, MatchScreen::kWidth, kDesignHeight - kTopBar);
    settingsPanel_->setBounds(0, kTopBar, settings::SettingsPanel::kWidth, settings::SettingsPanel::kHeight);
    message_.setBounds(34, kTopBar + 14, 860, 20);
    rigPanel_->setBounds(0, kTopBar, rig::RigEditorPanel::kWidth, rig::RigEditorPanel::kHeight);
    micPage_->setBounds(0, kTopBar, MicPage::kWidth, MicPage::kHeight);
    presetBrowser_->setBounds(0, kTopBar, PresetBrowser::kWidth, PresetBrowser::kHeight);

    const int ix = kInspX + 16, iw = kInspW - 32;
    selKind_.setBounds(ix, kTopBar + 14, iw, 16);
    selName_.setBounds(ix, kTopBar + 34, iw, 26);
    browse_.setBounds(ix, kTopBar + 106, iw, 30);

    knobs_[kBlend]->setBounds(ix, kTopBar + 156, 88, 88);
    blendLabel_.setBounds(ix + 102, kTopBar + 178, 150, 18);
    blendRead_.setBounds(ix + 102, kTopBar + 200, 210, 20);

    placeRow(kMaster.size(), kMasterY, 44, [this](size_t i) -> FilmstripKnob& { return *knobs_[static_cast<size_t>(kMaster[i].param)]; });
    placeRow(kPostEqSlots, kEqY, 36, [this](size_t i) -> FilmstripKnob& { return *knobs_[static_cast<size_t>(kPostEqFirst) + i]; });

    learn_.setBounds(ix, kLearnY, 110, 28);
    thr_.setBounds(ix + 120, kLearnY, iw - 120, 28);
    matchTitle_.setBounds(matchBox().getX() + 10, matchBox().getY() + 8, matchBox().getWidth() - 20, 16);
    matchValue_.setBounds(matchBox().getX() + 10, matchBox().getY() + 28, matchBox().getWidth() - 20, 20);
  }

  void refresh() {
    const auto st = processor_.status();
    presetButton_.setButtonText(juce::String(st.presetName).toUpperCase());
    latChip_.setText("LAT " + juce::String(st.latencySamples) + juce::String::fromUTF8(" smp \xc2\xb7 CPU \xe2\x80\x94"), juce::dontSendNotification);
    modeChip_.setText(st.liveCompatible ? juce::String::fromUTF8("\xe2\x97\x8f LIVE") : juce::String::fromUTF8("\xe2\x97\x8f STUDIO"), juce::dontSendNotification);
    modeChip_.setColour(juce::Label::textColourId, st.liveCompatible ? L::live() : L::studio());
    modeChip_.setColour(juce::Label::outlineColourId, st.liveCompatible ? L::liveBorder() : L::studio().withAlpha(0.45f));

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

    // Single topologies: path B is off, so its level and the blend are not editable (spec 4.1).
    const bool blendOn = rig::topologyOf(processor_.editBasePreset()) == rig::Topology::Blend;
    knobs_[kBlend]->setEnabled(blendOn);
    knobs_[kLevelB]->setEnabled(blendOn);

    const SlotBands bands = processor_.postEqSlots();
    for (int k = 0; k < kPostEqSlots; ++k) knobs_[static_cast<size_t>(kPostEqFirst + k)]->setEnabled(bands[static_cast<size_t>(k)] >= 0);
    updateReadouts();
    face_->refresh();
    if (drawer_->isVisible()) drawer_->refresh();
    if (settingsPanel_ && settingsPanel_->isVisible()) settingsPanel_->refresh();
  }

  void updateReadouts() {
    const double b = knobs_[kBlend]->getValue();
    const bool showLearn = rigController_ && (rigController_->learning() ||
                                              (rigController_->learnStatus().size() > 0 && static_cast<juce::int32>(learnShownUntil_ - juce::Time::getMillisecondCounter()) > 0));
    blendRead_.setText("SAW " + juce::String(juce::roundToInt((1.0 - b) * 100.0)) + " / BODY " + juce::String(juce::roundToInt(b * 100.0)),
                       juce::dontSendNotification);
    thr_.setText(showLearn ? juce::String(rigController_->learnStatus())
                           : "thr " + juce::String(knobs_[kGateThreshold]->getValue(), 1) + " dB",
                 juce::dontSendNotification);
  }

  skin::RigView& rig() { return rig_; }

  void setPlayAlongOpen(bool open) {
    panel_->setVisible(open);
    if (open) {
      panel_->refresh();
      panel_->toFront(false);
    }
    playAlong_.setToggleState(open, juce::dontSendNotification);
  }
  bool playAlongOpen() const { return panel_->isVisible(); }
  void setSettingsOpen(bool open, bool firstRun = false) {
    if (open) settingsPanel_->open(firstRun);
    else settingsPanel_->close();
    settingsBtn_.setToggleState(open, juce::dontSendNotification);
  }
  bool settingsOpen() const { return settingsPanel_->isVisible(); }
  bool aboutOpen() const { return about_ != nullptr && about_->isVisible(); }

  // The capture browser overlay for the selected piece (closed with its "< RIG" button).
  void openBrowser() {
    if (browser_ != nullptr) return;
    static constexpr Slot kSlots[] = {Slot::SawAmp, Slot::BodyAmp, Slot::Cab, Slot::SawPedal, Slot::BodyPedal};  // Piece order
    if (!browserSettings_) browserSettings_ = std::make_unique<BrowserSettings>();
    browser_ = std::make_unique<CaptureBrowser>(processor_, *browserSettings_, kSlots[static_cast<size_t>(rig_.selected())]);
    browser_->onClose = [this] {
      browser_->setVisible(false);
      juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Content>(this)] {
        if (safe != nullptr) safe->browser_.reset();
      });
    };
    addAndMakeVisible(*browser_);
    browser_->setBounds(0, 0, kDesignWidth, kDesignHeight);
  }
  void setRigEditorOpen(bool open) {
    rigPanel_->setVisible(open);
    if (open) {
      rigPanel_->refresh();
      rigPanel_->toFront(false);
    }
    rigButton_.setToggleState(open, juce::dontSendNotification);
  }
  bool rigEditorOpen() const { return rigPanel_->isVisible(); }
  rig::RigEditorPanel& rigEditor() { return *rigPanel_; }
  void refreshPanel() {
    if (panel_->isVisible()) panel_->refresh();
    if (rigPanel_->isVisible()) rigPanel_->refresh();
  }
  void setBrowserOpen(bool open) {
    if (open) micPage_->setVisible(false);
    presetBrowser_->setVisible(open);
    if (open) {
      presetBrowser_->toFront(false);
      presetBrowser_->open();
    }
  }
  bool browserOpen() const { return presetBrowser_->isVisible(); }
  PresetBrowser& browser() { return *presetBrowser_; }
  AbCompare& abCompare() { return abCompare_; }
  void stepPreset(int dir) {
    if (presetBrowser_->library().entries().empty()) presetBrowser_->scanBlocking();
    presetBrowser_->step(dir);
  }
  void mouseDown(const juce::MouseEvent& e) override {
    if (e.eventComponent != &ab_ || !e.mods.isPopupMenu()) return;
    juce::PopupMenu m;
    m.addItem(1, juce::String::fromUTF8("Copy A \xe2\x86\x92 B"));
    m.addItem(2, juce::String::fromUTF8("Copy B \xe2\x86\x92 A"));
    m.addItem(3, "Reset compare");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&ab_), [this](int r) {
      if (r == 1) abCompare_.copyAToB();
      else if (r == 2) abCompare_.copyBToA();
      else if (r == 3) abCompare_.reset();
      ab_.setButtonText(abCompare_.label());
    });
  }
  void setMicPageOpen(bool open) {
    if (open) presetBrowser_->setVisible(false);
    micPage_->setVisible(open);
    if (open) {
      micPage_->toFront(false);
      micPage_->open();
    }
  }
  bool micPageOpen() const { return micPage_->isVisible(); }
  MicPage& micPage() { return *micPage_; }
  void refreshMicPage() {
    if (micPage_->isVisible()) micPage_->refresh();
  }
  void openMatchScreen(bool exportMode) {
    screen_->open(exportMode ? MatchScreen::Mode::Export : MatchScreen::Mode::Match);
    screen_->toFront(false);  // above an open rig editor / mic page / preset browser overlay
  }
  bool matchScreenOpen() const { return screen_->isVisible(); }
  void refreshScreen() { screen_->refresh(); }

 private:
  static juce::Rectangle<int> matchBox() { return {kInspX + 16, kDesignHeight - 14 - 64, kInspW - 32, 64}; }
  static constexpr int kMasterY = kTopBar + 328, kEqY = kTopBar + 424, kLearnY = kTopBar + 500;
  static constexpr std::array<KnobDef, 5> kMaster{{{kInputGain, "INPUT", FilmstripKnob::Kind::Pedal, 0xffff6a1a},
                                                   {kGateThreshold, "GATE", FilmstripKnob::Kind::Pedal, 0xffff6a1a},
                                                   {kLevelA, "SAW", FilmstripKnob::Kind::Pedal, 0xffff6a1a},
                                                   {kLevelB, "BODY", FilmstripKnob::Kind::Pedal, 0xff4f8fd0},
                                                   {kOutputGain, "OUTPUT", FilmstripKnob::Kind::Pedal, 0xffe8e1d2}}};

  void configure(juce::TextButton& b, const juce::String& text, const juce::String& tip, bool placeholder) {
    b.setButtonText(text);
    b.setTitle(text);
    b.setTooltip(tip + (placeholder ? kNotAvailable : ""));
    b.setEnabled(!placeholder);
    addAndMakeVisible(b);
  }

  template <class Get>
  void placeRow(size_t n, int y, int size, Get get) {
    const int ix = kInspX + 16, iw = kInspW - 32;
    const float cell = static_cast<float>(iw) / static_cast<float>(n);
    for (size_t i = 0; i < n; ++i) {
      const int cx = ix + juce::roundToInt(cell * (static_cast<float>(i) + 0.5f));
      get(i).setBounds(cx - size / 2, y, size, size);
    }
  }

  static void drawLabel(juce::Graphics& g, const juce::Component& knob, const char* text) {
    g.setColour(knob.isEnabled() ? L::text() : L::dimText().withAlpha(0.5f));
    g.drawText(text, knob.getX() - 14, knob.getBottom() + 3, knob.getWidth() + 28, 14, juce::Justification::centred);
  }

  void updateSelection() {
    const auto s = selectionInfo(rig_.selected());
    selKind_.setText("SELECTED" + kDot + s.kind, juce::dontSendNotification);
    selName_.setText(s.name, juce::dontSendNotification);
    selName_.setColour(juce::Label::textColourId, juce::Colour(s.colour));
  }

  void chooseFile() {
    chooser_ = std::make_unique<juce::FileChooser>("Load a Sawblade preset", juce::File(), "*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
      const juce::File f = fc.getResult();
      if (f == juce::File()) return;
      processor_.loadPresetFile(std::filesystem::path(f.getFullPathName().toStdString()));
    });
  }

  SawbladeProcessor& processor_;
  AbCompare abCompare_{processor_};
  juce::Label wordmark_, latChip_, modeChip_, message_;
  juce::Label selKind_, selName_, blendLabel_, blendRead_, thr_, matchTitle_, matchValue_;
  juce::TextButton prev_, next_, ab_, match_, export_, presetButton_, browse_, learn_, playAlong_, rigButton_, settingsBtn_;
  juce::uint32 learnShownUntil_ = 0;
  std::unique_ptr<PedalFace> face_;
  std::unique_ptr<AdvancedDrawer> drawer_;
  std::unique_ptr<PlayAlongPanel> panel_;
  std::unique_ptr<BrowserSettings> browserSettings_;
  std::unique_ptr<CaptureBrowser> browser_;  // declared after the settings it uses
  std::unique_ptr<rig::RigController> rigController_;  // before the panel that uses it
  std::unique_ptr<rig::RigEditorPanel> rigPanel_;
  std::unique_ptr<MicPage> micPage_;
  std::unique_ptr<PresetBrowser> presetBrowser_;
  std::unique_ptr<MatchScreen> screen_;
  std::unique_ptr<settings::SettingsPanel> settingsPanel_;
  std::unique_ptr<about::AboutBox> about_;
  skin::RigView rig_;
  std::array<std::unique_ptr<FilmstripKnob>, kNumParams> knobs_;
  std::unique_ptr<juce::FileChooser> chooser_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Content)
};

// ---------------------------------------------------------------------------------------------
SawbladeEditor::SawbladeEditor(SawbladeProcessor& p) : juce::AudioProcessorEditor(p), processor_(p) {
  setLookAndFeel(&laf_);
  content_ = std::make_unique<Content>(p);
  addAndMakeVisible(*content_);

  setResizable(true, true);
  setResizeLimits(640, 400, 2560, 1600);
  getConstrainer()->setFixedAspectRatio(static_cast<double>(kDesignWidth) / kDesignHeight);
  const double uiScale = settings::Settings::shared().uiScale();
  setSize(juce::roundToInt(kDesignWidth * uiScale), juce::roundToInt(kDesignHeight * uiScale));
  startTimerHz(16);
}

SawbladeEditor::~SawbladeEditor() {
  stopTimer();
  setLookAndFeel(nullptr);
}

void SawbladeEditor::paint(juce::Graphics& g) { g.fillAll(L::background()); }

void SawbladeEditor::resized() {
  if (content_ == nullptr) return;
  content_->setBounds(0, 0, kDesignWidth, kDesignHeight);
  content_->setTransform(juce::AffineTransform::scale(static_cast<float>(contentScale())));
}

double SawbladeEditor::contentScale() const { return static_cast<double>(getWidth()) / kDesignWidth; }

skin::Piece SawbladeEditor::selectedPiece() const { return content_->rig().selected(); }

void SawbladeEditor::timerCallback() {
  if ((tick_++ & 3) == 0) {  // 4 Hz; the open play-along panel refreshes at the full rate
    content_->refresh();
    content_->refreshMicPage();
    content_->refreshScreen();
  }
  content_->refreshPanel();
}

void SawbladeEditor::setPlayAlongOpen(bool open) { content_->setPlayAlongOpen(open); }
void SawbladeEditor::setRigEditorOpen(bool open) { content_->setRigEditorOpen(open); }
bool SawbladeEditor::rigEditorOpen() const { return content_->rigEditorOpen(); }
rig::RigEditorPanel& SawbladeEditor::rigEditor() { return content_->rigEditor(); }
bool SawbladeEditor::playAlongOpen() const { return content_->playAlongOpen(); }
void SawbladeEditor::setSettingsOpen(bool open) { content_->setSettingsOpen(open); }
bool SawbladeEditor::settingsOpen() const { return content_->settingsOpen(); }
bool SawbladeEditor::aboutOpen() const { return content_->aboutOpen(); }
void SawbladeEditor::setMicPageOpen(bool open) { content_->setMicPageOpen(open); }
bool SawbladeEditor::micPageOpen() const { return content_->micPageOpen(); }
MicPage& SawbladeEditor::micPage() { return content_->micPage(); }
void SawbladeEditor::setBrowserOpen(bool open) { content_->setBrowserOpen(open); }
bool SawbladeEditor::browserOpen() const { return content_->browserOpen(); }
PresetBrowser& SawbladeEditor::browser() { return content_->browser(); }
AbCompare& SawbladeEditor::abCompare() { return content_->abCompare(); }
void SawbladeEditor::openMatchScreen(bool exportMode) {
  content_->openMatchScreen(exportMode);
}
bool SawbladeEditor::matchScreenOpen() const { return content_->matchScreenOpen(); }

bool SawbladeEditor::isInterestedInFileDrag(const juce::StringArray& files) {
  for (const auto& f : files)
    if (juce::File(f).isDirectory() || isSongFileName(f.toStdString())) return true;
  return false;
}

void SawbladeEditor::filesDropped(const juce::StringArray& files, int, int) {
  for (const auto& f : files) {
    if (!juce::File(f).isDirectory() && !isSongFileName(f.toStdString())) continue;
    processor_.playAlong().loadSong(f.toStdString(), /*userInitiated=*/true);
    setPlayAlongOpen(true);
    return;
  }
}

}  // namespace sawblade::plugin
