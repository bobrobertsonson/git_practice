#include "CaptureBrowser.h"

#include <algorithm>

#include "SawbladeLookAndFeel.h"

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;
using Source = BrowserController::Source;
using View = BrowserController::View;

constexpr int kHeaderH = 58, kFilterW = 220, kSelW = 300, kStripH = 28;
constexpr int kBodyY = kHeaderH, kBodyH = CaptureBrowser::kHeight - kHeaderH - kStripH;
constexpr int kCardH = 156, kGap = 12, kPad = 16;

const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");

void styleLabel(juce::Label& l, const juce::Font& f, juce::Colour c, juce::Justification j = juce::Justification::centredLeft) {
  l.setFont(f);
  l.setColour(juce::Label::textColourId, c);
  l.setJustificationType(j);
  l.setInterceptsMouseClicks(false, false);
  l.setMinimumHorizontalScale(0.85f);
}

juce::String tagsText(const t3k::CaptureRecord& r) {
  juce::StringArray t;
  if (r.format == "ir" || r.irsCount > 0) {
    t.add("IR");
    if (r.irsCount > 0) t.add(juce::String(r.irsCount) + " IRS");
  } else {
    if (r.a2Count > 0) t.add("A2");
    if (r.modelsCount > 0) t.add(juce::String(r.modelsCount) + (r.modelsCount == 1 ? " MODEL" : " MODELS"));
  }
  for (const auto& s : r.sizes) t.add(juce::String(s).toUpperCase());
  if (!r.status.empty() && r.status != "included") t.add(juce::String(r.status).toUpperCase());
  for (const auto& f : r.flags) t.add(juce::String(f).toUpperCase());
  juce::String out = t.joinIntoString(kDot);
  if (!r.passes && !r.reasons.empty()) {
    juce::StringArray why;
    for (const auto& s : r.reasons) why.add(juce::String(s));
    out += "\nFAILS: " + why.joinIntoString("; ");
  }
  return out;
}

}  // namespace

const char* CaptureBrowser::licenceNote() {
  return "Non-commercial (CC BY-NC) captures are usable: the rig and anything exported from it is marked non-commercial, "
         "and exports are for your own use.";
}

