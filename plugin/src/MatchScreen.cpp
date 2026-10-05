#include "MatchScreen.h"

#include <algorithm>
#include <cmath>

#include "MatchGlue.h"
#include "SawbladeLookAndFeel.h"

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;
namespace fs = std::filesystem;

const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");

juce::String clock(double seconds) {
  if (!(seconds > 0.0)) seconds = 0.0;
  const int s = static_cast<int>(seconds + 0.5);
  return juce::String::formatted("%02d:%02d", s / 60, s % 60);
}

// Progress bar: determinate fill, or a moving stripe while the fraction is unknown.
class Bar : public juce::Component {
 public:
  void set(double fraction, bool indeterminate, juce::Colour c) {
    if (fraction == f_ && indeterminate == ind_ && c == col_ && !ind_) return;
    f_ = fraction;
    ind_ = indeterminate;
    col_ = c;
    repaint();
  }
  double fraction() const { return f_; }
  bool indeterminate() const { return ind_; }
  void paint(juce::Graphics& g) override {
    const auto r = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff141210));
    g.fillRoundedRectangle(r, 3.0f);
    g.setColour(L::chipBorder());
    g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
    const auto inner = r.reduced(2.0f);
    g.setColour(col_);
    if (ind_) {
      const float w = inner.getWidth() * 0.22f;
      const float t = static_cast<float>(juce::Time::getMillisecondCounter() % 1600) / 1600.0f;
      g.fillRoundedRectangle(inner.getX() + (inner.getWidth() - w) * t, inner.getY(), w, inner.getHeight(), 2.0f);
    } else if (f_ > 0.0) {
      g.fillRoundedRectangle(inner.withWidth(inner.getWidth() * static_cast<float>(std::min(1.0, f_))), 2.0f);
    }
  }

 private:
  double f_ = 0.0;
  bool ind_ = false;
  juce::Colour col_ = L::saw();
};

}  // namespace

struct MatchScreen::Impl : juce::ListBoxModel {
  Impl(MatchScreen& o, SawbladeProcessor& p) : owner(o), proc(p) {}

  MatchScreen& owner;
  SawbladeProcessor& proc;
  Mode mode = Mode::Match;

  juce::Label title, subtitle;
  juce::TextButton closeBtn;
  // match column
  juce::Label capRef, refName, refStem, capDi, diName, diOffset, capTools, exeLabel, poolLabel, toolMsg;
  juce::TextButton exeLocate, poolLocate, startBtn, cancelBtn;
  // export column
  juce::Label capSource, sourceLabel, capMode, modeHint, capSize, capDevice, deviceLabel, exportMsg;
  juce::TextButton noCab, withCab, feather, lite, standard, exportBtn, exportCancel, revealBtn;
  // shared progress + results
  juce::Label capProgress, stage, message, eta, capResults, auditionStatus, resultLabel, licence;
  Bar bar;
  juce::ListBox results{"Match results", this};
  juce::TextButton audition, ab, apply, revert;

  std::vector<MatchCandidate> rows;
  std::unique_ptr<juce::FileChooser> chooser;
  std::string appliedName;
  bool exportModeChosen = false;
  std::string exportError;

