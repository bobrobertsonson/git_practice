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

  juce::Label title, subtitle;
  juce::TextButton closeBtn;
  // match column
  juce::Label capRef, refName, refStem, capDi, diName, diOffset, capTools, exeLabel, poolLabel, toolMsg;
  juce::TextButton exeLocate, poolLocate, startBtn, cancelBtn;
  // progress + results
  juce::Label capProgress, stage, message, eta, capResults, auditionStatus;
  Bar bar;
  juce::ListBox results{"Match results", this};
  juce::TextButton audition, ab, apply, revert;

  std::vector<MatchCandidate> rows;
  std::unique_ptr<juce::FileChooser> chooser;
  std::string appliedName;

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
    button(exeLocate, "LOCATE...", "Choose the sawblade-match executable");
    button(poolLocate, "LOCATE...", "Choose the capture pool manifest (pool_manifest.json)");
    button(startBtn, "START MATCH", "Run the matcher on the selected DI take against the loaded song");
    button(cancelBtn, "CANCEL", "Stop the running job");
    startBtn.setColour(juce::TextButton::buttonColourId, L::saw());
    startBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff140a04));

    caption(capProgress, "PROGRESS");
    text(stage, 15.0f, L::text(), false, true);
    text(message, 12.0f, L::dimText());
    text(eta, 12.0f, L::dimText(), true);
    owner.addAndMakeVisible(bar);
    caption(capResults, juce::String::fromUTF8("RESULTS \xc2\xb7 A-WEIGHTED ERROR"));
    text(auditionStatus, 12.0f, L::dimText());
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
    title.setText("MATCH", juce::dontSendNotification);
    subtitle.setText("FIND THE BLEND THAT SOUNDS LIKE YOUR REFERENCE", juce::dontSendNotification);
    layout();
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

  void locate(bool pool) {
    const juce::String what = pool ? "Choose the capture pool manifest" : "Choose sawblade-match";
    chooser = std::make_unique<juce::FileChooser>(what, juce::File(), pool ? "*.json" : "*");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, pool](const juce::FileChooser& fc) {
      const juce::File f = fc.getResult();
      if (f == juce::File()) return;
      const fs::path p(f.getFullPathName().toStdString());
      if (pool) proc.matchSettings().setPoolManifest(p);
      else proc.matchSettings().setMatchExecutable(p);
      refresh();
    });
  }

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

  void wire() {
    closeBtn.onClick = [this] { owner.close(); };
    exeLocate.onClick = [this] { locate(false); };
    poolLocate.onClick = [this] { locate(true); };
    startBtn.onClick = [this] { startMatch(); };
    cancelBtn.onClick = [this] { proc.jobs().cancel(JobKind::Match); };
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

  // ---- refresh --------------------------------------------------------------------------------------------
  void refresh() {
    const JobSnapshot snap = proc.jobs().snapshot(JobKind::Match);
    const ToolCheck tools = proc.jobs().checkTools(JobKind::Match);

    // tools
    exeLabel.setText("sawblade-match  " + juce::String(proc.matchSettings().matchExecutable().string()), juce::dontSendNotification);
    poolLabel.setText("pool  " + juce::String(proc.matchSettings().poolManifest().string()), juce::dontSendNotification);
    exeLabel.setColour(juce::Label::textColourId, tools.missing == ToolCheck::Missing::Executable ? L::error() : L::dimText());
    poolLabel.setColour(juce::Label::textColourId, tools.missing == ToolCheck::Missing::Pool ? L::error() : L::dimText());
    juce::String toolText;
    if (!tools.ok()) toolText = tools.message;

    const MatchPlan plan = planMatch(proc);
    refName.setText(proc.playAlong().activeStemsDir().empty() ? "No song loaded" : juce::String(activeSongName(proc)),
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
    toolMsg.setText(toolText, juce::dontSendNotification);
    toolMsg.setColour(juce::Label::textColourId, tools.ok() ? L::warning() : L::error());

    // progress
    juce::String stageText = "Ready";
    juce::Colour sc = L::text();
    switch (snap.state) {
      case JobState::None: stageText = "Ready to match"; sc = L::dimText(); break;
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
    else if (snap.state == JobState::Succeeded) msg = "Finished. Pick a result and AUDITION it in the rig; APPLY keeps it.";
    message.setText(juce::String(msg.toStdString()), juce::dontSendNotification);
    message.setColour(juce::Label::textColourId, snap.state == JobState::Failed ? L::error() : L::dimText());
    const bool known = snap.progress.fraction >= 0.0;
    bar.set(snap.state == JobState::Succeeded ? 1.0 : std::max(0.0, snap.progress.fraction), snap.active() && !known,
            snap.state == JobState::Failed ? L::error() : snap.state == JobState::Succeeded ? L::live() : L::saw());
    juce::String e;
    if (snap.state != JobState::None) e = "elapsed " + clock(snap.elapsedSeconds);
    if (snap.active() && snap.progress.etaSeconds >= 0.0) e += kDot + "ETA " + clock(snap.progress.etaSeconds);
    if (snap.progress.bestErrorDb) e += kDot + "best error " + juce::String(*snap.progress.bestErrorDb, 2) + " dB";
    if (snap.active() && !snap.progressJson) e += kDot + "progress from the log (no ETA)";
    eta.setText(e, juce::dontSendNotification);

    // results + audition
    {
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
    capProgress.setBounds(rx, 84, rw, 14);
    stage.setBounds(rx, 102, rw, 22);
    bar.setBounds(rx, 130, rw, 14);
    message.setBounds(rx, 152, rw, 34);
    eta.setBounds(rx, 190, rw, 18);
    capResults.setBounds(rx, 226, rw, 14);
    results.setBounds(rx, 246, rw, 330);
    audition.setBounds(rx, 590, 130, 36);
    ab.setBounds(rx + 140, 590, 140, 36);
    apply.setBounds(rx + 290, 590, 150, 36);
    revert.setBounds(rx + 450, 590, 110, 36);
    auditionStatus.setBounds(rx, 636, rw, 40);
  }
};

MatchScreen::MatchScreen(SawbladeProcessor& p) : impl_(std::make_unique<Impl>(*this, p)) {
  setOpaque(true);
  setTitle("Match screen");
  setSize(kWidth, kHeight);
  impl_->build();
  setVisible(false);
}

MatchScreen::~MatchScreen() = default;

void MatchScreen::open() {
  impl_->proc.jobs().attachExisting();
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
  g.fillRect(34, 254, 408, 1);
  g.fillRect(34, 160, 408, 1);
}

}  // namespace sawblade::plugin