struct CaptureBrowser::Impl {
  // One card of the grid.
  struct Card : juce::Component {
    Card(const t3k::CaptureRecord& r, bool sel, bool perPathCab) : id(r.toneId), selected(sel) {
      setTitle(juce::String(r.title));
      styleLabel(title, L::titleFont(16.0f), L::text());
      title.setText(juce::String(r.title), juce::dontSendNotification);
      title.setComponentID("title");
      styleLabel(creator, L::labelFont(12.0f), L::dimText());
      creator.setText("@" + juce::String(r.creator), juce::dontSendNotification);
      creator.setComponentID("creator");
      // The licence is always shown, whether or not the record passes the filter.
      styleLabel(licence, L::labelFont(12.0f), r.passes ? L::sawText() : L::warning());
      licence.setText(r.license.empty() ? juce::String("unknown licence") : juce::String(r.license), juce::dontSendNotification);
      licence.setComponentID("licence");
      // v0.3 Task E: "STEPS n" when this tone's pack is known (the `ladder` tool, asked for the selected tone) to have a gain ladder; nothing otherwise.
      styleLabel(steps, L::monoFont(11.0f), L::sawText(), juce::Justification::centredRight);
      steps.setComponentID("steps");
      steps.setTooltip("This pack has a gain ladder: the amp's GAIN knob steps through its captures");
      styleLabel(tags, L::labelFont(11.0f), L::text().withAlpha(0.85f), juce::Justification::topLeft);
      tags.setText(tagsText(r), juce::dontSendNotification);
      styleLabel(stats, L::monoFont(12.0f), L::dimText());
      stats.setText(juce::String::fromUTF8("\xe2\x99\xa5 ") + juce::String(r.favorites) + kDot + juce::String::fromUTF8("\xe2\x86\x93 ") + juce::String(r.downloads),
                    juce::dontSendNotification);
      preview.setButtonText("PREVIEW");
      preview.setTitle("Preview " + juce::String(r.title));
      preview.setTooltip("Play the built-in riff through your rig with this capture swapped in");
      use.setButtonText(perPathCab ? "USE..." : "USE");
      use.setTitle("Use " + juce::String(r.title));
      use.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a1a0e));
      use.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffffb27a));
      for (juce::Component* c : {static_cast<juce::Component*>(&title), static_cast<juce::Component*>(&creator), static_cast<juce::Component*>(&licence), static_cast<juce::Component*>(&steps),
                                 static_cast<juce::Component*>(&tags), static_cast<juce::Component*>(&stats), static_cast<juce::Component*>(&preview),
                                 static_cast<juce::Component*>(&use)})
        addAndMakeVisible(*c);
      dim = !r.passes;
    }
    void resized() override {
      auto b = getLocalBounds().reduced(12, 10);
      title.setBounds(b.removeFromTop(22));
      creator.setBounds(b.removeFromTop(16));
      auto licRow = b.removeFromTop(16);
      steps.setBounds(licRow.removeFromRight(72));
      licence.setBounds(licRow);
      b.removeFromTop(4);
      auto bottom = b.removeFromBottom(26);
      stats.setBounds(b.removeFromBottom(18));
      tags.setBounds(b);
      use.setBounds(bottom.removeFromRight(56));
      bottom.removeFromRight(6);
      preview.setBounds(bottom.removeFromRight(86));
    }
    void paint(juce::Graphics& g) override {
      auto r = getLocalBounds().toFloat();
      g.setColour(juce::Colour(0xff151311));
      g.fillRoundedRectangle(r, 8.0f);
      g.setColour(selected ? L::saw() : L::rule());
      g.drawRoundedRectangle(r.reduced(selected ? 1.0f : 0.5f), 8.0f, selected ? 2.0f : 1.0f);
      if (dim) {
        g.setColour(L::background().withAlpha(0.35f));
        g.fillRoundedRectangle(r, 8.0f);
      }
    }
    void mouseDown(const juce::MouseEvent&) override {
      if (onSelect) onSelect(id);
    }
    void setSteps(int n) { steps.setText(n >= 2 ? "STEPS " + juce::String(n) : juce::String(), juce::dontSendNotification); }
    std::int64_t id;
    bool selected, dim = false;
    juce::Label title, creator, licence, steps, tags, stats;
    juce::TextButton preview, use;
    std::function<void(std::int64_t)> onSelect;
  };

  struct Grid : juce::Component {
    std::vector<std::unique_ptr<Card>> cards;
    int width = 0;
    void layoutCards() {
      const int cols = 3;
      const int cw = (width - 2 * kPad - (cols - 1) * kGap) / cols;
      int i = 0;
      for (auto& c : cards) {
        c->setBounds(kPad + (i % cols) * (cw + kGap), kPad + (i / cols) * (kCardH + kGap), cw, kCardH);
        ++i;
      }
      const int rows = (static_cast<int>(cards.size()) + cols - 1) / cols;
      setSize(width, std::max(kBodyH, 2 * kPad + rows * (kCardH + kGap)));
    }
  };

  Impl(CaptureBrowser& o, BrowserController& c) : owner(o), ctl(c) {}

  CaptureBrowser& owner;
  BrowserController& ctl;

  juce::TextButton back, sourceFav, sourceSearch, sourcePool, gearPedal, gearAmp, gearIr, passes, openBrowser, loginBtn, cancelLogin, resetCli;
  juce::TextButton selPreview, selUse[2];
  std::vector<std::unique_ptr<juce::TextButton>> modelButtons;
  juce::Label title, forSlot, searchCap, center, filterNote, sourceCap, gearCap, qualityCap, selCap, selTitle, selMeta, selLicence, selStatus, selReason,
      selHint, modelsCap, loginTitle, loginMsg, cliCap;
  juce::TextEditor search, cliPath, loginUri, loginCode;
  juce::Viewport viewport;
  Grid grid;
  juce::String gridSignature;
  int shownTargets = 0;

  juce::TextButton* pill(juce::TextButton& b, const juce::String& text, const juce::String& tip, int group) {
    b.setButtonText(text);
    b.setTitle(text);
    b.setTooltip(tip);
    b.setClickingTogglesState(true);
    if (group) b.setRadioGroupId(group);
    b.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff6b2f12));
    b.setColour(juce::TextButton::textColourOnId, juce::Colour(0xffffb27a));
    owner.addAndMakeVisible(b);
    return &b;
  }
  void button(juce::TextButton& b, const juce::String& text, const juce::String& tip) {
    b.setButtonText(text);
    b.setTitle(text);
    b.setTooltip(tip);
    owner.addAndMakeVisible(b);
  }
  void caption(juce::Label& l, const juce::String& text) {
    l.setText(text, juce::dontSendNotification);
    styleLabel(l, L::labelFont(11.0f), L::dimText());
    owner.addAndMakeVisible(l);
  }
  void editor(juce::TextEditor& e, const juce::String& title, const juce::Font& f, bool readOnly) {
    e.setTitle(title);
    e.setFont(f);
    e.setReadOnly(readOnly);
    e.setSelectAllWhenFocused(readOnly);
    e.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0e0d0c));
    e.setColour(juce::TextEditor::textColourId, L::text());
    e.setColour(juce::TextEditor::outlineColourId, L::chipBorder());
    e.setColour(juce::TextEditor::focusedOutlineColourId, L::saw());
    e.setIndents(8, 6);
    owner.addAndMakeVisible(e);
  }

  void build() {
    button(back, juce::String::fromUTF8("\xe2\x80\xb9 RIG"), "Back to the rig");
    back.onClick = [this] {
      if (owner.onClose) owner.onClose();
    };
    title.setText("CAPTURES", juce::dontSendNotification);
    styleLabel(title, juce::Font(juce::FontOptions(24.0f, juce::Font::bold)), L::saw());
    owner.addAndMakeVisible(title);
    forSlot.setText(juce::String("FOR") + kDot + slotName(ctl.slot()) + " SLOT", juce::dontSendNotification);
    styleLabel(forSlot, L::labelFont(12.0f), L::dimText());
    owner.addAndMakeVisible(forSlot);
    caption(searchCap, "SEARCH");
    searchCap.setJustificationType(juce::Justification::centredRight);
    editor(search, "Search captures", L::monoFont(13.0f), false);
    search.setTextToShowWhenEmpty("tone name, creator...", L::placeholderText());
    search.onReturnKey = [this] { ctl.setQuery(search.getText().trim().toStdString()); };

    caption(sourceCap, "SOURCE");
    pill(sourceFav, "FAVORITES", "Your TONE3000 favorites", 701);
    pill(sourceSearch, "SEARCH", "Search TONE3000 (type a query and press Enter)", 701);
    pill(sourcePool, "POOL", "Captures already in the local pool", 701);
    sourceFav.onClick = [this] { ctl.setSource(Source::Favorites); };
    sourceSearch.onClick = [this] { ctl.setSource(Source::Search); };
    sourcePool.onClick = [this] { ctl.setSource(Source::Pool); };
    caption(gearCap, "GEAR");
    pill(gearPedal, "PEDAL", "Pedal captures", 702);
    pill(gearAmp, "AMP", "Amp captures", 702);
    pill(gearIr, "CAB IR", "Cabinet impulse responses", 702);
    gearPedal.onClick = [this] { ctl.setGear("pedal"); };
    gearAmp.onClick = [this] { ctl.setGear("amp"); };
    gearIr.onClick = [this] { ctl.setGear("ir"); };
    caption(qualityCap, "QUALITY");
    pill(passes, "PASSES FILTER ONLY", "Hide captures that fail the quality filter", 0);
    passes.onClick = [this] { ctl.setPassesOnly(passes.getToggleState()); };
    filterNote.setText(CaptureBrowser::licenceNote(), juce::dontSendNotification);
    styleLabel(filterNote, L::bodyFont(12.0f), L::dimText(), juce::Justification::topLeft);
    owner.addAndMakeVisible(filterNote);

    viewport.setViewedComponent(&grid, false);
    viewport.setScrollBarsShown(true, false);
    owner.addAndMakeVisible(viewport);
    styleLabel(center, L::bodyFont(15.0f), L::dimText(), juce::Justification::centred);
    owner.addAndMakeVisible(center);

    loginTitle.setText("Log in to TONE3000", juce::dontSendNotification);
    styleLabel(loginTitle, L::titleFont(22.0f), L::text(), juce::Justification::centred);
    styleLabel(loginMsg, L::bodyFont(14.0f), L::dimText(), juce::Justification::centred);
    for (auto* l : {&loginTitle, &loginMsg}) owner.addChildComponent(*l);
    button(loginBtn, "LOG IN", "Log in to TONE3000 in your browser");
    loginBtn.setColour(juce::TextButton::buttonColourId, L::saw());
    loginBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff140a04));
    loginBtn.onClick = [this] { ctl.logIn(); };
    button(openBrowser, "OPEN IN BROWSER", "Open the login page");
    openBrowser.onClick = [this] {
      const auto u = juce::URL(juce::String(ctl.state().loginUri));
      if (u.isWellFormed()) u.launchInDefaultBrowser();
    };
    button(cancelLogin, "CANCEL", "Cancel the login");
    cancelLogin.onClick = [this] { ctl.cancelLogin(); };
    editor(loginUri, "Login address", L::monoFont(14.0f), true);
    editor(loginCode, "Login code", juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 30.0f, juce::Font::bold)), true);
    for (juce::Component* c : {static_cast<juce::Component*>(&loginBtn), static_cast<juce::Component*>(&openBrowser),
                               static_cast<juce::Component*>(&cancelLogin), static_cast<juce::Component*>(&loginUri), static_cast<juce::Component*>(&loginCode)})
      c->setVisible(false);

    caption(selCap, "SELECTED");
    styleLabel(selTitle, L::titleFont(20.0f), L::text(), juce::Justification::topLeft);
    selTitle.setMinimumHorizontalScale(0.9f);
    styleLabel(selMeta, L::labelFont(12.0f), L::dimText(), juce::Justification::topLeft);
    styleLabel(selLicence, L::labelFont(13.0f), L::sawText());
    selLicence.setComponentID("selLicence");
    caption(modelsCap, "MODELS");
    styleLabel(selStatus, L::bodyFont(13.0f), L::text(), juce::Justification::topLeft);
    styleLabel(selReason, L::bodyFont(12.0f), L::warning(), juce::Justification::topLeft);
    selHint.setText("Preview plays a built-in DI riff through your rig with this capture swapped in.", juce::dontSendNotification);
    styleLabel(selHint, L::bodyFont(12.0f), L::dimText(), juce::Justification::topLeft);
    for (auto* l : {&selTitle, &selMeta, &selLicence, &selStatus, &selReason, &selHint}) owner.addAndMakeVisible(*l);
    button(selPreview, juce::String::fromUTF8("\xe2\x96\xb6 PREVIEW"), "Play the built-in riff through your rig with this capture swapped in");
    selPreview.onClick = [this] {
      if (ctl.state().previewing || ctl.state().status == "Rendering the preview...") ctl.stopPreview();
      else ctl.preview();
    };
    for (int i = 0; i < 2; ++i) {
      button(selUse[i], "USE", "Use this capture");
      selUse[i].setColour(juce::TextButton::buttonColourId, L::saw());
      selUse[i].setColour(juce::TextButton::textColourOffId, juce::Colour(0xff140a04));
      selUse[i].onClick = [this, i] { ctl.use(i); };
    }

    caption(cliCap, "CLI");
    editor(cliPath, "Path of sawblade-t3k", L::monoFont(12.0f), false);
    cliPath.setText(ctl.executable(), juce::dontSendNotification);
    auto commit = [this] {
      const std::string t = cliPath.getText().trim().toStdString();
      if (t != ctl.executable()) ctl.setExecutable(t);
    };
    cliPath.onReturnKey = commit;
    cliPath.onFocusLost = commit;
    button(resetCli, "RESET", "Use the default sawblade-t3k path");
    resetCli.onClick = [this] {
      ctl.setExecutable({});
      cliPath.setText(ctl.executable(), juce::dontSendNotification);
    };
  }

  void layout() {
    auto r = owner.getLocalBounds();
    auto head = r.removeFromTop(kHeaderH).reduced(18, 12);
    back.setBounds(head.removeFromLeft(80).withHeight(34));
    head.removeFromLeft(14);
    title.setBounds(head.removeFromLeft(130));
    forSlot.setBounds(head.removeFromLeft(260));
    search.setBounds(head.removeFromRight(240).withSizeKeepingCentre(240, 30));
    searchCap.setBounds(head.removeFromRight(70));

    auto strip = r.removeFromBottom(kStripH).reduced(18, 3);
    cliCap.setBounds(strip.removeFromLeft(36));
    resetCli.setBounds(strip.removeFromRight(70));
    strip.removeFromRight(8);
    cliPath.setBounds(strip.removeFromLeft(520));

    auto filters = r.removeFromLeft(kFilterW).reduced(kPad, kPad);
    sourceCap.setBounds(filters.removeFromTop(14));
    filters.removeFromTop(6);
    {
      auto row = filters.removeFromTop(28);
      sourceFav.setBounds(row.removeFromLeft(92));
      row.removeFromLeft(6);
      sourceSearch.setBounds(row.removeFromLeft(80));
      filters.removeFromTop(6);
      sourcePool.setBounds(filters.removeFromTop(28).removeFromLeft(80));
    }
    filters.removeFromTop(14);
    gearCap.setBounds(filters.removeFromTop(14));
    filters.removeFromTop(6);
    {
      auto row = filters.removeFromTop(28);
      gearPedal.setBounds(row.removeFromLeft(76));
      row.removeFromLeft(6);
      gearAmp.setBounds(row.removeFromLeft(60));
      filters.removeFromTop(6);
      gearIr.setBounds(filters.removeFromTop(28).removeFromLeft(80));
    }
    filters.removeFromTop(14);
    qualityCap.setBounds(filters.removeFromTop(14));
    filters.removeFromTop(6);
    passes.setBounds(filters.removeFromTop(28));
    filterNote.setBounds(filters.removeFromBottom(60));

    auto sel = r.removeFromRight(kSelW).reduced(kPad, kPad);
    selCap.setBounds(sel.removeFromTop(14));
    selTitle.setBounds(sel.removeFromTop(52));
    selMeta.setBounds(sel.removeFromTop(32));
    selLicence.setBounds(sel.removeFromTop(20));
    sel.removeFromTop(8);
    modelsCap.setBounds(sel.removeFromTop(14));
    modelsArea = sel.removeFromTop(8 * 30);
    sel.removeFromTop(6);
    selStatus.setBounds(sel.removeFromTop(64));
    selReason.setBounds(sel.removeFromTop(34));
    selHint.setBounds(sel.removeFromBottom(34));
    selUse[1].setBounds(sel.removeFromBottom(40));
    sel.removeFromBottom(6);
    selUse[0].setBounds(sel.removeFromBottom(40));
    sel.removeFromBottom(6);
    selPreview.setBounds(sel.removeFromBottom(34));

    centerArea = r;
    viewport.setBounds(centerArea);
    center.setBounds(centerArea.reduced(40));
    grid.width = viewport.getMaximumVisibleWidth();
    grid.layoutCards();
    const int cx = centerArea.getCentreX(), cy = centerArea.getY() + 150;
    loginTitle.setBounds(cx - 240, cy, 480, 30);
    loginMsg.setBounds(cx - 300, cy + 40, 600, 40);
    loginBtn.setBounds(cx - 90, cy + 100, 180, 40);
    loginUri.setBounds(cx - 280, cy + 100, 560, 34);
    loginCode.setBounds(cx - 130, cy + 146, 260, 56);
    openBrowser.setBounds(cx - 160, cy + 220, 150, 36);
    cancelLogin.setBounds(cx + 10, cy + 220, 150, 36);
  }

  void onSelect(std::int64_t id) { ctl.select(id); }

  void refresh() {
    const auto& st = ctl.state();
    const bool browse = st.view == View::Browse;
    sourceFav.setToggleState(st.source == Source::Favorites, juce::dontSendNotification);
    sourceSearch.setToggleState(st.source == Source::Search, juce::dontSendNotification);
    sourcePool.setToggleState(st.source == Source::Pool, juce::dontSendNotification);
    gearPedal.setToggleState(st.gear == "pedal", juce::dontSendNotification);
    gearAmp.setToggleState(st.gear == "amp", juce::dontSendNotification);
    gearIr.setToggleState(st.gear == "ir", juce::dontSendNotification);
    passes.setToggleState(st.passesOnly, juce::dontSendNotification);

    // Centre: login views, messages or the grid.
    const bool loginView = st.view == View::LoginRequired, loggingIn = st.view == View::LoggingIn;
    for (auto* c : {static_cast<juce::Component*>(&loginTitle), static_cast<juce::Component*>(&loginMsg)}) c->setVisible(loginView || loggingIn);
    loginBtn.setVisible(loginView);
    loginUri.setVisible(loggingIn);
    loginCode.setVisible(loggingIn);
    openBrowser.setVisible(loggingIn);
    cancelLogin.setVisible(loggingIn);
    if (loginView) loginMsg.setText(st.loginMessage.empty() ? juce::String("Sawblade needs your TONE3000 account to list and download captures.") : juce::String(st.loginMessage), juce::dontSendNotification);
    if (loggingIn) {
      loginMsg.setText(juce::String(st.loginMessage), juce::dontSendNotification);
      if (loginUri.getText() != juce::String(st.loginUri)) loginUri.setText(juce::String(st.loginUri), juce::dontSendNotification);
      if (loginCode.getText() != juce::String(st.loginCode)) loginCode.setText(juce::String(st.loginCode), juce::dontSendNotification);
      openBrowser.setEnabled(!st.loginUri.empty());
    }

    const auto vis = ctl.visible();
    juce::String msg;
    bool msgError = false;
    if (st.view == View::Checking) msg = "Checking your TONE3000 login...";
    else if (browse) {
      if (st.loading) msg = "Loading...";
      else if (st.listError) {
        msg = juce::String(describe(*st.listError));
        msgError = true;
      } else if (vis.empty())
        msg = st.source == Source::Search && st.query.empty() ? "Type a query and press Enter" : "No captures";
    }
    center.setText(msg, juce::dontSendNotification);
    center.setColour(juce::Label::textColourId, msgError ? L::error() : L::dimText());
    center.setVisible(msg.isNotEmpty());
    viewport.setVisible(browse && msg.isEmpty());

    std::string why;
    const auto targets = ctl.targets(&why);
    const bool perPath = slotIsCab() && targets.size() == 2;
    if (gridSig() != gridSignature || modelsSig_() != modelsSig) scheduleRebuild();
    for (auto& c : grid.cards) {
      c->preview.setEnabled(!targets.empty());
      c->use.setEnabled(!targets.empty());
    }
    refreshSteps();

    // Selected panel.
    const auto* rec = ctl.selected();
    const bool has = rec != nullptr;
    for (juce::Component* c : {static_cast<juce::Component*>(&selTitle), static_cast<juce::Component*>(&selMeta), static_cast<juce::Component*>(&selLicence),
                               static_cast<juce::Component*>(&modelsCap), static_cast<juce::Component*>(&selPreview), static_cast<juce::Component*>(&selHint)})
      c->setVisible(has);
    if (has) {
      selTitle.setText(juce::String(rec->title), juce::dontSendNotification);
      juce::String meta = "@" + juce::String(rec->creator);
      if (!rec->createdAt.empty()) meta += kDot + juce::String(rec->createdAt).substring(0, 10);
      meta += kDot + juce::String(rec->format == "ir" ? rec->irsCount : rec->modelsCount) + (rec->format == "ir" ? " IR" : " models");
      selMeta.setText(meta, juce::dontSendNotification);
      selLicence.setText(rec->license.empty() ? juce::String("unknown licence") : juce::String(rec->license), juce::dontSendNotification);
      selLicence.setColour(juce::Label::textColourId, rec->passes ? L::sawText() : L::warning());
    }
    selStatus.setText(juce::String(st.status), juce::dontSendNotification);
    selStatus.setColour(juce::Label::textColourId, st.statusIsError ? L::error() : L::text());
    selReason.setText(targets.empty() ? juce::String(why) : juce::String(), juce::dontSendNotification);
    const bool busy = st.busy;
    selPreview.setButtonText((st.previewing || st.status == "Rendering the preview...") ? juce::String("STOP") : juce::String::fromUTF8("\xe2\x96\xb6 PREVIEW"));
    selPreview.setEnabled(has && !targets.empty() && (!busy || st.status == "Rendering the preview..."));
    for (int i = 0; i < 2; ++i) {
      const bool shown = has && i < static_cast<int>(targets.size());
      selUse[i].setVisible(shown);
      if (shown) {
        selUse[i].setButtonText(perPath ? "USE IN " + juce::String(targets[static_cast<std::size_t>(i)].label) : "USE IN " + juce::String(slotName(ctl.slot())));
        selUse[i].setTitle(selUse[i].getButtonText());
        selUse[i].setEnabled(!busy);
      }
    }
    if (has && targets.empty()) {
      selUse[0].setVisible(true);
      selUse[0].setButtonText("USE IN " + juce::String(slotName(ctl.slot())));
      selUse[0].setEnabled(false);
    }
  }

  bool slotIsCab() const { return ctl.slot() == Slot::Cab; }

  bool perPathCab() const {
    std::string why;
    return slotIsCab() && ctl.targets(&why).size() == 2;
  }
  // The ladder answers arrive asynchronously (through the processor's ladder tool): the cards pick them up here, no rebuild.
  // The cards on screen (inside the viewport), in display order, are the tones whose ladder is looked up.
  void updateLadderWanted() {
    const auto area = viewport.getViewArea();
    std::vector<std::int64_t> ids;
    // Hidden by the editor (its close button hides it before it is destroyed): nothing on screen. (A browser with no parent is not hidden by anyone.)
    const bool hidden = owner.getParentComponent() != nullptr && !owner.isVisible();
    if (viewport.isVisible() && !hidden)
      for (auto& c : grid.cards)
        if (c->getBounds().intersects(area)) ids.push_back(c->id);
    ctl.wantLadders(ids);
  }
  void refreshSteps() {
    updateLadderWanted();
    for (auto& c : grid.cards) c->setSteps(ctl.ladderSteps(c->id));
  }
  juce::String gridSig() const {
    const auto& st = ctl.state();
    juce::String sig = juce::String(st.selectedId) + "|" + juce::String(perPathCab() ? 1 : 0);
    for (auto* v : ctl.visible()) sig << "," << juce::String(v->toneId);
    return sig;
  }
  juce::String modelsSig_() const {
    const auto& st = ctl.state();
    const bool has = ctl.selected() != nullptr;
    const std::size_t want = has ? std::min<std::size_t>(st.models.size(), 8) : 0;
    juce::String sig = juce::String(has ? 1 : 0) + juce::String(st.modelIndex) + "|" + juce::String(st.modelsLoading ? 1 : 0);
    for (std::size_t i = 0; i < want; ++i) sig << "," << juce::String(st.models[i].modelId);
    return sig;
  }

  // Cards and model buttons are rebuilt outside the callbacks of the widgets being replaced.
  void scheduleRebuild() {
    if (rebuildPending) return;
    rebuildPending = true;
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<CaptureBrowser>(&owner)] {
      if (safe != nullptr) safe->impl_->rebuildDynamic();
    });
  }

  void rebuildDynamic() {
    rebuildPending = false;
    const auto& st = ctl.state();
    std::string why;
    const auto targets = ctl.targets(&why);
    const bool perPath = slotIsCab() && targets.size() == 2;
    gridSignature = gridSig();
    grid.cards.clear();
    grid.removeAllChildren();
    for (auto* v : ctl.visible()) {
      auto c = std::make_unique<Card>(*v, v->toneId == st.selectedId, perPath);
      const std::int64_t id = v->toneId;
      c->onSelect = [this](std::int64_t i) { onSelect(i); };
      c->preview.onClick = [this, id] {
        ctl.select(id);
        ctl.preview();
      };
      c->use.onClick = [this, id, perPath] {
        ctl.select(id);
        if (!perPath) ctl.use(0);
      };
      c->preview.setEnabled(!targets.empty());
      c->use.setEnabled(!targets.empty());
      c->setSteps(ctl.ladderSteps(id));
      grid.addAndMakeVisible(*c);
      grid.cards.push_back(std::move(c));
    }
    grid.layoutCards();

    const bool has = ctl.selected() != nullptr;
    modelsSig = modelsSig_();
    modelButtons.clear();
    const std::size_t want = has ? std::min<std::size_t>(st.models.size(), 8) : 0;
    for (std::size_t i = 0; i < want; ++i) {
      auto b = std::make_unique<juce::TextButton>();
      const auto& m = st.models[i];
      juce::String text = juce::String(m.name.empty() ? "model " + std::to_string(m.modelId) : m.name);
      if (!m.size.empty()) text += "   " + juce::String(m.size);
      b->setButtonText(text);
      b->setTitle(text);
      b->setToggleState(static_cast<int>(i) == st.modelIndex, juce::dontSendNotification);
      b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff6b2f12));
      b->setColour(juce::TextButton::textColourOnId, juce::Colour(0xffffb27a));
      b->setBounds(modelsArea.getX(), modelsArea.getY() + static_cast<int>(i) * 30, modelsArea.getWidth(), 26);
      const int idx = static_cast<int>(i);
      b->onClick = [this, idx] { ctl.selectModel(idx); };
      owner.addAndMakeVisible(*b);
      modelButtons.push_back(std::move(b));
    }
  }

  bool rebuildPending = false;
  juce::Rectangle<int> centerArea, modelsArea;
  juce::String modelsSig;
};