  // ---- helpers ------------------------------------------------------------------------------------------
  void caption(juce::Label& l, const juce::String& text) {
    l.setText(text, juce::dontSendNotification);
    l.setFont(L::labelFont(11.0f));
    l.setColour(juce::Label::textColourId, L::dimText());
    l.setInterceptsMouseClicks(false, false);
    owner.addAndMakeVisible(l);
  }
  void text(juce::Label& l, float size, juce::Colour c = L::text(), bool mono = false, bool bold = false) {
    l.setFont(mono ? L::monoFont(size) : bold ? L::titleFont(size) : L::bodyFont(size));
    l.setColour(juce::Label::textColourId, c);
    l.setJustificationType(juce::Justification::topLeft);
    l.setMinimumHorizontalScale(1.0f);
    l.setInterceptsMouseClicks(false, false);
    owner.addAndMakeVisible(l);
  }
  void button(juce::TextButton& b, const juce::String& t, const juce::String& tip, bool toggle = false) {
    b.setButtonText(t);
    b.setTitle(t);
    b.setTooltip(tip);
    if (toggle) b.setClickingTogglesState(true);
    b.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff6b2f12));
    owner.addAndMakeVisible(b);
  }

  void build() {
    title.setFont(L::titleFont(22.0f));
    title.setColour(juce::Label::textColourId, L::saw());
    subtitle.setFont(L::labelFont(11.0f));
    subtitle.setColour(juce::Label::textColourId, L::dimText());
    for (juce::Label* l : {&title, &subtitle}) {
      l->setInterceptsMouseClicks(false, false);
      owner.addAndMakeVisible(*l);
    }
    button(closeBtn, juce::String::fromUTF8("\xe2\x80\xb9 RIG"), "Close this screen (a running job keeps going)");

    caption(capRef, juce::String::fromUTF8("1 \xc2\xb7 REFERENCE"));
    text(refName, 15.0f, L::text(), false, true);
    text(refStem, 12.0f, L::dimText());
    caption(capDi, juce::String::fromUTF8("2 \xc2\xb7 YOUR DI"));
    text(diName, 15.0f, L::text(), false, true);
    text(diOffset, 12.0f, L::dimText());
    caption(capTools, juce::String::fromUTF8("3 \xc2\xb7 TOOLS"));
    text(exeLabel, 11.0f, L::dimText(), true);
    text(poolLabel, 11.0f, L::dimText(), true);
    text(toolMsg, 12.0f, L::warning());
    button(exeLocate, "LOCATE...", "Choose the sawblade-match (or sawblade-export) executable");
    button(poolLocate, "LOCATE...", "Choose the capture pool manifest (pool_manifest.json)");
    button(startBtn, "START MATCH", "Run the matcher on the selected DI take against the loaded song");
    button(cancelBtn, "CANCEL", "Stop the running job");
    startBtn.setColour(juce::TextButton::buttonColourId, L::saw());
    startBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff140a04));

    caption(capSource, "SOURCE");
    text(sourceLabel, 13.0f, L::text(), false, true);
    caption(capMode, "MODE");
    caption(capSize, "SIZE");
    caption(capDevice, "DEVICE");
    text(modeHint, 12.0f, L::dimText());
    text(deviceLabel, 12.0f, L::dimText());
    text(exportMsg, 12.0f, L::warning());
    button(noCab, "NO CAB", "Train the blend without the cab IR (exact for a live blend: both paths share one cab)", true);
    button(withCab, "WITH CAB", "Train the blend including the cab IR(s) (the only exact export for a studio blend)", true);
    button(feather, "FEATHER", "Smallest model", true);
    button(lite, "LITE", "Small model", true);
    button(standard, "STANDARD", "Standard-size model", true);
    for (auto* b : {&noCab, &withCab}) b->setRadioGroupId(81);
    for (auto* b : {&feather, &lite, &standard}) b->setRadioGroupId(82);
    standard.setToggleState(true, juce::dontSendNotification);
    button(exportBtn, "TRAIN EXPORT", "Train a NAM model of the loaded preset (runs sawblade-export)");
    button(exportCancel, "CANCEL", "Stop the running export");
    button(revealBtn, "REVEAL", "Show the exported model in the file manager");
    exportBtn.setColour(juce::TextButton::buttonColourId, L::saw());
    exportBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff140a04));
    for (auto* b : {&noCab, &withCab, &feather, &lite, &standard}) b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff6b2f12));

    caption(capProgress, "PROGRESS");
    text(stage, 15.0f, L::text(), false, true);
    text(message, 12.0f, L::dimText());
    text(eta, 12.0f, L::dimText(), true);
    owner.addAndMakeVisible(bar);
    caption(capResults, juce::String::fromUTF8("RESULTS \xc2\xb7 A-WEIGHTED ERROR"));
    text(auditionStatus, 12.0f, L::dimText());
    text(resultLabel, 13.0f, L::text(), false, true);
    text(licence, 11.0f, L::dimText());
    licence.setText("Built from TONE3000 captures: for your own use. Sharing an export needs permission from the capture creators and TONE3000.",
                    juce::dontSendNotification);
    results.setRowHeight(44);
    results.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff141210));
    results.setColour(juce::ListBox::outlineColourId, L::chipBorder());
    results.setOutlineThickness(1);
    results.setTitle("Match results");
    results.setTooltip("The matcher's best result and the alternatives, with their A-weighted error");
    owner.addAndMakeVisible(results);
    button(audition, "AUDITION", "Load the selected result into the rig, keeping the current preset for A/B");
    button(ab, "A / B", "Switch between the preset you had (A) and the auditioned result (B)");
    button(apply, "APPLY", "Make the selected result the current preset");
    button(revert, "REVERT", "Go back to the preset you had before the audition");
    apply.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a1a0e));
    apply.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffffb27a));

    wire();
    applyMode();
  }

  // ---- ListBoxModel ---------------------------------------------------------------------------------------
  int getNumRows() override { return static_cast<int>(rows.size()); }
  void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
    if (row < 0 || row >= static_cast<int>(rows.size())) return;
    const MatchCandidate& c = rows[static_cast<std::size_t>(row)];
    if (selected) {
      g.setColour(juce::Colour(0xff3a1c0b));
      g.fillRect(0, 0, w, h);
    }
    g.setColour(L::rule());
    g.fillRect(0, h - 1, w, 1);
    g.setColour(c.rank == 1 ? L::saw() : L::dimText());
    g.setFont(L::titleFont(15.0f));
    g.drawText(juce::String("#") + juce::String(c.rank), 12, 0, 40, h, juce::Justification::centredLeft);
    g.setColour(L::text());
    g.setFont(L::monoFont(15.0f));
    g.drawText(juce::String(c.errorDb, 2) + " dB", 56, 0, 100, h, juce::Justification::centredLeft);
    const juce::Colour tc = c.topology == "blend" ? L::saw() : c.topology == "single2" ? L::studio() : L::body();
    g.setColour(tc);
    g.setFont(L::labelFont(11.0f));
    g.drawText(juce::String(c.topology).toUpperCase() + (c.topology == "blend" ? "  " + juce::String(juce::roundToInt(c.blend * 100.0)) + "% BODY" : juce::String()), 166, 4, 150, 16,
               juce::Justification::centredLeft);
    if (c.rank == 1) {
      g.setColour(L::live());
      g.drawText("MATCHER'S CHOICE", 166, 22, 150, 16, juce::Justification::centredLeft);
    } else if (!c.presetExists) {
      g.setColour(L::error());
      g.drawText("PRESET FILE MISSING", 166, 22, 150, 16, juce::Justification::centredLeft);
    }
    g.setColour(c.presetExists ? L::text() : L::dimText());
    g.setFont(L::bodyFont(13.0f));
    g.drawFittedText(c.captures, 330, 4, w - 342, h - 8, juce::Justification::centredLeft, 2, 1.0f);
  }
  void selectedRowsChanged(int) override { owner.repaint(); }

  // ---- actions --------------------------------------------------------------------------------------------
  const MatchCandidate* selectedRow() const {
    const int r = results.getSelectedRow();
    return r >= 0 && r < static_cast<int>(rows.size()) ? &rows[static_cast<std::size_t>(r)] : nullptr;
  }

  void locate(bool pool, bool exportExe) {
    const juce::String what = pool ? "Choose the capture pool manifest" : exportExe ? "Choose sawblade-export" : "Choose sawblade-match";
    chooser = std::make_unique<juce::FileChooser>(what, juce::File(), pool ? "*.json" : "*");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, pool, exportExe](const juce::FileChooser& fc) {
      const juce::File f = fc.getResult();
      if (f == juce::File()) return;
      const fs::path p(f.getFullPathName().toStdString());
      if (pool) proc.matchSettings().setPoolManifest(p);
      else if (exportExe) proc.matchSettings().setExportExecutable(p);
      else proc.matchSettings().setMatchExecutable(p);
      refresh();
    });
  }

  std::string chosenMode() const { return withCab.getToggleState() ? "withcab" : "nocab"; }
  std::string chosenSize() const { return feather.getToggleState() ? "feather" : lite.getToggleState() ? "lite" : "standard"; }

  void startMatch() {
    const MatchPlan plan = planMatch(proc);
    std::string err;
    if (!plan.ok) {
      toolMsg.setText(plan.message, juce::dontSendNotification);
      return;
    }
    if (!proc.jobs().startMatch(plan.request, &err)) toolMsg.setText(err, juce::dontSendNotification);
    results.deselectAllRows();
    rows.clear();
    results.updateContent();
    appliedName.clear();
    refresh();
  }

  void startExport() {
    const ExportSource src = prepareExportSource(proc);
    if (!src.ok) {
      exportError = src.message;
      refresh();
      return;
    }
    ExportRequest r;
    r.preset = src.file;
    r.mode = chosenMode();
    r.size = chosenSize();
    if (const auto take = selectedTake(proc)) r.di = take->wav;
    std::string err;
    exportError = proc.jobs().startExport(r, &err) ? std::string() : err;
    refresh();
  }

  void wire() {
    closeBtn.onClick = [this] { owner.close(); };
    exeLocate.onClick = [this] { locate(false, mode == Mode::Export); };
    poolLocate.onClick = [this] { locate(true, false); };
    startBtn.onClick = [this] { startMatch(); };
    cancelBtn.onClick = [this] { proc.jobs().cancel(JobKind::Match); };
    exportBtn.onClick = [this] { startExport(); };
    exportCancel.onClick = [this] { proc.jobs().cancel(JobKind::Export); };
    noCab.onClick = [this] {
      exportModeChosen = true;
      refresh();
    };
    withCab.onClick = [this] {
      exportModeChosen = true;
      refresh();
    };
    revealBtn.onClick = [this] {
      const auto s = proc.jobs().snapshot(JobKind::Export);
      if (!s.outDir.empty() && owner.reveal) owner.reveal(juce::File(s.outDir.string()));
    };
    audition.onClick = [this] {
      if (const MatchCandidate* c = selectedRow()) {
        std::string err;
        if (!proc.audition().audition(c->preset, &err)) auditionStatus.setText("Could not load that result: " + err, juce::dontSendNotification);
      }
      refresh();
    };
    ab.onClick = [this] {
      proc.audition().toggleAB();
      refresh();
    };
    apply.onClick = [this] {
      const MatchCandidate* c = selectedRow();
      if (c == nullptr) return;
      const auto st = proc.audition().state();
      std::string err;
      if (!st.active || st.candidate != c->preset) proc.audition().audition(c->preset, &err);
      if (proc.audition().apply()) appliedName = "#" + std::to_string(c->rank) + " " + c->captures;
      refresh();
    };
    revert.onClick = [this] {
      proc.audition().revert();
      refresh();
    };
  }

  void applyMode() {
    const bool m = mode == Mode::Match;
    title.setText(m ? "MATCH" : "EXPORT NAM", juce::dontSendNotification);
    subtitle.setText(m ? "FIND THE BLEND THAT SOUNDS LIKE YOUR REFERENCE" : "TRAIN ONE MODEL OF THIS BLEND FOR A LOADER PEDAL", juce::dontSendNotification);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&capRef, &refName, &refStem, &capDi, &diName, &diOffset, &poolLabel, &poolLocate, &startBtn,
                                                                      &cancelBtn, &capResults, &results, &audition, &ab, &apply, &revert, &auditionStatus})
      c->setVisible(m);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&capSource, &sourceLabel, &capMode, &modeHint, &capSize, &capDevice, &deviceLabel, &exportMsg, &noCab,
                                                                      &withCab, &feather, &lite, &standard, &exportBtn, &exportCancel, &revealBtn, &resultLabel, &licence})
      c->setVisible(!m);
    capTools.setText(m ? juce::String::fromUTF8("3 \xc2\xb7 TOOLS") : juce::String("TOOLS"), juce::dontSendNotification);
    capTools.setVisible(true);
    exeLabel.setVisible(true);
    exeLocate.setVisible(true);
    toolMsg.setVisible(true);
    layout();
  }

  // ---- refresh --------------------------------------------------------------------------------------------
  void refresh() {
    const bool m = mode == Mode::Match;
    const JobKind kind = m ? JobKind::Match : JobKind::Export;
    const JobSnapshot snap = proc.jobs().snapshot(kind);
    const ToolCheck tools = proc.jobs().checkTools(kind);
    const auto st = proc.status();

    // tools
    exeLabel.setText((m ? "sawblade-match  " : "sawblade-export  ") + juce::String((m ? proc.matchSettings().matchExecutable() : proc.matchSettings().exportExecutable()).string()),
                     juce::dontSendNotification);
    poolLabel.setText("pool  " + juce::String(proc.matchSettings().poolManifest().string()), juce::dontSendNotification);
    exeLabel.setColour(juce::Label::textColourId, tools.missing == ToolCheck::Missing::Executable ? L::error() : L::dimText());
    poolLabel.setColour(juce::Label::textColourId, tools.missing == ToolCheck::Missing::Pool ? L::error() : L::dimText());
    juce::String toolText;
    if (!tools.ok()) toolText = tools.message;

    if (m) {
      const MatchPlan plan = planMatch(proc);
      refName.setText(proc.playAlong().settings().folder.empty() ? "No song loaded" : juce::String(fs::path(proc.playAlong().settings().folder).filename().string()),
                      juce::dontSendNotification);
      refName.setColour(juce::Label::textColourId, plan.reference.found ? L::text() : L::dimText());
      refStem.setText(plan.reference.found ? juce::String(plan.reference.label) : juce::String("Load a song in PLAY ALONG: its guitar stem is the reference."),
                      juce::dontSendNotification);
      diName.setText(plan.take ? juce::String(plan.take->name) : juce::String("No take selected"), juce::dontSendNotification);
      diName.setColour(juce::Label::textColourId, plan.take ? L::text() : L::dimText());
      juce::String diText;
      if (plan.take) {
        diText = juce::String(plan.take->lengthSeconds(), 1) + " s" + kDot + juce::String(plan.take->sampleRate / 1000.0, 1) + " kHz";
        if (plan.take->overruns > 0) diText += kDot + juce::String(static_cast<int>(plan.take->overruns)) + " overruns";
        diText += "\n" + juce::String(plan.offsetNote);
      } else {
        diText = "Record a take in PLAY ALONG and choose it with USE FOR MATCH.";
      }
      diOffset.setText(diText, juce::dontSendNotification);
      if (toolText.isEmpty() && !plan.ok) toolText = plan.message;
      startBtn.setEnabled(plan.ok && tools.ok() && !snap.active());
      cancelBtn.setEnabled(snap.active());
      poolLocate.setColour(juce::TextButton::buttonColourId, tools.missing == ToolCheck::Missing::Pool ? juce::Colour(0xff5a2a1c) : juce::Colour(0xff1b1916));
      exeLocate.setColour(juce::TextButton::buttonColourId, tools.missing == ToolCheck::Missing::Executable ? juce::Colour(0xff5a2a1c) : juce::Colour(0xff1b1916));
    } else {
      const auto src = proc.audition().currentCandidateFile();
      sourceLabel.setText(src ? juce::String("Matched preset: " + src->filename().string()) : juce::String("Current preset: " + st.presetName), juce::dontSendNotification);
      if (!exportModeChosen) {
        noCab.setToggleState(st.liveCompatible, juce::dontSendNotification);
        withCab.setToggleState(!st.liveCompatible, juce::dontSendNotification);
      }
      modeHint.setText(st.liveCompatible ? "LIVE blend: both paths share one cab, so NO CAB is exact."
                                         : "STUDIO blend (per-path cabs): only WITH CAB is exact. NO CAB is an approximation.",
                       juce::dontSendNotification);
      modeHint.setColour(juce::Label::textColourId, st.liveCompatible || withCab.getToggleState() ? L::dimText() : L::warning());
      deviceLabel.setText("auto (CUDA, MPS or CPU, whichever the trainer finds)", juce::dontSendNotification);
      exportMsg.setText(exportError, juce::dontSendNotification);
      exportBtn.setEnabled(tools.ok() && !snap.active());
      exportCancel.setEnabled(snap.active());
      const bool done = snap.state == JobState::Succeeded;
      revealBtn.setEnabled(done);
      resultLabel.setText(done ? "Model written to " + juce::String(snap.outDir.string()) : juce::String(), juce::dontSendNotification);
    }
    toolMsg.setText(toolText, juce::dontSendNotification);
    toolMsg.setColour(juce::Label::textColourId, tools.ok() ? L::warning() : L::error());

    // progress
    juce::String stageText = "Ready";
    juce::Colour sc = L::text();
    switch (snap.state) {
      case JobState::None: stageText = m ? "Ready to match" : "Ready to export"; sc = L::dimText(); break;
      case JobState::Starting: stageText = "Starting..."; sc = L::warning(); break;
      case JobState::Running: stageText = snap.progress.stage.empty() ? "Running" : juce::String(snap.progress.stage); break;
      case JobState::Succeeded: stageText = "Done"; sc = L::live(); break;
      case JobState::Failed: stageText = "Failed"; sc = L::error(); break;
      case JobState::Cancelled: stageText = "Cancelled"; sc = L::warning(); break;
    }
    stage.setText(stageText, juce::dontSendNotification);
    stage.setColour(juce::Label::textColourId, sc);
    juce::String msg = snap.progress.message;
    if (snap.state == JobState::Failed || snap.state == JobState::Cancelled) msg = snap.message;
    else if (snap.state == JobState::Succeeded) msg = m ? "Finished. Pick a result and AUDITION it in the rig; APPLY keeps it." : "Export finished.";
    message.setText(juce::String(msg.toStdString()), juce::dontSendNotification);
    message.setColour(juce::Label::textColourId, snap.state == JobState::Failed ? L::error() : L::dimText());
    const bool known = snap.progress.fraction >= 0.0;
    bar.set(snap.state == JobState::Succeeded ? 1.0 : std::max(0.0, snap.progress.fraction), snap.active() && !known,
            snap.state == JobState::Failed ? L::error() : snap.state == JobState::Succeeded ? L::live() : L::saw());
    juce::String e;
    if (snap.state != JobState::None) e = "elapsed " + clock(snap.elapsedSeconds);
    if (snap.active() && snap.progress.etaSeconds >= 0.0) e += kDot + "ETA " + clock(snap.progress.etaSeconds);
    if (snap.progress.bestErrorDb) e += kDot + "best error " + juce::String(*snap.progress.bestErrorDb, 2) + " dB";
    if (snap.active() && !snap.progressJson && m) e += kDot + "progress from the log (no ETA)";
    eta.setText(e, juce::dontSendNotification);

    // results + audition
    if (m) {
      const std::vector<MatchCandidate> now = snap.state == JobState::Succeeded ? snap.results : std::vector<MatchCandidate>{};
      bool changed = now.size() != rows.size();
      for (std::size_t i = 0; !changed && i < now.size(); ++i) changed = now[i].preset != rows[i].preset || now[i].errorDb != rows[i].errorDb;
      if (changed) {
        rows = now;
        results.updateContent();
        if (!rows.empty()) results.selectRow(0);
        results.repaint();
      }
      const auto as = proc.audition().state();
      const MatchCandidate* sel = selectedRow();
      audition.setEnabled(sel != nullptr && sel->presetExists);
      apply.setEnabled(sel != nullptr && sel->presetExists);
      ab.setEnabled(as.active);
      revert.setEnabled(as.active);
      ab.setButtonText(as.active ? (as.onCandidate ? "A / B  (B)" : "A / B  (A)") : "A / B");
      if (as.active)
        auditionStatus.setText(juce::String(std::string("A: ") + as.originalName + "    B: " + as.candidateName + "    now playing " + (as.onCandidate ? "B (the result)" : "A (your preset)")),
                               juce::dontSendNotification);
      else if (!appliedName.empty())
        auditionStatus.setText("Applied " + juce::String(appliedName), juce::dontSendNotification);
      else if (!rows.empty())
        auditionStatus.setText("AUDITION loads the selected result into the rig; A / B compares it with the preset you had.", juce::dontSendNotification);
      else
        auditionStatus.setText({}, juce::dontSendNotification);
    }
  }

  void layout() {
    constexpr int lx = 34, lw = 410, rx = 478, rw = 768;
    title.setBounds(124, 10, 400, 28);
    subtitle.setBounds(124, 38, 760, 16);
    closeBtn.setBounds(18, 14, 90, 34);

    const bool m = mode == Mode::Match;
    if (m) {
      capRef.setBounds(lx, 84, lw, 14);
      refName.setBounds(lx, 102, lw, 22);
      refStem.setBounds(lx, 126, lw, 34);
      capDi.setBounds(lx, 166, lw, 14);
      diName.setBounds(lx, 184, lw, 22);
      diOffset.setBounds(lx, 208, lw, 38);
      capTools.setBounds(lx, 262, lw, 14);
      exeLabel.setBounds(lx, 282, lw - 110, 18);
      exeLocate.setBounds(lx + lw - 100, 278, 100, 26);
      poolLabel.setBounds(lx, 312, lw - 110, 18);
      poolLocate.setBounds(lx + lw - 100, 308, 100, 26);
      toolMsg.setBounds(lx, 344, lw, 56);
      startBtn.setBounds(lx, 414, 200, 40);
      cancelBtn.setBounds(lx + 212, 414, 120, 40);
    } else {
      capSource.setBounds(lx, 84, lw, 14);
      sourceLabel.setBounds(lx, 102, lw, 22);
      capMode.setBounds(lx, 142, lw, 14);
      noCab.setBounds(lx, 160, 196, 34);
      withCab.setBounds(lx + 208, 160, 196, 34);
      modeHint.setBounds(lx, 200, lw, 36);
      capSize.setBounds(lx, 248, lw, 14);
      feather.setBounds(lx, 266, 126, 34);
      lite.setBounds(lx + 136, 266, 126, 34);
      standard.setBounds(lx + 272, 266, 132, 34);
      capDevice.setBounds(lx, 312, lw, 14);
      deviceLabel.setBounds(lx, 330, lw, 20);
      capTools.setBounds(lx, 366, lw, 14);
      exeLabel.setBounds(lx, 386, lw - 110, 18);
      exeLocate.setBounds(lx + lw - 100, 382, 100, 26);
      toolMsg.setBounds(lx, 414, lw, 52);
      exportMsg.setBounds(lx, 470, lw, 36);
      exportBtn.setBounds(lx, 512, 200, 40);
      exportCancel.setBounds(lx + 212, 512, 120, 40);
    }
    capProgress.setBounds(rx, 84, rw, 14);
    stage.setBounds(rx, 102, rw, 22);
    bar.setBounds(rx, 130, rw, 14);
    message.setBounds(rx, 152, rw, 34);
    eta.setBounds(rx, 190, rw, 18);
    if (m) {
      capResults.setBounds(rx, 226, rw, 14);
      results.setBounds(rx, 246, rw, 330);
      audition.setBounds(rx, 590, 130, 36);
      ab.setBounds(rx + 140, 590, 140, 36);
      apply.setBounds(rx + 290, 590, 150, 36);
      revert.setBounds(rx + 450, 590, 110, 36);
      auditionStatus.setBounds(rx, 636, rw, 40);
    } else {
      resultLabel.setBounds(rx, 236, rw, 40);
      revealBtn.setBounds(rx, 288, 160, 36);
      licence.setBounds(rx, 350, rw, 44);
    }
  }
};

