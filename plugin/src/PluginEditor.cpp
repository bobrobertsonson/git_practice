#include "PluginEditor.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "ExportPanel.h"
#include "MatchScreen.h"
#include "PlayAlongPanel.h"
#include "about/AboutBox.h"
#include "settings/SettingsPanel.h"
#include "browser/BrowserSettings.h"
#include "browser/CaptureBrowser.h"
#include "pedals/AdvancedDrawer.h"
#include "pedals/PedalFace.h"
#include "rig/AmpHead.h"
#include "rig/CabScreen.h"
#include "rig/Pedalboard.h"
#include "rig/RigController.h"
#include "rig/RigModel.h"
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
  const char* name;  // the amp heads' name; a pedal's name is its tile's (the pedalboard)
  juce::uint32 colour;
};
SelectionInfo selectionInfo(Piece p) {
  switch (p) {
    case Piece::SawAmp: return {"SAW AMP", "SAW HEAD", 0xffff6a1a};
    case Piece::BodyAmp: return {"BODY AMP", "BODY HEAD", 0xff4f8fd0};
    case Piece::SawPedal: return {"SAW PEDAL", "", 0xffff6a1a};
    case Piece::BodyPedal: return {"BODY PEDAL", "", 0xff4f8fd0};
  }
  return {"", "", 0xffffffff};
}