CaptureBrowser::CaptureBrowser(SawbladeProcessor& p, BrowserSettings& settings, Slot slot)
    : ctl_(std::make_unique<BrowserController>(p, settings, slot)), impl_(std::make_unique<Impl>(*this, *ctl_)) {
  setSize(kWidth, kHeight);
  setOpaque(true);
  setTitle("Capture browser");
  impl_->build();
  impl_->layout();
  ctl_->onChange = [this] { impl_->refresh(); };
  impl_->refresh();
  impl_->rebuildDynamic();
  ctl_->start();
  startTimerHz(10);
}

CaptureBrowser::~CaptureBrowser() {
  stopTimer();
  ctl_->onChange = nullptr;
}

void CaptureBrowser::timerCallback() {
  ctl_->poll();
  impl_->refreshSteps();
}

void CaptureBrowser::paint(juce::Graphics& g) {
  using L = SawbladeLookAndFeel;
  g.fillAll(L::background());
  g.setColour(L::panel());
  g.fillRect(0, 0, kWidth, kHeaderH);
  g.setColour(L::rule());
  g.drawHorizontalLine(kHeaderH - 1, 0.0f, static_cast<float>(kWidth));
  g.setColour(juce::Colour(0xff100f0e));
  g.fillRect(0, kBodyY, kFilterW, kBodyH);
  g.fillRect(kWidth - kSelW, kBodyY, kSelW, kBodyH);
  g.setColour(L::rule());
  g.drawVerticalLine(kFilterW, static_cast<float>(kBodyY), static_cast<float>(kBodyY + kBodyH));
  g.drawVerticalLine(kWidth - kSelW, static_cast<float>(kBodyY), static_cast<float>(kBodyY + kBodyH));
  g.setColour(L::panel());
  g.fillRect(0, kHeight - kStripH, kWidth, kStripH);
  g.setColour(L::rule());
  g.drawHorizontalLine(kHeight - kStripH, 0.0f, static_cast<float>(kWidth));
}

void CaptureBrowser::resized() {
  if (impl_) impl_->layout();
}

}  // namespace sawblade::plugin