MatchScreen::MatchScreen(SawbladeProcessor& p) : impl_(std::make_unique<Impl>(*this, p)) {
  setOpaque(true);
  setTitle("Match screen");
  reveal = [](const juce::File& f) { f.revealToUser(); };
  setSize(kWidth, kHeight);
  impl_->build();
  setVisible(false);
}

MatchScreen::~MatchScreen() = default;

MatchScreen::Mode MatchScreen::mode() const { return impl_->mode; }

void MatchScreen::open(Mode m) {
  impl_->mode = m;
  impl_->exportModeChosen = false;
  impl_->exportError.clear();
  impl_->proc.jobs().attachExisting();
  impl_->applyMode();
  setVisible(true);
  toFront(false);
  impl_->refresh();
}

void MatchScreen::close() {
  setVisible(false);
  if (onClose) onClose();
}

void MatchScreen::refresh() {
  if (isVisible()) impl_->refresh();
}

void MatchScreen::resized() { impl_->layout(); }

void MatchScreen::paint(juce::Graphics& g) {
  g.fillAll(L::background());
  g.setColour(L::panel());
  g.fillRect(0, 0, getWidth(), 60);
  g.setColour(L::rule());
  g.fillRect(0, 60, getWidth(), 1);
  // two columns
  g.setColour(L::panelDeep());
  g.fillRoundedRectangle(20.0f, 72.0f, 436.0f, static_cast<float>(getHeight() - 92), 6.0f);
  g.fillRoundedRectangle(464.0f, 72.0f, 796.0f, static_cast<float>(getHeight() - 92), 6.0f);
  g.setColour(L::rule());
  if (impl_->mode == Mode::Match) {
    g.fillRect(34, 254, 408, 1);
    g.fillRect(34, 160, 408, 1);
  }
}

}  // namespace sawblade::plugin