// The cab chip's name: the IR of a shared cab, "A + B" (or one name when both are the same IR) of the per-path and mixed cabs.
juce::String cabChipName(const CabPreset& c) {
  if (c.mode == CabMode::Shared) return juce::String(rig::captureTitle(c.ir));
  const juce::String a = juce::String(rig::captureTitle(c.irA)), b = juce::String(rig::captureTitle(c.irB));
  return a == b ? a : a + " + " + b;
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
    configure(match_, "MATCH", "Open the MATCH screen: find the blend that sounds like the loaded song, from a DI take", false);
    match_.onClick = [this] { openMatchScreen(); };
    configure(export_, "EXPORT NAM", "Train a NAM model of this rig for a loader pedal", false);
    export_.onClick = [this] { openExportPanel(); };
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
    configure(cabButton_, "CAB", "Show / hide the CAB page: the cab's impulse response(s), browse IRs, mic positions", false);
    cabButton_.setTitle("Cab page");
    cabButton_.setClickingTogglesState(true);
    cabButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff6b2f12));
    cabButton_.onClick = [this] { setCabPageOpen(cabButton_.getToggleState()); };
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
    rig_.cabChip().onClick = [this] { setCabPageOpen(true); };
    addAndMakeVisible(rig_);
    // The pedalboards (v0.4 Task D) lie over the rig view, under the amp controls, the pedal face and the drawer.
    rigController_ = std::make_unique<rig::RigController>(processor_);
    board_ = std::make_unique<rig::Pedalboard>(*rigController_);
    board_->onSelect = [this](rig::BoardTile& t) { rig_.select(t.path() == 0 ? Piece::SawPedal : Piece::BodyPedal, t.blockId()); };
    board_->onTileDoubleClick = [this](rig::BoardTile& t) {
      if (isFaceTile(t)) drawer_->toggle();  // the advanced drawer belongs to the pedal that carries the live face
    };
    board_->onTilesChanged = [this] {
      placeFace();
      updateSelection();
    };
    board_->onScrolled = [this] {  // the live face follows its tile, or hides while the tile is scrolled out of view
      placeFace();
      if (face_) face_->refresh();
    };
    board_->onMessage = [this](const juce::String& m) { showMessage(m); };
    board_->onSearchRequest = [this](int path, int index) {  // "SEARCH TONE3000...": the capture browser in insert mode (pedals, USE adds a block)
      openBrowserFor(path == 0 ? Slot::SawPedal : Slot::BodyPedal, {}, InsertPoint{path == 0 ? 'a' : 'b', index});
    };
    addAndMakeVisible(*board_);
    for (int path = 0; path < 2; ++path) {  // the amp controls (v0.2 Task D) lie over the amp heads' art
      ampHeads_[static_cast<size_t>(path)] = std::make_unique<rig::AmpHead>(processor_, path);
      addAndMakeVisible(*ampHeads_[static_cast<size_t>(path)]);
    }
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
    FilmstripKnob& blendKnob = addKnob({kBlend, "BLEND", FilmstripKnob::Kind::Amp, 0xffff6a1a});
    blendKnob.onValueChange = [this] { updateReadouts(); };
    // v0.3 Task C: only a USER gesture on the knob (mouse down .. up, the wheel) turns a single-path rig into a blend. The knob's drag
    // start / end are fired by the Slider for user input only: host automation moves the value through the parameter attachment, which
    // never starts a drag, so it cannot change the topology. The switch happens at the drag end: the value has settled, so the rebuild
    // that follows writes back the same BLEND value and cannot snap the knob while it is being dragged.
    // v0.3 Task D: the knob's own parameter gesture ends BEFORE this runs, so the drag and the fill it triggers are held together in
    // one history gesture (opened here, after the parameter gesture's start, closed after blendTurnedUp): one undo step restores the
    // knob and the SAW-only rig together.
    blendKnob.onDragStart = [this] {
      blendGesture_ = true;
      blendBefore_ = knobs_[kBlend]->getValue();
      if (rigController_) rigController_->beginGesture();
    };
    blendKnob.onDragEnd = [this] {
      if (!blendGesture_) return;
      blendGesture_ = false;
      if (rigController_ && knobs_[kBlend]->getValue() > 0.0) rigController_->blendTurnedUp(blendBefore_);
      if (rigController_) rigController_->endGesture();
    };
    for (const KnobDef& d : kMaster) addKnob(d);
    for (int k = 0; k < kPostEqSlots; ++k) addKnob({kPostEqFirst + k, nullptr, FilmstripKnob::Kind::Pedal, 0xffff6a1a});
    knobs_[kGateThreshold]->onValueChange = [this] { updateReadouts(); };

    // The live pedal controls: the face over the pedalboard tile of the first circuit block, the advanced drawer beside it.
    face_ = std::make_unique<PedalFace>(processor_);
    face_->setHostProbe([this](const CircuitSlot& slot) { return circuitHosted(slot); });
    addChildComponent(*face_);
    drawer_ = std::make_unique<AdvancedDrawer>(processor_);
    addChildComponent(*drawer_);

    panel_ = std::make_unique<PlayAlongPanel>(processor_);
    panel_->setVisible(false);
    addChildComponent(*panel_);  // on top of the rig and the inspector

    rigPanel_ = std::make_unique<rig::RigEditorPanel>(processor_, *rigController_);
    rigPanel_->setVisible(false);
    addChildComponent(*rigPanel_);  // last child; opening either overlay brings it to the front
    micPage_ = std::make_unique<MicPage>(processor_);
    micPage_->setVisible(false);
    micPage_->onClose = [this] {
      const bool backToCab = micFromCab_;  // opened by MIC POSITIONS: closing comes back to the CAB page
      setMicPageOpen(false);
      if (backToCab) setCabPageOpen(true);
    };
    addChildComponent(*micPage_);
    cabScreen_ = std::make_unique<rig::CabScreen>(processor_, *rigController_);
    cabScreen_->setVisible(false);
    cabScreen_->onClose = [this] { setCabPageOpen(false); };
    cabScreen_->onBrowseIr = [this] { openCabBrowser(); };
    cabScreen_->onMicPositions = [this] { openMicFromCab(); };
    addChildComponent(*cabScreen_);
    presetBrowser_ = std::make_unique<PresetBrowser>(processor_);
    presetBrowser_->setVisible(false);
    presetBrowser_->onClose = [this] { setBrowserOpen(false); };
    presetBrowser_->onLoadFile = [this] { chooseFile(); };
    addChildComponent(*presetBrowser_);  // last child: on top of everything below the top bar
    screen_ = std::make_unique<MatchScreen>(processor_);
    addChildComponent(*screen_);  // last child: covers everything below the top bar
    exportPanel_ = std::make_unique<ExportPanel>(processor_);
    addChildComponent(*exportPanel_);  // last child: covers everything below the top bar
    panel_->onMatch = [this] { openMatchScreen(); };
    panel_->onExport = [this] { openExportPanel(); };

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
    // Top bar: left to right wordmark, preset selector, A/B, PLAY ALONG, RIG, CAB, settings; right to left EXPORT, MATCH, mode chip,
    // latency chip.
    constexpr int y = 12, h = 34;
    wordmark_.setBounds(18, 8, 190, 42);
    int x = 210;
    prev_.setBounds(x, y, 34, h);
    presetButton_.setBounds(x + 34, y, 128, h);
    next_.setBounds(x + 34 + 128, y, 34, h);
    x += 34 + 128 + 34 + 12;
    ab_.setBounds(x, y, 52, h);
    playAlong_.setBounds(x + 52 + 12, y, 98, h);
    rigButton_.setBounds(x + 52 + 12 + 98 + 12, y, 64, h);
    cabButton_.setBounds(x + 52 + 12 + 98 + 12 + 64 + 12, y, 64, h);
    settingsBtn_.setBounds(x + 52 + 12 + 98 + 12 + 64 + 12 + 64 + 12, y, 34, h);
    int r = kDesignWidth - 18;
    export_.setBounds(r - 130, y, 130, h);
    r -= 130 + 12;
    match_.setBounds(r - 84, y, 84, h);
    r -= 84 + 12;
    modeChip_.setBounds(r - 88, y + 2, 88, 30);
    r -= 88 + 12;
    latChip_.setBounds(r - 136, y + 2, 136, 30);

    rig_.setBounds(0, kTopBar, kRigW, skin::RigView::kHeight);
    board_->setBounds(rig_.getBounds());
    ampHeads_[0]->setBounds(rig_.head(0).getBounds() + rig_.getPosition());
    ampHeads_[1]->setBounds(rig_.head(1).getBounds() + rig_.getPosition());
    placeFace();
    panel_->setBounds(0, kDesignHeight - PlayAlongPanel::kHeight, PlayAlongPanel::kWidth, PlayAlongPanel::kHeight);
    screen_->setBounds(0, kTopBar, MatchScreen::kWidth, kDesignHeight - kTopBar);
    exportPanel_->setBounds(0, kTopBar, ExportPanel::kWidth, kDesignHeight - kTopBar);
    settingsPanel_->setBounds(0, kTopBar, settings::SettingsPanel::kWidth, settings::SettingsPanel::kHeight);
    message_.setBounds(34, kTopBar + 14, 860, 20);
    rigPanel_->setBounds(0, kTopBar, rig::RigEditorPanel::kWidth, rig::RigEditorPanel::kHeight);
    cabScreen_->setBounds(0, kTopBar, rig::CabScreen::kWidth, rig::CabScreen::kHeight);
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
    if (rigController_) rigController_->sync();  // drives the BLEND fill (BodyFill::tick) whether or not the rig editor is open
    const auto st = processor_.status();
    presetButton_.setButtonText(juce::String(st.presetName).toUpperCase());
    // LEVEL MATCH (v0.3): while the trim of this rig is being measured (background, a few seconds) the chip says so; the trim is
    // 0 (or the previous one) until then.
    const bool levelPending = st.levelMatchOn && st.levelPending;
    latChip_.setText(levelPending ? juce::String::fromUTF8("LEVEL \xe2\x80\xa6")
                                  : "LAT " + juce::String(st.latencySamples) + juce::String::fromUTF8(" smp \xc2\xb7 CPU \xe2\x80\x94"),
                     juce::dontSendNotification);
    latChip_.setTooltip(levelPending ? "Matching this rig to -18 LUFS on a built-in reference signal (LEVEL MATCH, background); until then no trim is applied. "
                                       "Reported plugin latency: " + juce::String(st.latencySamples) + " samples"
                                     : juce::String("Reported plugin latency (CPU meter: not available in this prototype)"));
    modeChip_.setText(st.liveCompatible ? juce::String::fromUTF8("\xe2\x97\x8f LIVE") : juce::String::fromUTF8("\xe2\x97\x8f STUDIO"), juce::dontSendNotification);
    modeChip_.setColour(juce::Label::textColourId, st.liveCompatible ? L::live() : L::studio());
    modeChip_.setColour(juce::Label::outlineColourId, st.liveCompatible ? L::liveBorder() : L::studio().withAlpha(0.45f));

    if (transient_.isNotEmpty() && static_cast<juce::int32>(transientUntil_ - juce::Time::getMillisecondCounter()) > 0) {
      message_.setColour(juce::Label::textColourId, L::warning());
      message_.setText(transient_, juce::dontSendNotification);
    } else if (st.loading) {
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
    const Preset shown = processor_.editBasePreset();
    const bool blendOn = rig::topologyOf(shown) == rig::Topology::Blend;
    knobs_[kBlend]->setEnabled(true);  // always: turning it up from full SAW is what enables the blend topology (RigController::blendTurnedUp)
    knobs_[kLevelB]->setEnabled(blendOn);

    const SlotBands bands = processor_.postEqSlots();
    for (int k = 0; k < kPostEqSlots; ++k) knobs_[static_cast<size_t>(kPostEqFirst + k)]->setEnabled(bands[static_cast<size_t>(k)] >= 0);
    updateReadouts();
    {
      const rig::FillStatus fill = rigController_ ? rigController_->bodyFill().status() : rig::FillStatus{};
      for (int path = 0; path < 2; ++path) ampHeads_[static_cast<size_t>(path)]->refresh(shown, processor_.ladderInfo(path), fill);
    }
    // Path B off: the BODY head and cable are dimmed (its controls are disabled by AmpHead), its board is empty and says so.
    const bool bodyOff = !shown.b.enabled;
    rig_.setBodyOff(bodyOff);
    ampHeads_[1]->setAlpha(bodyOff ? skin::RigView::kOffAlpha : 1.0f);
    board_->refresh(shown);
    placeFace();
    updateSelection();
    {
      using Mode = skin::CabChip::Mode;
      rig_.cabChip().set(shown.cab.enabled ? cabChipName(shown.cab) : juce::String(), !shown.cab.enabled ? Mode::Off : st.liveCompatible ? Mode::Live : Mode::Studio);
    }
    if (cabScreen_->isVisible()) cabScreen_->refresh();
    face_->refresh();
    if (drawer_->isVisible()) drawer_->refresh();
    if (settingsPanel_ && settingsPanel_->isVisible()) settingsPanel_->refresh();
  }

  // A short status-line message from the pedalboard ("SAW path full: 8 blocks"); it stays a few seconds.
  void showMessage(const juce::String& m) {
    transient_ = m;
    transientUntil_ = juce::Time::getMillisecondCounter() + 6000;
    message_.setColour(juce::Label::textColourId, L::warning());
    message_.setText(m, juce::dontSendNotification);
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
    if (open) {
      settingsPanel_->open(firstRun);
      settingsPanel_->toFront(true);  // above the RIG / mic / browser overlays
    }
    else settingsPanel_->close();
    settingsBtn_.setToggleState(open, juce::dontSendNotification);
  }
  bool settingsOpen() const { return settingsPanel_->isVisible(); }
  bool aboutOpen() const { return about_ != nullptr && about_->isVisible(); }

  // The capture browser overlay for the selected piece (closed with its "< RIG" button). For a pedal it targets exactly the block of the
  // tile the inspector shows (the picked one, else the path's first tile): a modeled circuit has no capture to replace and says so.
  void openBrowser() {
    static constexpr Slot kSlots[] = {Slot::SawAmp, Slot::BodyAmp, Slot::SawPedal, Slot::BodyPedal};  // Piece order
    const Piece p = rig_.selected();
    std::string pin;
    if (p == Piece::SawPedal || p == Piece::BodyPedal)
      if (const rig::BoardTile* t = board_->selectedTile()) pin = t->blockId();
    openBrowserFor(kSlots[static_cast<size_t>(p)], pin);
  }
  // The capture browser for the cab's IR (the CAB page's BROWSE IR).
  void openCabBrowser() { openBrowserFor(Slot::Cab, {}); }
  void openBrowserFor(Slot slot, const std::string& pinnedBlockId, std::optional<InsertPoint> insert = std::nullopt) {
    if (browser_ != nullptr) return;
    closeOverlaysExcept(Overlay::None);  // the capture browser covers the whole editor: nothing stays open under it
    if (!browserSettings_) browserSettings_ = std::make_unique<BrowserSettings>();
    browser_ = std::make_unique<CaptureBrowser>(processor_, *browserSettings_, slot, pinnedBlockId, insert);
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
    if (open) closeOverlaysExcept(Overlay::Rig);
    rigPanel_->setVisible(open);
    if (open) {
      rigPanel_->refresh();
      rigPanel_->toFront(false);
    }
    rigButton_.setToggleState(open, juce::dontSendNotification);
  }
  bool rigEditorOpen() const { return rigPanel_->isVisible(); }
  rig::RigEditorPanel& rigEditor() { return *rigPanel_; }
  // The CAB page (v0.4 Task D): in the mutually exclusive overlay group; UI state, never saved.
  void setCabPageOpen(bool open) {
    if (open) closeOverlaysExcept(Overlay::Cab);
    cabScreen_->setVisible(open);
    if (open) {
      cabScreen_->refresh();
      cabScreen_->toFront(false);
    }
    cabButton_.setToggleState(open, juce::dontSendNotification);
  }
  bool cabPageOpen() const { return cabScreen_->isVisible(); }
  rig::CabScreen& cabScreen() { return *cabScreen_; }
  rig::Pedalboard& pedalboard() { return *board_; }
  skin::CabChip& cabChip() { return rig_.cabChip(); }
  // MIC POSITIONS: the mic page, whose close button comes back to the CAB page.
  void openMicFromCab() {
    setMicPageOpen(true);
    micFromCab_ = true;
  }
  void refreshPanel() {
    if (panel_->isVisible()) panel_->refresh();
    if (rigPanel_->isVisible()) rigPanel_->refresh();
  }
  void setBrowserOpen(bool open) {
    if (open) closeOverlaysExcept(Overlay::Browser);
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
  rig::AmpHead& ampHead(int path) { return *ampHeads_[static_cast<size_t>(path)]; }
  // Cmd / Ctrl + Z: undo the last rig edit (RigController::undo); Cmd / Ctrl + Shift + Z: redo it. v0.3 Task D.
  bool handleKey(const juce::KeyPress& k) {
    if (!(k.getModifiers().isCommandDown() && (k.getKeyCode() == 'z' || k.getKeyCode() == 'Z'))) return false;
    const bool redo = k.getModifiers().isShiftDown();
    // Keys bubble up from children that did not take them: a text field (a read-only one passes Cmd+Z on) or an open overlay is
    // the user's current context, so it must never undo a rig edit underneath it.
    if (dynamic_cast<juce::TextInputTarget*>(focusProbe_()) != nullptr) return false;
    if (anyOverlayOpen()) return false;
    if (!(redo ? rigController_->canRedo() : rigController_->canUndo())) return false;
    return redo ? rigController_->redo() : rigController_->undo();
  }
  // The component that has the keyboard focus (tests replace it: a headless X server gives no window, so no focus).
  std::function<juce::Component*()> focusProbe_ = [] { return juce::Component::getCurrentlyFocusedComponent(); };
  // Overlays that cover or take over the editor's context: Cmd / Ctrl + Z (and + Shift) must not undo / redo underneath them. The RIG editor
  // (rigPanel_) is NOT in this list on purpose: it is where the rig is edited, so undoing from there is the point.
  // New overlays: add them here (or document why not) and to the test "does not bubble into an undo" in test_amp_head.cpp.
  bool anyOverlayOpen() const {
    const auto vis = [](const juce::Component* c) { return c != nullptr && c->isVisible(); };
    return vis(drawer_.get()) || vis(settingsPanel_.get()) || vis(about_.get()) || vis(presetBrowser_.get()) || vis(screen_.get()) ||
           vis(exportPanel_.get()) || vis(micPage_.get()) || vis(browser_.get()) || vis(panel_.get()) || vis(cabScreen_.get()) || board_->pickerOpen();
  }
  // Test hooks: the capture browser (the BROWSE CAPTURES overlay), and closing every overlay.
  void openCaptureBrowserForTests() { openBrowser(); }
  bool captureBrowserOpen() const { return browser_ != nullptr && browser_->isVisible(); }
  void closeAllOverlays() {
    settingsPanel_->close();
    settingsBtn_.setToggleState(false, juce::dontSendNotification);
    if (about_ != nullptr) about_->setVisible(false);
    drawer_->setOpen(false, /*animate=*/false);
    for (juce::Component* c : std::initializer_list<juce::Component*>{presetBrowser_.get(), screen_.get(), exportPanel_.get(), micPage_.get(), panel_.get(), rigPanel_.get(),
                                                                       cabScreen_.get()})
      c->setVisible(false);
    micFromCab_ = false;
    board_->closePicker();
    rigButton_.setToggleState(false, juce::dontSendNotification);
    cabButton_.setToggleState(false, juce::dontSendNotification);
    playAlong_.setToggleState(false, juce::dontSendNotification);
    if (browser_ != nullptr) browser_->onClose();
  }
  bool drawerOpen() const { return drawer_->isVisible(); }
  void setDrawerOpen(bool open) { drawer_->setOpen(open, /*animate=*/false); }
  void setFocusProbe(std::function<juce::Component*()> p) { focusProbe_ = std::move(p); }
  rig::RigController& rigControllerRef() { return *rigController_; }
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
    if (!open) micFromCab_ = false;  // (the page's own close handler has read it by now)
    if (open) closeOverlaysExcept(Overlay::Mic);
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
  void openMatchScreen() {
    closeOverlaysExcept(Overlay::Match);
    screen_->open();
    screen_->toFront(false);  // above an open rig editor / mic page / preset browser overlay
  }
  bool matchScreenOpen() const { return screen_->isVisible(); }
  void openExportPanel() {
    closeOverlaysExcept(Overlay::Export);
    exportPanel_->open();
    exportPanel_->toFront(false);
  }
  bool exportPanelOpen() const { return exportPanel_->isVisible(); }
  ExportPanel& exportPanel() { return *exportPanel_; }
  void refreshScreen() {
    screen_->refresh();
    exportPanel_->refresh();
  }

 private:
  // The full-width overlays are mutually exclusive: opening one closes the others (the RIG and CAB buttons follow). The settings panel and
  // the play-along panel are not part of the group.
  enum class Overlay { None, Rig, Mic, Browser, Match, Export, Cab };
  void closeOverlaysExcept(Overlay keep) {
    board_->closePicker();
    if (keep != Overlay::Rig) {
      rigPanel_->setVisible(false);
      rigButton_.setToggleState(false, juce::dontSendNotification);
    }
    if (keep != Overlay::Cab) {
      cabScreen_->setVisible(false);
      cabButton_.setToggleState(false, juce::dontSendNotification);
    }
    if (keep != Overlay::Mic) {
      micPage_->setVisible(false);
      micFromCab_ = false;
    }
    if (keep != Overlay::Browser) presetBrowser_->setVisible(false);
    if (keep != Overlay::Match) screen_->setVisible(false);
    if (keep != Overlay::Export) exportPanel_->setVisible(false);
  }

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

  // The inspector's selection: an amp head, or the pedal tile picked on the pedalboard ("SELECTED · SAW PEDAL" + the tile's name).
  void updateSelection() {
    const Piece p = rig_.selected();
    const auto s = selectionInfo(p);
    const bool pedal = p == Piece::SawPedal || p == Piece::BodyPedal;
    board_->setSelected(pedal ? (p == Piece::SawPedal ? 0 : 1) : -1, rig_.selectedBlockId());
    if (pedal && !rig_.selectedBlockId().empty() && board_->selectedTile() == nullptr) {
      rig_.select(p, {});  // the picked block left the board: back to the path's first tile (select() calls this again)
      return;
    }
    juce::String name = s.name;
    if (pedal) {
      const rig::BoardTile* t = board_->selectedTile();
      name = t != nullptr ? t->name() : juce::String("NO PEDAL");
    }
    selKind_.setText("SELECTED" + kDot + s.kind, juce::dontSendNotification);
    selName_.setText(name, juce::dontSendNotification);
    selName_.setColour(juce::Label::textColourId, juce::Colour(s.colour));
  }

  // A circuit block has a tile (hence a place for the live face) when it is before its path's amp and the path is on.
  // and, once the tile exists, that it is fully in view (a board that scrolls may hide it).
  bool circuitHosted(const CircuitSlot& slot) const {
    const Preset p = processor_.editBasePreset();
    const PathPreset& pp = slot.path == 0 ? p.a : p.b;
    if (slot.path == 1 && !pp.enabled) return false;
    return slot.block < rig::boardBlockCount(pp) && board_->tileFullyVisible(slot.path, slot.block);
  }
  bool isFaceTile(const rig::BoardTile& t) const {
    const auto slot = processor_.circuitSlot();
    return slot && slot->path == t.path() && slot->block == t.blockIndex();
  }
  // The live face lies over the tile of the first circuit block; the advanced drawer opens beside that tile.
  void placeFace() {
    const auto slot = processor_.circuitSlot();
    rig::BoardTile* t = slot && circuitHosted(*slot) ? board_->tileForBlock(slot->path, slot->block) : nullptr;
    const auto pedal = (t != nullptr ? board_->tileBounds(*t) : board_->slotBounds(0, 0)) + board_->getPosition();
    if (t != nullptr) {
      face_->setBounds(pedal);
      t->setTooltip(t->name() + " (click to select, double-click for the advanced controls, footswitch = bypass)");
    }
    if (pedal != anchor_) {  // setAnchor ends a running slide: only when the pedal moved
      anchor_ = pedal;
      drawer_->setAnchor(pedal, rig_.getBounds().withTrimmedRight(24));
    }
  }

  void chooseFile() {
    chooser_ = std::make_unique<juce::FileChooser>("Load a Sawblade preset", juce::File(), "*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
      const juce::File f = fc.getResult();
      if (f == juce::File()) return;
      processor_.loadPresetFile(std::filesystem::path(f.getFullPathName().toStdString()), nullptr, /*undoable=*/true);
    });
  }

  SawbladeProcessor& processor_;
  AbCompare abCompare_{processor_};
  juce::Label wordmark_, latChip_, modeChip_, message_;
  juce::Label selKind_, selName_, blendLabel_, blendRead_, thr_, matchTitle_, matchValue_;
  juce::TextButton prev_, next_, ab_, match_, export_, presetButton_, browse_, learn_, playAlong_, rigButton_, cabButton_, settingsBtn_;
  juce::uint32 learnShownUntil_ = 0;
  juce::String transient_;         // the pedalboard's last status message
  juce::uint32 transientUntil_ = 0;
  // New overlays: add them to anyOverlayOpen() (Cmd / Ctrl + Z) or document why not.
  std::array<std::unique_ptr<rig::AmpHead>, 2> ampHeads_;
  bool blendGesture_ = false;  // the BLEND knob is in a user drag / wheel gesture
  double blendBefore_ = 0.0;   // its value when the gesture started
  std::unique_ptr<PedalFace> face_;
  std::unique_ptr<AdvancedDrawer> drawer_;
  std::unique_ptr<PlayAlongPanel> panel_;
  std::unique_ptr<BrowserSettings> browserSettings_;
  std::unique_ptr<CaptureBrowser> browser_;  // declared after the settings it uses
  std::unique_ptr<rig::RigController> rigController_;  // before the panel that uses it
  std::unique_ptr<rig::RigEditorPanel> rigPanel_;
  std::unique_ptr<rig::Pedalboard> board_;  // after the controller it edits through
  std::unique_ptr<rig::CabScreen> cabScreen_;
  bool micFromCab_ = false;                 // the mic page was opened from the CAB page (its close returns there)
  juce::Rectangle<int> anchor_;             // where the advanced drawer is anchored (the pedal tile it opens beside)
  std::unique_ptr<MicPage> micPage_;
  std::unique_ptr<PresetBrowser> presetBrowser_;
  std::unique_ptr<MatchScreen> screen_;
  std::unique_ptr<ExportPanel> exportPanel_;
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

  setWantsKeyboardFocus(true);  // Cmd / Ctrl + Z
  setResizable(true, true);
  setResizeLimits(640, 400, 2560, 1600);
  getConstrainer()->setFixedAspectRatio(static_cast<double>(kDesignWidth) / kDesignHeight);
  const double uiScale = settings::Settings::shared().uiScale();
  setSize(juce::roundToInt(kDesignWidth * uiScale), juce::roundToInt(kDesignHeight * uiScale));
  startTimerHz(16);
}

