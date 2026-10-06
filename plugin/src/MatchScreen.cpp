#include "MatchScreen.h"

#include <algorithm>
#include <cmath>

#include "MatchGlue.h"
#include "SawbladeLookAndFeel.h"
#include "SongInput.h"

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

constexpr int kRightX = 478;  // left edge of the progress / results column

// The take picker (section 2): the takes newest first, one row each. The chosen row is the match DI; clicking a row chooses it.
struct TakePicker : juce::ListBoxModel {
  std::vector<TakeInfo> takes;
  std::function<void(int)> onSelect;
  int getNumRows() override { return static_cast<int>(takes.size()); }
  void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
    if (row < 0 || row >= static_cast<int>(takes.size())) return;
    const TakeInfo& t = takes[static_cast<std::size_t>(row)];
    if (selected) {
      g.setColour(juce::Colour(0xff3a1c0b));
      g.fillRect(0, 0, w, h);
    }
    g.setColour(selected ? L::saw() : L::text());
    g.setFont(L::monoFont(12.0f));
    g.drawText(juce::String(t.name), 8, 0, w - 190, h, juce::Justification::centredLeft, true);
    g.setColour(L::dimText());
    g.drawText(juce::String(t.lengthSeconds(), 1) + " s", w - 180, 0, 56, h, juce::Justification::centredRight);
    const auto off = t.offsetMs();
    g.drawText(off ? juce::String("@ ") + juce::String(*off / 1000.0, 1) + " s" : juce::String("no song"), w - 118, 0, 70, h, juce::Justification::centredLeft);
    if (selected) {
      g.setColour(L::saw());
      g.setFont(L::labelFont(10.0f));
      g.drawText("DI", w - 40, 0, 32, h, juce::Justification::centredRight);
    }
  }
  void selectedRowsChanged(int row) override {
    if (onSelect) onSelect(row);
  }
};

}  // namespace

struct MatchScreen::Impl : juce::ListBoxModel {
  Impl(MatchScreen& o, SawbladeProcessor& p) : owner(o), proc(p) {}

  MatchScreen& owner;
  SawbladeProcessor& proc;

  juce::Label title, subtitle;
  juce::TextButton closeBtn;
  // match column
  juce::Label capRef, refName, refStem, refStatus, capDi, diName, diOffset, capTools, exeLabel, poolLabel, toolMsg;
  juce::TextButton exeLocate, poolLocate, startBtn, cancelBtn;
  // section 1 (the song): the two pickers, the separation progress / status, the install command for a missing model
  juce::TextButton songBtn, stemsBtn, sepCancel, fetchCopy;
  juce::TextEditor fetchField;
  Bar sepBar;
  bool fetchShown = false;
  song_input::PickNotice pickNotice;
  bool dropHover = false;
  // section 2 (the DI): REC / STOP, the take picker, and the slot IMPORT DI... takes (Task B, plugin part)
  juce::Label recTime, recNote, pickerEmpty, startNote;
  juce::TextButton recBtn;
  TakePicker picker;
  juce::ListBox pickerList{"Take picker", &picker};
  bool syncingPicker = false;
  std::uint64_t seenTakesVersion = ~std::uint64_t{0};
  int takesTick = 0;
  bool recSeen = false;
  std::string newestAtRecStart, chosenAtRecStart;
  // progress + results
  juce::Label capProgress, stage, message, eta, capResults, auditionStatus;
  Bar bar;
  juce::ListBox results{"Match results", this};
  juce::TextButton audition, ab, apply, revert, applyRefined;
  // two-pass MATCH: PREVIEW / REFINED badge, REFINING... with a thin bar, and the auto-refine setting
  juce::Label previewBadge, refiningLabel;
  Bar refineBar;
  juce::ToggleButton autoRefine;

  // A row of the results list: a candidate, or a section header (REFINED / PREVIEW). `tag` is its badge.
  struct Row {
    MatchCandidate c;
    bool header = false;
    std::string text, tag;
    bool sameAs(const Row& o) const { return header == o.header && text == o.text && tag == o.tag && c.preset == o.c.preset && c.errorDb == o.c.errorDb && c.rank == o.c.rank; }
  };
  std::vector<Row> rows;
  int lastGoodRow = -1;
  std::unique_ptr<juce::FileChooser> chooser, songChooser;
  // Where IMPORT DI... goes (v0.2.1 Task B, plugin part): section 2, right of REC / STOP, same row. Nothing is drawn here yet.
  juce::Rectangle<int> importSlot;
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

    caption(capRef, juce::String::fromUTF8("1 \xc2\xb7 REFERENCE SONG"));
    button(songBtn, juce::String::fromUTF8("SONG FILE\xe2\x80\xa6"), "Choose the reference song (wav, mp3, flac, m4a, aif, aac, ogg): it is separated into stems on this machine, once, then cached. You can also drop a song file or a stems folder on this screen.");
    button(stemsBtn, juce::String::fromUTF8("STEMS FOLDER\xe2\x80\xa6"), "Choose a folder of already separated stems (drums, bass, vocals, other, guitar as .wav or .flac).");
    songBtn.setTitle(juce::String::fromUTF8("CHOOSE SONG FILE\xe2\x80\xa6"));  // the accessible name is the full action
    stemsBtn.setTitle(juce::String::fromUTF8("CHOOSE STEMS FOLDER\xe2\x80\xa6"));
    button(sepCancel, "CANCEL", "Cancel the separation");
    sepCancel.setTitle("CANCEL SEPARATION");  // unique: the match's own CANCEL button keeps the title "CANCEL"
    sepCancel.setVisible(false);
    button(fetchCopy, "COPY", "Copy the install command to the clipboard");
    fetchCopy.setVisible(false);
    text(refName, 15.0f, L::text(), false, true);
    text(refStem, 12.0f, L::dimText());
    text(refStatus, 11.5f, L::dimText());
    song_input::styleFetchField(fetchField);
    fetchField.setVisible(false);
    owner.addChildComponent(fetchField);
    owner.addChildComponent(sepBar);
    caption(capDi, juce::String::fromUTF8("2 \xc2\xb7 YOUR DI"));
    button(recBtn, "REC", "Record the clean input (before the gate) to a take. Press again to stop. The take becomes the DI for the match. Takes are saved in the takes folder with a sidecar that stores where the song was.");
    recBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff4a1712));
    recBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffffb0a0));
    text(recTime, 15.0f, L::dimText(), true);
    recTime.setJustificationType(juce::Justification::centredLeft);
    text(recNote, 11.0f, L::dimText());
    picker.onSelect = [this](int row) {
      if (syncingPicker || row < 0 || row >= static_cast<int>(picker.takes.size())) return;
      chooseTakeForMatch(proc, picker.takes[static_cast<std::size_t>(row)].name);  // another take than before also cancels a running refinement
      refresh();
    };
    pickerList.setRowHeight(22);
    pickerList.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff141210));
    pickerList.setColour(juce::ListBox::outlineColourId, L::chipBorder());
    pickerList.setOutlineThickness(1);
    pickerList.setTitle("Take picker");
    pickerList.setTooltip("Your recorded DI takes, newest first. The chosen take is the DI the match runs on.");
    pickerEmpty.setFont(L::bodyFont(12.0f));
    pickerEmpty.setColour(juce::Label::textColourId, L::dimText());
    pickerEmpty.setText("No takes yet. Press REC and play.", juce::dontSendNotification);
    pickerEmpty.setJustificationType(juce::Justification::centred);
    pickerEmpty.setInterceptsMouseClicks(false, false);
    text(diName, 15.0f, L::text(), false, true);
    text(diOffset, 12.0f, L::dimText());
    text(startNote, 11.5f, L::dimText());
    caption(capTools, juce::String::fromUTF8("3 \xc2\xb7 TOOLS"));
    text(exeLabel, 11.0f, L::dimText(), true);
    text(poolLabel, 11.0f, L::dimText(), true);
    text(toolMsg, 12.0f, L::warning());
    button(exeLocate, "LOCATE...", "Choose the sawblade-match executable");
    button(poolLocate, "LOCATE...", "Choose the capture pool manifest (pool_manifest.json)");
    button(startBtn, "START MATCH", "Run the matcher on the selected DI take against the loaded song");
    button(cancelBtn, "CANCEL", "Stop the running job (during the background refinement: only the refinement)");
    autoRefine.setButtonText("AUTO-REFINE");
    autoRefine.setTitle("Auto-refine");
    autoRefine.setTooltip("After the quick PREVIEW pass, run the thorough pass in the background on the same take and offer its result. Nothing is ever loaded by itself.");
    autoRefine.setColour(juce::ToggleButton::textColourId, L::text());
    autoRefine.setColour(juce::ToggleButton::tickColourId, L::saw());
    owner.addAndMakeVisible(autoRefine);
    for (juce::Label* l : {&previewBadge, &refiningLabel}) {
      l->setFont(L::labelFont(11.0f));
      l->setInterceptsMouseClicks(false, false);
      owner.addAndMakeVisible(*l);
    }
    refiningLabel.setJustificationType(juce::Justification::centredRight);
    refiningLabel.setColour(juce::Label::textColourId, L::saw());
    owner.addAndMakeVisible(refineBar);
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
    button(applyRefined, "APPLY REFINED BEST", "Load the refined (thorough) best result and make it the current preset. This is the only way a refined result reaches the rig, other than choosing one of its candidates.");
    applyRefined.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1d3a22));
    applyRefined.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff9be5a4));
    apply.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a1a0e));
    apply.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffffb27a));

    // The take picker goes last in the child order: the results list stays the screen's first ListBox (tests and tools find it so).
    owner.addAndMakeVisible(pickerList);
    owner.addAndMakeVisible(pickerEmpty);  // above the (opaque) list, or it would be hidden by it
    wire();
    title.setText("MATCH", juce::dontSendNotification);
    subtitle.setText("FIND THE BLEND THAT SOUNDS LIKE YOUR REFERENCE", juce::dontSendNotification);
    layout();
  }

  // ---- ListBoxModel ---------------------------------------------------------------------------------------
  int getNumRows() override { return static_cast<int>(rows.size()); }
  void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
    if (row < 0 || row >= static_cast<int>(rows.size())) return;
    const Row& r = rows[static_cast<std::size_t>(row)];
    if (r.header) {
      g.setColour(juce::Colour(0xff1e2a1f));
      g.fillRect(0, 0, w, h);
      g.setColour(L::rule());
      g.fillRect(0, h - 1, w, 1);
      g.setColour(r.tag == "REFINED" ? L::live() : L::saw());
      g.setFont(L::titleFont(13.0f));
      g.drawText(juce::String(r.text), 12, 0, w - 24, h, juce::Justification::centredLeft);
      return;
    }
    const MatchCandidate& c = r.c;
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
    g.drawText(juce::String(c.errorDb, 2) + " dB", 56, r.tag.empty() ? 0 : 3, 100, r.tag.empty() ? h : h - 14, juce::Justification::centredLeft);
    if (!r.tag.empty()) {
      g.setColour(r.tag == "REFINED" ? L::live() : L::saw());
      g.setFont(L::labelFont(10.0f));
      g.drawText(juce::String(r.tag), 56, h - 17, 100, 14, juce::Justification::centredLeft);
    }
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
  void selectedRowsChanged(int row) override {
    // A section header is not a candidate: the selection goes back to the last candidate that was selected.
    if (row >= 0 && row < static_cast<int>(rows.size()) && rows[static_cast<std::size_t>(row)].header) {
      if (lastGoodRow >= 0 && lastGoodRow < static_cast<int>(rows.size()) && !rows[static_cast<std::size_t>(lastGoodRow)].header) results.selectRow(lastGoodRow);
      else results.deselectAllRows();
    } else if (row >= 0) {
      lastGoodRow = row;
    }
    owner.repaint();
  }

  // ---- actions --------------------------------------------------------------------------------------------
  const MatchCandidate* selectedRow() const {
    const int r = results.getSelectedRow();
    return r >= 0 && r < static_cast<int>(rows.size()) && !rows[static_cast<std::size_t>(r)].header ? &rows[static_cast<std::size_t>(r)].c : nullptr;
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
    lastGoodRow = -1;
    results.updateContent();
    appliedName.clear();
    refresh();
  }

  void wire() {
    closeBtn.onClick = [this] { owner.close(); };
    songBtn.onClick = [this] { owner.chooseSongFile(); };
    stemsBtn.onClick = [this] { owner.chooseStemsFolder(); };
    sepCancel.onClick = [this] { proc.playAlong().cancelSeparation(); };
    fetchCopy.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(fetchField.getText()); };
    recBtn.onClick = [this] {
      toggleRecording(proc);  // the same recorder, and the same rules, as the take band
      refresh();
    };
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
    applyRefined.onClick = [this] {
      std::string err;
      if (applyRefinedBest(proc, &err)) {
        const auto r = proc.jobs().refineSnapshot();
        appliedName = "#1 (refined best) " + r.results.front().captures;
      } else {
        auditionStatus.setText("Could not apply the refined best: " + err, juce::dontSendNotification);
      }
      refresh();
    };
    autoRefine.onClick = [this] { proc.matchSettings().setAutoRefine(autoRefine.getToggleState()); };
  }

  // ---- section 1: the song --------------------------------------------------------------------------------
  void refreshSong(const MatchPlan& plan) {
    auto& pa = proc.playAlong();
    const auto st = pa.loadStatus();
    const auto s = pa.settings();
    const auto line = song_input::statusLine(st, pa.standalone(), s.hostSync, pickNotice.active(), "Choose a song file or a stems folder, or drop one here.");
    refName.setText(pa.activeStemsDir().empty() ? "No song loaded" : juce::String(activeSongName(proc)), juce::dontSendNotification);
    refName.setColour(juce::Label::textColourId, plan.reference.found ? L::text() : L::dimText());
    refStem.setText(plan.reference.found ? juce::String("Reference: ") + juce::String(plan.reference.label) : juce::String(), juce::dontSendNotification);
    refStatus.setText(line.text, juce::dontSendNotification);
    refStatus.setTooltip(line.tooltip);
    refStatus.setColour(juce::Label::textColourId, line.colour);
    bool relayout = line.showFetch != fetchShown;
    if (line.showFetch && fetchField.getText() != line.fetchCommand) {
      fetchField.setText(line.fetchCommand, juce::dontSendNotification);
      relayout = true;
    }
    fetchShown = line.showFetch;
    if (relayout) {
      layoutSongStatus();
      song_input::fitFetchFont(fetchField);
    }
    fetchField.setVisible(line.showFetch);
    fetchCopy.setVisible(line.showFetch);
    fetchCopy.setTooltip(line.fetchTooltip);
    sepBar.setVisible(line.separating);
    sepBar.set(st.separationFraction, st.separationFraction <= 0.0, L::saw());
    sepCancel.setVisible(line.separating);
    songBtn.setEnabled(!line.separating);
    stemsBtn.setEnabled(!line.separating);
  }

  // ---- section 2: the DI -----------------------------------------------------------------------------------
  void rescanTakes() {
    auto& r = proc.recorder();
    seenTakesVersion = r.takesVersion();
    auto fresh = r.listTakes();
    bool same = fresh.size() == picker.takes.size();
    for (std::size_t i = 0; same && i < fresh.size(); ++i) same = fresh[i].name == picker.takes[i].name && fresh[i].lengthSamples == picker.takes[i].lengthSamples;
    if (same) return;
    picker.takes = std::move(fresh);
    syncingPicker = true;
    pickerList.updateContent();
    syncingPicker = false;
    pickerList.repaint();
  }

  // Rescans the takes and lets a finished recording become the DI. Runs before the plan is made, so the plan sees the new choice.
  void followRecorder() {
    auto& r = proc.recorder();
    const auto state = r.state();
    // Takes on disk: rescanned when the recorder changed them, and every ~10 s (the timer runs at 4 Hz; other instances / the file manager).
    if (r.takesVersion() != seenTakesVersion || (++takesTick % 40) == 0) rescanTakes();
    // A take that finished since the screen last looked becomes the DI (the user pressed REC to match with it).
    if (state != TakeRecorder::State::Idle && !recSeen) {
      recSeen = true;
      newestAtRecStart = picker.takes.empty() ? std::string() : picker.takes.front().name;
      chosenAtRecStart = proc.matchSettings().selectedTake();
    } else if (state == TakeRecorder::State::Idle && recSeen) {
      recSeen = false;
      rescanTakes();
      // The new take becomes the DI unless the user chose another one while recording.
      if (!picker.takes.empty() && picker.takes.front().name != newestAtRecStart && proc.matchSettings().selectedTake() == chosenAtRecStart)
        chooseTakeForMatch(proc, picker.takes.front().name);
    }
  }

  void refreshDi(const MatchPlan& plan) {
    auto& r = proc.recorder();
    const auto state = r.state();
    // The chosen row follows the match settings (a pick in the take band shows here too).
    const std::string chosen = proc.matchSettings().selectedTake();
    int want = -1;
    for (std::size_t i = 0; i < picker.takes.size(); ++i)
      if (picker.takes[i].name == chosen) want = static_cast<int>(i);
    if (pickerList.getSelectedRow() != want) {
      syncingPicker = true;
      if (want >= 0) pickerList.selectRow(want, true);
      else pickerList.deselectAllRows();
      syncingPicker = false;
    }
    pickerEmpty.setVisible(picker.takes.empty());

    const juce::String why = juce::String(recordUnavailableReason(proc));
    recBtn.setButtonText(state == TakeRecorder::State::Idle ? "REC" : "STOP");
    recBtn.setTitle(state == TakeRecorder::State::Idle ? "REC" : "STOP");
    recBtn.setEnabled(why.isEmpty());
    const juce::String t = juce::String(recordStateText(proc));
    recTime.setText(t, juce::dontSendNotification);
    recTime.setColour(juce::Label::textColourId, state == TakeRecorder::State::Recording ? juce::Colour(0xffff6a5a) : L::dimText());
    juce::String note = "REC records your clean input (before the gate). The newest take becomes the DI.";
    juce::Colour nc = L::dimText();
    if (const std::string err = r.lastError(); !err.empty()) {
      note = juce::String(err);
      nc = L::error();
    } else if (why.isNotEmpty()) {
      note = why;
      nc = L::warning();
    } else if (state != TakeRecorder::State::Idle) {
      note = "Recording the clean input, before the gate.";
      if (r.overruns() > 0) {
        note = juce::String(static_cast<int>(r.overruns())) + " overruns: the disk could not keep up; the gaps are filled with silence.";
        nc = L::warning();
      }
    }
    recNote.setText(note, juce::dontSendNotification);
    recNote.setColour(juce::Label::textColourId, nc);
    recBtn.setTooltip(why.isNotEmpty() ? why : juce::String("Record the clean input (before the gate) to a take. Press again to stop. The take becomes the DI for the match."));

    // The chosen take is shown whether or not a song is loaded (planMatch stops at "no song" before it looks at the take).
    const std::optional<TakeInfo> take = selectedTake(proc);
    diName.setText(take ? juce::String(take->name) : juce::String("No DI chosen"), juce::dontSendNotification);
    diName.setColour(juce::Label::textColourId, take ? L::text() : L::dimText());
    juce::String diText;
    if (take) {
      diText = juce::String(take->lengthSeconds(), 1) + " s" + kDot + juce::String(take->sampleRate / 1000.0, 1) + " kHz";
      if (take->overruns > 0) diText += kDot + juce::String(static_cast<int>(take->overruns)) + " overruns";
      if (plan.take) diText += "\n" + juce::String(plan.offsetNote);
    } else {
      diText = "Record a take with REC, or pick one above.";
    }
    diOffset.setText(diText, juce::dontSendNotification);
  }

  // ---- refresh --------------------------------------------------------------------------------------------
  void refresh() {
    const JobSnapshot snap = proc.jobs().snapshot(JobKind::Match);
    const JobSnapshot refine = proc.jobs().refineSnapshot();
    const bool quickPass = snap.pass == "quick";
    const bool refineActive = refine.active() || (quickPass && snap.refinePending);
    const bool refineReady = refine.state == JobState::Succeeded && !refine.results.empty();
    const bool promoted = appliedQuickIsRefinedBest(proc, snap, refine);
    const ToolCheck tools = proc.jobs().checkTools(JobKind::Match);

    // tools
    exeLabel.setText("sawblade-match  " + juce::String(proc.matchSettings().matchExecutable().string()), juce::dontSendNotification);
    poolLabel.setText("pool  " + juce::String(proc.matchSettings().poolManifest().string()), juce::dontSendNotification);
    exeLabel.setColour(juce::Label::textColourId, tools.missing == ToolCheck::Missing::Executable ? L::error() : L::dimText());
    poolLabel.setColour(juce::Label::textColourId, tools.missing == ToolCheck::Missing::Pool ? L::error() : L::dimText());
    juce::String toolText;
    if (!tools.ok()) toolText = tools.message;

    followRecorder();
    const MatchPlan plan = planMatch(proc);
    refreshSong(plan);
    refreshDi(plan);
    const bool haveSong = !proc.playAlong().activeStemsDir().empty();
    const auto loadSt = proc.playAlong().loadStatus();
    const bool separating = loadSt.state == PlayAlong::LoadStatus::State::Separating;
    if (toolText.isEmpty() && !plan.ok && haveSong && plan.take) toolText = plan.message;  // e.g. no audio in the stems folder
    startBtn.setEnabled(plan.ok && tools.ok() && !snap.active());
    // What START MATCH is waiting for (the song first), so a disabled button never needs explaining elsewhere.
    juce::String startText;
    if (!haveSong) startText = separating ? "wait for the separation to finish" : "load a song first";
    else if (!plan.take) startText = "record or import a DI";
    else if (!plan.ok) startText = "fix the reference song";
    startNote.setText(startText, juce::dontSendNotification);
    startNote.setColour(juce::Label::textColourId, startText.isNotEmpty() ? L::warning() : L::dimText());
    startBtn.setTooltip(startText.isNotEmpty() ? "START MATCH needs: " + startText : juce::String("Run the matcher on the selected DI take against the loaded song"));
    cancelBtn.setEnabled(snap.active() || refineActive);
    const char* cancelText = !snap.active() && refineActive ? "CANCEL REFINE" : "CANCEL";
    cancelBtn.setButtonText(cancelText);
    cancelBtn.setTitle(cancelText);
    autoRefine.setToggleState(proc.matchSettings().autoRefine(), juce::dontSendNotification);
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
      case JobState::Succeeded: stageText = quickPass ? "Done (quick pass)" : "Done"; sc = L::live(); break;
      case JobState::Failed: stageText = "Failed"; sc = L::error(); break;
      case JobState::Cancelled: stageText = "Cancelled"; sc = L::warning(); break;
    }
    stage.setText(stageText, juce::dontSendNotification);
    stage.setColour(juce::Label::textColourId, sc);
    juce::String msg = snap.progress.message;
    if (snap.state == JobState::Failed || snap.state == JobState::Cancelled) msg = snap.message;
    else if (snap.state == JobState::Succeeded && quickPass) {
      if (refineActive) msg = "Quick pass finished: these are PREVIEW results. The thorough pass is refining them in the background; its result is offered, never loaded by itself.";
      else if (refineReady) msg = "Quick pass finished. The thorough pass is ready (REFINED section).";
      else if (!snap.refineNote.empty()) msg = snap.refineNote;
      else if (!proc.matchSettings().autoRefine()) msg = "Quick pass finished: these are PREVIEW results. Auto-refine is off, so there is no thorough pass.";
      else msg = "Quick pass finished: these are PREVIEW results.";
    } else if (snap.state == JobState::Succeeded) msg = "Finished. Pick a result and AUDITION it in the rig; APPLY keeps it.";
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
      // The list: a REFINED section first when the thorough pass is ready, then the quick (PREVIEW) or single-run results.
      const auto applied = proc.audition().appliedCandidateFile();
      std::vector<Row> now;
      if (refineReady) {
        Row h;
        h.header = true;
        h.tag = "REFINED";
        h.text = "REFINED  -  refined result ready (thorough pass)";
        now.push_back(h);
        for (const auto& c : refine.results) now.push_back({c, false, {}, "REFINED"});
        if (snap.state == JobState::Succeeded && quickPass) {
          Row q;
          q.header = true;
          q.tag = "PREVIEW";
          q.text = "PREVIEW  -  quick pass";
          now.push_back(q);
        }
      }
      if (snap.state == JobState::Succeeded)
        for (const auto& c : snap.results) {
          std::string tag;
          if (quickPass) tag = promoted && applied && *applied == c.preset ? "REFINED" : "PREVIEW";
          now.push_back({c, false, {}, tag});
        }
      bool changed = now.size() != rows.size();
      for (std::size_t i = 0; !changed && i < now.size(); ++i) changed = !now[i].sameAs(rows[i]);
      if (changed) {
        // Keep the user's selection (by candidate) when the list grows a REFINED section above it.
        fs::path keepPreset;
        if (const MatchCandidate* sel = selectedRow()) keepPreset = sel->preset;
        const bool hadRows = !rows.empty();
        rows = std::move(now);
        results.updateContent();
        int pick = -1;
        for (std::size_t i = 0; i < rows.size(); ++i)
          if (!rows[i].header && !keepPreset.empty() && rows[i].c.preset == keepPreset) {
            pick = static_cast<int>(i);
            break;
          }
        if (pick < 0 && !hadRows)
          for (std::size_t i = 0; i < rows.size(); ++i)
            if (!rows[i].header) {
              pick = static_cast<int>(i);
              break;
            }
        if (pick >= 0) results.selectRow(pick);
        else results.deselectAllRows();
        lastGoodRow = pick;  // the rows moved: remap, so a click on a header returns to the right candidate
        results.repaint();
      }
      // badges and the refinement bar on the results header
      juce::String badge;
      juce::Colour bc = L::saw();
      if (promoted) {
        badge = "REFINED";
        bc = L::live();
      } else if (refineReady) {
        badge = "REFINED READY";
        bc = L::live();
      } else if (quickPass && snap.state == JobState::Succeeded) {
        badge = "PREVIEW";
      }
      previewBadge.setText(badge, juce::dontSendNotification);
      previewBadge.setColour(juce::Label::textColourId, bc);
      previewBadge.setTooltip(promoted ? "The applied preset is the same chain as the thorough pass's best." : refineReady ? "The thorough pass finished: see the REFINED section." : "Quick pass: preview results.");
      // A thorough pass that ended without a result: one line on the header (the quick results stay).
      const bool refineEnded = !refineActive && quickPass && (refine.state == JobState::Failed || refine.state == JobState::Cancelled);
      // An old quick job that was not refined (too old, or it could not start) says why.
      const bool refineNoted = !refineActive && !refineEnded && quickPass && snap.state == JobState::Succeeded && refine.state == JobState::None && !snap.refineNote.empty();
      refiningLabel.setVisible(refineActive || refineEnded || refineNoted);
      refineBar.setVisible(refineActive);
      refiningLabel.setBounds(refineActive ? juce::Rectangle<int>(kRightX + 440, 224, 150, 18) : juce::Rectangle<int>(kRightX + 430, 224, 338, 18));
      if (refineEnded) {
        juce::String note = refine.state == JobState::Cancelled ? juce::String("Refinement cancelled") : "Refinement failed: " + juce::String(refine.message);
        refiningLabel.setText(note, juce::dontSendNotification);
        refiningLabel.setColour(juce::Label::textColourId, refine.state == JobState::Failed ? L::error() : L::warning());
      }
      if (refineNoted) {
        refiningLabel.setText(juce::String(snap.refineNote), juce::dontSendNotification);
        refiningLabel.setColour(juce::Label::textColourId, L::warning());
      }
      if (refineActive) {
        refiningLabel.setColour(juce::Label::textColourId, L::saw());
        const double f = refine.progress.fraction;
        refiningLabel.setText(juce::String::fromUTF8("REFINING\xe2\x80\xa6") + (f >= 0.0 ? " " + juce::String(juce::roundToInt(std::min(1.0, f) * 100.0)) + "%" : juce::String()),
                              juce::dontSendNotification);
        refineBar.set(std::max(0.0, f), f < 0.0, L::saw());
      }
      applyRefined.setEnabled(refineReady && refine.results.front().presetExists && !promoted);
      applyRefined.setVisible(refineReady);

      const auto as = proc.audition().state();
      const MatchCandidate* sel = selectedRow();
      audition.setEnabled(sel != nullptr && sel->presetExists);
      apply.setEnabled(sel != nullptr && sel->presetExists);
      ab.setEnabled(as.active);
      revert.setEnabled(as.active);
      ab.setButtonText(as.active ? (as.onCandidate ? "A / B  (B)" : "A / B  (A)") : "A / B");
      juce::String status;
      if (as.active)
        status = juce::String(std::string("A: ") + as.originalName + "    B: " + as.candidateName + "    now playing " + (as.onCandidate ? "B (the result)" : "A (your preset)"));
      else if (!appliedName.empty())
        status = "Applied " + juce::String(appliedName);
      else if (!rows.empty())
        status = "AUDITION loads the selected result into the rig; A / B compares it with the preset you had.";
      if (promoted)
        status += (status.isEmpty() ? "" : "\n") + juce::String("The applied preset is the same chain as the refined best: it now counts as REFINED. Nothing was reloaded.");
      else if (refineReady)
        status += (status.isEmpty() ? "" : "\n") + juce::String("Refined result ready: nothing was loaded. Choose a refined candidate, or APPLY REFINED BEST.");
      auditionStatus.setText(status, juce::dontSendNotification);
    }
  }

  // The status line under the song name; for a missing model it shortens to make room for COPY and the command field.
  void layoutSongStatus() {
    constexpr int lx = 34, lw = 410;
    if (fetchShown) {
      refStatus.setBounds(lx, 172, lw - 62, 28);
      fetchCopy.setBounds(lx + lw - 56, 174, 56, 20);
      fetchField.setBounds(lx, 204, lw, 38);  // ends at y 242; the rule below sits at 246
    } else {
      refStatus.setBounds(lx, 172, lw, 30);
    }
    sepBar.setBounds(lx, 208, lw, 8);
  }

  void layout() {
    constexpr int lx = 34, lw = 410, rx = kRightX, rw = 768;
    title.setBounds(124, 10, 400, 28);
    subtitle.setBounds(124, 38, 760, 16);
    closeBtn.setBounds(18, 14, 90, 34);

    // 1 · REFERENCE SONG  (y 80 .. 244; also the drop highlight)
    capRef.setBounds(lx, 80, lw, 14);
    songBtn.setBounds(lx, 98, 128, 28);
    stemsBtn.setBounds(lx + 136, 98, 150, 28);
    sepCancel.setBounds(lx + lw - 112, 98, 112, 28);
    refName.setBounds(lx, 132, lw, 22);
    refStem.setBounds(lx, 154, lw, 16);
    layoutSongStatus();
    // 2 · YOUR DI  (y 254 .. 490)
    capDi.setBounds(lx, 254, lw, 14);
    recBtn.setBounds(lx, 272, 96, 30);
    recTime.setBounds(lx + 106, 272, 140, 30);
    importSlot = {lx + 256, 272, lw - 256, 30};  // reserved for IMPORT DI... (Task B, plugin part)
    recNote.setBounds(lx, 306, lw, 28);
    pickerList.setBounds(lx, 336, lw, 88);
    pickerEmpty.setBounds(lx, 336, lw, 88);
    diName.setBounds(lx, 428, lw, 20);
    diOffset.setBounds(lx, 448, lw, 40);
    // 3 · TOOLS + START MATCH
    capTools.setBounds(lx, 498, lw, 14);
    exeLabel.setBounds(lx, 516, lw - 110, 18);
    exeLocate.setBounds(lx + lw - 100, 512, 100, 26);
    poolLabel.setBounds(lx, 546, lw - 110, 18);
    poolLocate.setBounds(lx + lw - 100, 542, 100, 26);
    toolMsg.setBounds(lx, 572, lw, 30);
    autoRefine.setBounds(lx, 604, lw, 24);
    startBtn.setBounds(lx, 634, 200, 38);
    cancelBtn.setBounds(lx + 212, 634, 120, 38);
    startNote.setBounds(lx, 676, lw, 32);
    capProgress.setBounds(rx, 84, rw, 14);
    stage.setBounds(rx, 102, rw, 22);
    bar.setBounds(rx, 130, rw, 14);
    message.setBounds(rx, 152, rw, 34);
    eta.setBounds(rx, 190, rw, 18);
    capResults.setBounds(rx, 226, 250, 14);
    previewBadge.setBounds(rx + 258, 224, 170, 18);
    refiningLabel.setBounds(rx + 440, 224, 150, 18);
    refineBar.setBounds(rx + 596, 229, rw - 596, 8);
    results.setBounds(rx, 246, rw, 330);
    audition.setBounds(rx, 590, 130, 36);
    ab.setBounds(rx + 140, 590, 140, 36);
    apply.setBounds(rx + 290, 590, 150, 36);
    revert.setBounds(rx + 450, 590, 110, 36);
    applyRefined.setBounds(rx + 572, 590, 196, 36);
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
  impl_->rescanTakes();
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
  g.fillRect(34, 246, 408, 1);  // between 1 · REFERENCE SONG and 2 · YOUR DI
  g.fillRect(34, 492, 408, 1);  // between 2 · YOUR DI and 3 · TOOLS
  if (impl_->dropHover) {       // a song file or a stems folder is being dragged over the screen: section 1 takes it
    g.setColour(L::saw().withAlpha(0.18f));
    g.fillRoundedRectangle(24.0f, 76.0f, 428.0f, 168.0f, 6.0f);
    g.setColour(L::saw());
    g.drawRoundedRectangle(24.5f, 76.5f, 427.0f, 167.0f, 6.0f, 1.5f);
  }
}