SawbladeEditor::~SawbladeEditor() {
  stopTimer();
  processor_.historyAbortGestures();  // a drag still open when the window closes is one step, not a stuck gesture
  setLookAndFeel(nullptr);
}

void SawbladeEditor::parentHierarchyChanged() {
  // Standalone only: a plugin editor must not rename the host's window.
  if (processor_.wrapperType != juce::AudioProcessor::wrapperType_Standalone) return;
  if (auto* top = getTopLevelComponent(); top != nullptr && top != this)
    top->setName("Sawblade - " + about::buildStamp());
}

void SawbladeEditor::paint(juce::Graphics& g) { g.fillAll(L::background()); }

void SawbladeEditor::resized() {
  if (content_ == nullptr) return;
  content_->setBounds(0, 0, kDesignWidth, kDesignHeight);
  content_->setTransform(juce::AffineTransform::scale(static_cast<float>(contentScale())));
}

double SawbladeEditor::contentScale() const { return static_cast<double>(getWidth()) / kDesignWidth; }

skin::Piece SawbladeEditor::selectedPiece() const { return content_->rig().selected(); }
const std::string& SawbladeEditor::selectedBlockId() const { return content_->rig().selectedBlockId(); }
void SawbladeEditor::setCabPageOpen(bool open) { content_->setCabPageOpen(open); }
bool SawbladeEditor::cabPageOpen() const { return content_->cabPageOpen(); }
rig::CabScreen& SawbladeEditor::cabScreen() { return content_->cabScreen(); }
rig::Pedalboard& SawbladeEditor::pedalboard() { return content_->pedalboard(); }
skin::CabChip& SawbladeEditor::cabChip() { return content_->cabChip(); }

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
rig::AmpHead& SawbladeEditor::ampHead(int path) { return content_->ampHead(path); }
rig::RigController& SawbladeEditor::rigController() { return content_->rigControllerRef(); }
void SawbladeEditor::refreshNow() { content_->refresh(); }
void SawbladeEditor::openCaptureBrowserForTests() { content_->openCaptureBrowserForTests(); }
bool SawbladeEditor::captureBrowserOpen() const { return content_->captureBrowserOpen(); }
void SawbladeEditor::closeAllOverlaysForTests() { content_->closeAllOverlays(); }
bool SawbladeEditor::advancedDrawerOpen() const { return content_->drawerOpen(); }
void SawbladeEditor::setAdvancedDrawerOpen(bool open) { content_->setDrawerOpen(open); }
void SawbladeEditor::setFocusProbeForTests(std::function<juce::Component*()> probe) { content_->setFocusProbe(std::move(probe)); }
bool SawbladeEditor::keyPressed(const juce::KeyPress& k) { return content_->handleKey(k); }
void SawbladeEditor::openMatchScreen() { content_->openMatchScreen(); }
bool SawbladeEditor::matchScreenOpen() const { return content_->matchScreenOpen(); }
void SawbladeEditor::openExportPanel() { content_->openExportPanel(); }
bool SawbladeEditor::exportPanelOpen() const { return content_->exportPanelOpen(); }
ExportPanel& SawbladeEditor::exportPanel() { return content_->exportPanel(); }

bool SawbladeEditor::isInterestedInFileDrag(const juce::StringArray& files) { return PlayAlongPanel::isLoadableDrop(files); }

void SawbladeEditor::filesDropped(const juce::StringArray& files, int, int) {
  if (PlayAlongPanel::loadDroppedFiles(processor_, files)) setPlayAlongOpen(true);
}

}  // namespace sawblade::plugin