bool MatchScreen::handlePicked(song_input::Action a, const juce::File& f) {
  const bool loaded = song_input::handlePicked(impl_->proc.playAlong(), a, f, impl_->pickNotice);
  if (f != juce::File()) impl_->refresh();  // a rejected pick, a refusal and the separation progress show at once
  return loaded;
}

void MatchScreen::chooseSongFile() {
  song_input::launchChooser(impl_->songChooser, song_input::Action::SongFile, [this](const juce::File& f) { handlePicked(song_input::Action::SongFile, f); });
}

void MatchScreen::chooseStemsFolder() {
  song_input::launchChooser(impl_->songChooser, song_input::Action::StemsFolder, [this](const juce::File& f) { handlePicked(song_input::Action::StemsFolder, f); });
}

bool MatchScreen::isInterestedInFileDrag(const juce::StringArray& files) { return song_input::isLoadableDrop(files); }

void MatchScreen::fileDragEnter(const juce::StringArray&, int, int) {
  impl_->dropHover = true;
  repaint();
}

void MatchScreen::fileDragExit(const juce::StringArray&) {
  impl_->dropHover = false;
  repaint();
}

void MatchScreen::filesDropped(const juce::StringArray& files, int, int) {
  impl_->dropHover = false;
  song_input::loadDroppedFiles(impl_->proc.playAlong(), files);
  impl_->refresh();
  repaint();
}

}  // namespace sawblade::plugin
