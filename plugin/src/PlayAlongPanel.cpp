#include "PlayAlongPanel.h"

#include <algorithm>
#include <cmath>

#include "MatchGlue.h"
#include "PlayAlong.h"
#include "SawbladeLookAndFeel.h"

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;

juce::String timeText(double seconds) {
  if (!(seconds > 0.0)) seconds = 0.0;
  const int tenths = static_cast<int>(std::floor(seconds * 10.0 + 1e-9));
  return juce::String::formatted("%02d:%02d.%d", tenths / 600, (tenths / 10) % 60, tenths % 10);
}

std::string baseName(const std::string& folder) {
  juce::File f(folder);
  return f.getFileName().toStdString();
}

void styleBar(juce::Slider& s, const juce::String& title, const juce::String& tip, double lo, double hi, double step, const juce::String& suffix,
              int decimals) {
  s.setSliderStyle(juce::Slider::LinearBar);
  s.setRange(lo, hi, step);
  s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 80, 20);
  s.setTextBoxIsEditable(true);
  s.setTextValueSuffix(suffix);
  s.setNumDecimalPlacesToDisplay(decimals);
  s.setMouseDragSensitivity(300);
  s.setTitle(title);
  s.setTooltip(tip);
  s.setColour(juce::Slider::trackColourId, juce::Colour(0xff6b2f12));
  s.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff141210));
  s.setColour(juce::Slider::textBoxTextColourId, L::text());
  s.setColour(juce::Slider::textBoxOutlineColourId, L::chipBorder());
  s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
}

// Small status lamp (lit while the backing plays); drawn in code, in the LED's orange.
class StatusDot : public juce::Component {
 public:
  void setOn(bool on) {
    if (on == on_) return;
    on_ = on;
    repaint();
  }
  bool isOn() const noexcept { return on_; }
  void setLit(juce::Colour glow, juce::Colour core) {
    lit_ = glow;
    core_ = core;
  }
  void paint(juce::Graphics& g) override {
    const auto r = getLocalBounds().toFloat().reduced(2.0f);
    if (on_) {
      g.setGradientFill(juce::ColourGradient(lit_.withAlpha(0.55f), r.getCentre(), lit_.withAlpha(0.0f), r.getTopLeft() - juce::Point<float>(4.0f, 4.0f), true));
      g.fillEllipse(r.expanded(4.0f));
    }
    g.setColour(on_ ? core_ : juce::Colour(0xff3a2a20));
    g.fillEllipse(r.reduced(r.getWidth() * 0.2f));
    g.setColour(juce::Colour(0xff0b0a09));
    g.drawEllipse(r.reduced(r.getWidth() * 0.2f), 1.0f);
  }

 private:
  bool on_ = false;
  juce::Colour lit_ = L::saw(), core_ = juce::Colour(0xffffb27a);
};

// One row per take: name, length, where in the song it starts, overruns, and a tag for the one used by MATCH.
struct TakeRows : juce::ListBoxModel {
  std::vector<TakeInfo> takes;
  std::string matchTake;
  int getNumRows() override { return static_cast<int>(takes.size()); }
  void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
    if (row < 0 || row >= static_cast<int>(takes.size())) return;
    const TakeInfo& t = takes[static_cast<std::size_t>(row)];
    if (selected) {
      g.setColour(juce::Colour(0xff3a1c0b));
      g.fillRect(0, 0, w, h);
    }
    const bool isMatch = t.name == matchTake;
    g.setColour(isMatch ? L::saw() : L::text());
    g.setFont(L::monoFont(12.0f));
    g.drawText(juce::String(t.name), 8, 0, 230, h, juce::Justification::centredLeft, true);
    g.setColour(L::dimText());
    g.drawText(juce::String(t.lengthSeconds(), 1) + " s", 240, 0, 60, h, juce::Justification::centredRight);
    const auto off = t.offsetMs();
    g.drawText(off ? juce::String("@ ") + juce::String(*off / 1000.0, 1) + " s" : juce::String("no song"), 306, 0, 90, h, juce::Justification::centredLeft);
    if (t.overruns > 0) {
      g.setColour(L::warning());
      g.drawText(juce::String(static_cast<int>(t.overruns)) + " overrun" + (t.overruns > 1 ? "s" : ""), 396, 0, 78, h, juce::Justification::centredLeft);
    }
    if (isMatch) {
      g.setColour(L::saw());
      g.setFont(L::labelFont(10.0f));
      g.drawText("FOR MATCH", w - 78, 0, 72, h, juce::Justification::centredRight);
    }
  }
};

}  // namespace

struct PlayAlongPanel::Impl {
  explicit Impl(PlayAlongPanel& o, SawbladeProcessor& p) : owner(o), proc(p) {}

  PlayAlongPanel& owner;
  SawbladeProcessor& proc;
  PlayAlong& pa() { return proc.playAlong(); }

  juce::Label title, song, status, position, loopRead, standaloneNote;
  juce::Label capLoop, capCount, capGuitar, capLevel, capOffset;
  juce::TextButton load, cancel, model, keepKeys, play, setA, setB, loop, countIn, mute, ghost, full, sync;
  // record / match band
  juce::Label capTakes, recTime, recInfo, emptyNote;
  juce::TextButton rec, renameTake, deleteTake, useForMatch, matchBtn, exportBtn;
  StatusDot recDot;
  TakeRows takeRows;
  juce::ListBox takeList{"Takes", &takeRows};
  std::uint64_t seenVersion = ~std::uint64_t{0};
  int refreshTick = 0;
  juce::String notice;
  std::uint32_t noticeUntil = 0;
  juce::Slider seek, bpm, level, offset;
  double barProgress = 0.0;
  juce::ProgressBar bar{barProgress};
  StatusDot led;
  std::unique_ptr<juce::FileChooser> chooser;
  bool updating = false;   // true while refresh() writes into the controls
  double shownLength = -1.0;
  double lastLoopA = -1.0, lastLoopB = -1.0;

  void configure(juce::TextButton& b, const juce::String& text, const juce::String& tip, bool toggle = false) {
    b.setButtonText(text);
    b.setTitle(text);
    b.setTooltip(tip);
    if (toggle) b.setClickingTogglesState(true);
    owner.addAndMakeVisible(b);
  }
  void caption(juce::Label& l, const juce::String& text) {
    l.setText(text, juce::dontSendNotification);
    l.setFont(L::labelFont(10.0f));
    l.setColour(juce::Label::textColourId, L::dimText());
    l.setInterceptsMouseClicks(false, false);
    owner.addAndMakeVisible(l);
  }

  void build() {
    title.setText("PLAY ALONG", juce::dontSendNotification);
    title.setFont(L::labelFont(13.0f));
    title.setColour(juce::Label::textColourId, L::saw());
    song.setFont(L::titleFont(15.0f));
    song.setMinimumHorizontalScale(0.8f);
    status.setFont(L::bodyFont(12.0f));
    status.setMinimumHorizontalScale(0.8f);
    position.setFont(L::monoFont(14.0f));
    position.setJustificationType(juce::Justification::centred);
    position.setColour(juce::Label::backgroundColourId, juce::Colour(0xff141210));
    position.setColour(juce::Label::outlineColourId, L::chipBorder());
    position.setTitle("Position");
    position.setTooltip("Playhead position / song length");
    loopRead.setFont(L::monoFont(12.0f));
    loopRead.setColour(juce::Label::textColourId, L::dimText());
    standaloneNote.setFont(L::labelFont(11.0f));
    standaloneNote.setColour(juce::Label::textColourId, L::dimText());
    standaloneNote.setText("FREE-RUN TRANSPORT", juce::dontSendNotification);
    standaloneNote.setJustificationType(juce::Justification::centredRight);
    for (juce::Label* l : {&title, &song, &status, &loopRead, &standaloneNote}) {
      l->setInterceptsMouseClicks(false, false);
      owner.addAndMakeVisible(*l);
    }
    owner.addAndMakeVisible(position);
    led.setInterceptsMouseClicks(false, false);
    owner.addAndMakeVisible(led);

    configure(load, "LOAD SONG", "Choose a song file (mp3, wav, flac, m4a: separated into stems on this machine, once, then cached) or a folder of already separated stems (drums, bass, vocals, other, guitar as .wav or .flac). You can also drop either on the plugin.");
    configure(cancel, "CANCEL", "Cancel the separation");
    cancel.setVisible(false);
    configure(model, "6-STEM", "Separation model for song files. 6-stem (htdemucs_6s, default) has a guitar stem. 4-stem (htdemucs, fallback): the 'other' stem is treated as the guitar. Click to switch.");
    model.setTitle("Separation model");
    bar.setPercentageDisplay(false);
    bar.setColour(juce::ProgressBar::foregroundColourId, juce::Colour(0xff6b2f12));
    bar.setColour(juce::ProgressBar::backgroundColourId, juce::Colour(0xff141210));
    bar.setVisible(false);
    owner.addAndMakeVisible(bar);
    configure(keepKeys, "KEEP KEYS", "Keep the 'other' stem (keys, synths) in the backing instead of treating it as the guitar. Reloads the song.", true);
    configure(play, "PLAY", "Play / pause the backing", false);
    configure(setA, "SET A", "Set loop start to the current position");
    configure(setB, "SET B", "Set loop end to the current position");
    configure(loop, "LOOP", "Loop between A and B", true);
    configure(countIn, "COUNT-IN", "One bar of clicks before playback starts", true);
    configure(mute, "MUTE", "Remove the song's guitar from the backing", true);
    configure(ghost, "GHOST", "Keep the song's guitar 12 dB down as a guide", true);
    configure(full, "FULL", "Play the song's guitar at full level", true);
    configure(sync, "SYNC TO HOST", "Plugin: enable the backing and follow the host transport. Off by default.", true);
    for (juce::TextButton* b : {&mute, &ghost, &full}) b->setRadioGroupId(71);
    for (juce::TextButton* b : {&keepKeys, &loop, &countIn, &mute, &ghost, &full, &sync})
      b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff6b2f12));

    seek.setSliderStyle(juce::Slider::LinearHorizontal);
    seek.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    seek.setRange(0.0, 1.0, 0.0);
    seek.setTitle("Seek");
    seek.setTooltip("Playhead position");
    seek.setColour(juce::Slider::thumbColourId, L::saw());
    seek.setColour(juce::Slider::trackColourId, juce::Colour(0xff6b2f12));
    seek.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff2a2622));
    owner.addAndMakeVisible(seek);
    styleBar(bpm, "Count-in BPM", "Count-in tempo", 30.0, 300.0, 1.0, " BPM", 0);
    styleBar(level, "Backing level", "Backing level. Suggested once when you load a song (matched to the rig's loudness); never changed automatically after that.",
             kBackingLevelMinDb, kBackingLevelMaxDb, 0.1, " dB", 1);
    styleBar(offset, "Backing offset",
             "Where your DI starts inside the song, in ms (the matcher's offset). Positive: the backing leads. Applied when playback is stopped.",
             -kOffsetLimitMs, kOffsetLimitMs, 1.0, " ms", 0);
    for (juce::Slider* s : {&bpm, &level, &offset}) owner.addAndMakeVisible(*s);
    caption(capLoop, "LOOP");
    caption(capCount, "COUNT-IN");
    caption(capGuitar, "GUITAR STEM");
    caption(capLevel, "BACKING LEVEL");
    caption(capOffset, "OFFSET");

    wire();
    buildBand();
  }

  // --- the record / match band ---------------------------------------------------------------------------------
  void buildBand() {
    caption(capTakes, "TAKES");
    configure(rec, "REC", "Record the clean input (before the gate) to a take. Press again to stop. Takes are saved in the takes folder with a sidecar that stores where the song was.");
    configure(renameTake, "RENAME", "Rename the selected take");
    configure(deleteTake, "DELETE", "Delete the selected take (the audio file and its sidecar)");
    configure(useForMatch, "USE FOR MATCH", "Use the selected take as the DI for MATCH");
    configure(matchBtn, "MATCH", "Find the blend that sounds like the loaded song, from the selected take (Standalone app)");
    configure(exportBtn, "EXPORT NAM", "Train a NAM model of the loaded preset for a loader pedal");
    rec.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff4a1712));
    rec.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffffb0a0));
    matchBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a1a0e));
    matchBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffffb27a));
    exportBtn.setColour(juce::TextButton::buttonColourId, L::saw());
    exportBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff140a04));
    recDot.setLit(juce::Colour(0xffff3a2a), juce::Colour(0xffff8a7a));
    recDot.setInterceptsMouseClicks(false, false);
    owner.addAndMakeVisible(recDot);
    recTime.setFont(L::monoFont(15.0f));
    recTime.setInterceptsMouseClicks(false, false);
    recInfo.setFont(L::bodyFont(11.5f));
    recInfo.setColour(juce::Label::textColourId, L::dimText());
    recInfo.setJustificationType(juce::Justification::topLeft);
    recInfo.setInterceptsMouseClicks(false, false);
    emptyNote.setFont(L::bodyFont(12.0f));
    emptyNote.setColour(juce::Label::textColourId, L::dimText());
    emptyNote.setText("No takes yet. Press REC and play.", juce::dontSendNotification);
    emptyNote.setJustificationType(juce::Justification::centred);
    emptyNote.setInterceptsMouseClicks(false, false);
    for (juce::Label* l : {&recTime, &recInfo}) owner.addAndMakeVisible(*l);
    takeList.setRowHeight(22);
    takeList.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff141210));
    takeList.setColour(juce::ListBox::outlineColourId, L::chipBorder());
    takeList.setOutlineThickness(1);
    takeList.setTitle("Takes");
    takeList.setTooltip("Recorded DI takes, newest first");
    owner.addAndMakeVisible(takeList);
    owner.addAndMakeVisible(emptyNote);
    wireBand();
  }

  void showNotice(const juce::String& text) {
    notice = text;
    noticeUntil = juce::Time::getMillisecondCounter() + 6000;
  }

  std::string selectedTakeName() const {
    const int r = takeList.getSelectedRow();
    return r >= 0 && r < static_cast<int>(takeRows.takes.size()) ? takeRows.takes[static_cast<std::size_t>(r)].name : std::string();
  }

  void wireBand() {
    rec.onClick = [this] {
      auto& r = proc.recorder();
      if (r.state() == TakeRecorder::State::Idle) r.start(pa().activeStemsDir());
      else r.stop();
      refreshBand();
    };
    useForMatch.onClick = [this] {
      const std::string n = selectedTakeName();
      if (n.empty()) return;
      chooseTakeForMatch(proc, n);  // another take than before also cancels a running refinement
      refreshBand();
    };
    renameTake.onClick = [this] {
      const std::string n = selectedTakeName();
      if (n.empty()) return;
      auto* w = new juce::AlertWindow("Rename take", "New name for " + juce::String(n), juce::MessageBoxIconType::NoIcon);
      w->addTextEditor("name", juce::String(n));
      w->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
      w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
      w->enterModalState(true, juce::ModalCallbackFunction::create([this, n, w](int result) {
                           if (result != 1) return;
                           std::string err;
                           const std::string nn = w->getTextEditorContents("name").toStdString();
                           if (!renameTakeForMatch(proc, n, nn, &err)) showNotice(juce::String(err));
                           refreshBand(true);
                         }),
                         true);
    };
    deleteTake.onClick = [this] {
      const std::string n = selectedTakeName();
      if (n.empty()) return;
      auto* w = new juce::AlertWindow("Delete take", "Delete " + juce::String(n) + " and its sidecar? This cannot be undone.", juce::MessageBoxIconType::WarningIcon);
      w->addButton("Delete", 1, juce::KeyPress(juce::KeyPress::returnKey));
      w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
      w->enterModalState(true, juce::ModalCallbackFunction::create([this, n](int result) {
                           if (result != 1) return;
                           if (!deleteTakeForMatch(proc, n)) showNotice("Could not delete " + juce::String(n));
                           refreshBand(true);
                         }),
                         true);
    };
    matchBtn.onClick = [this] { matchClicked(); };
    exportBtn.onClick = [this] {
      if (owner.onExport) owner.onExport();
    };
  }

  void matchClicked() {
    if (!proc.matchEnabled()) {
      showNotice("MATCH runs in the Standalone app: open the Standalone app.");
      return;
    }
    if (owner.onMatch) owner.onMatch();
  }

  void refreshBand(bool force = false) {
    auto& r = proc.recorder();
    const auto state = r.state();
    const bool busy = state != TakeRecorder::State::Idle;
    const double rate = r.sampleRate() > 0.0 ? r.sampleRate() : 48000.0;
    rec.setButtonText(state == TakeRecorder::State::Idle ? "REC" : "STOP");
    rec.setEnabled(state != TakeRecorder::State::Finalizing);
    recDot.setOn(state == TakeRecorder::State::Recording || state == TakeRecorder::State::Armed);
    juce::String t;
    switch (state) {
      case TakeRecorder::State::Idle: t = "READY"; break;
      case TakeRecorder::State::Armed: t = "ARMED"; break;
      case TakeRecorder::State::Recording: t = "REC " + timeText(static_cast<double>(r.recordedSamples()) / rate); break;
      case TakeRecorder::State::Finalizing: t = "SAVING"; break;
    }
    recTime.setText(t, juce::dontSendNotification);
    recTime.setColour(juce::Label::textColourId, state == TakeRecorder::State::Recording ? juce::Colour(0xffff6a5a) : L::dimText());

    // The takes on disk: rescanned when the recorder changed them, and every 10 s (other instances / the user's file manager).
    if (force || r.takesVersion() != seenVersion || (++refreshTick % 160) == 0) {
      seenVersion = r.takesVersion();
      auto fresh = r.listTakes();
      const std::string keep = selectedTakeName();
      const std::string match = proc.matchSettings().selectedTake();
      bool same = fresh.size() == takeRows.takes.size() && match == takeRows.matchTake;
      for (std::size_t i = 0; same && i < fresh.size(); ++i)
        same = fresh[i].name == takeRows.takes[i].name && fresh[i].lengthSamples == takeRows.takes[i].lengthSamples;
      if (!same) {
        takeRows.takes = std::move(fresh);
        takeRows.matchTake = match;
        takeList.updateContent();
        int row = -1;
        for (std::size_t i = 0; i < takeRows.takes.size(); ++i)
          if (takeRows.takes[i].name == keep) row = static_cast<int>(i);
        if (row < 0 && !takeRows.takes.empty()) row = 0;
        if (row >= 0) takeList.selectRow(row, true);
        takeList.repaint();
      }
    }
    emptyNote.setVisible(takeRows.takes.empty());
    const bool haveSel = !selectedTakeName().empty();
    renameTake.setEnabled(haveSel && !busy);
    deleteTake.setEnabled(haveSel && !busy);
    useForMatch.setEnabled(haveSel);

    juce::String info;
    juce::Colour col = L::dimText();
    if (const std::string err = r.lastError(); !err.empty()) {
      info = juce::String(err);
      col = L::error();
    } else if (notice.isNotEmpty() && juce::Time::getMillisecondCounter() < noticeUntil) {
      info = notice;
      col = L::warning();
    } else if (state != TakeRecorder::State::Idle) {
      info = "Recording the clean input, before the gate.";
      if (r.overruns() > 0) {
        info = juce::String(static_cast<int>(r.overruns())) + " overruns: the disk could not keep up; the gaps are filled with silence.";
        col = L::warning();
      }
    } else if (!proc.matchEnabled()) {
      info = "MATCH: open the Standalone app.";
    } else {
      info = "REC saves the clean input. Choose a take, USE FOR MATCH, then MATCH.";
    }
    recInfo.setText(info, juce::dontSendNotification);
    recInfo.setColour(juce::Label::textColourId, col);
  }

  void layoutBand() {
    constexpr int y0 = kPlayAlongHeight, m = 18;
    capTakes.setBounds(340, y0 + 8, 120, 14);
    rec.setBounds(m, y0 + 14, 96, 36);
    recDot.setBounds(m + 104, y0 + 22, 20, 20);
    recTime.setBounds(m + 128, y0 + 14, 170, 36);
    recInfo.setBounds(m, y0 + 58, 300, 46);
    takeList.setBounds(340, y0 + 26, 500, 78);
    emptyNote.setBounds(340, y0 + 26, 500, 78);
    renameTake.setBounds(856, y0 + 26, 94, 34);
    deleteTake.setBounds(856, y0 + 68, 94, 34);
    useForMatch.setBounds(958, y0 + 26, 150, 34);
    matchBtn.setBounds(1124, y0 + 26, 138, 34);
    exportBtn.setBounds(1124, y0 + 68, 138, 34);
  }

  // --- controls -> processor -------------------------------------------------------------------
  void sendLoop(bool on) {
    const auto s = pa().settings();
    pa().setLoopMs(s.loopAMs, s.loopBMs, on);
  }
  double positionMs() {
    const auto sn = pa().snapshot();
    return sn.sampleRate > 0.0 ? 1000.0 * static_cast<double>(sn.position) / sn.sampleRate : 0.0;
  }

  void wire() {
    load.onClick = [this] { owner.chooseFolder(); };
    cancel.onClick = [this] { pa().cancelSeparation(); };
    model.onClick = [this] { pa().setFourStemModel(!pa().settings().fourStemModel); };
    keepKeys.onClick = [this] { pa().setKeepOther(keepKeys.getToggleState()); };
    play.onClick = [this] {
      if (pa().snapshot().playing) pa().pause();
      else pa().play();
    };
    setA.onClick = [this] {
      const auto s = pa().settings();
      pa().setLoopMs(positionMs(), s.loopBMs, s.loopOn);
    };
    setB.onClick = [this] {
      const auto s = pa().settings();
      pa().setLoopMs(s.loopAMs, positionMs(), s.loopOn);
    };
    loop.onClick = [this] { sendLoop(loop.getToggleState()); };
    countIn.onClick = [this] { pa().setCountIn(countIn.getToggleState(), bpm.getValue()); };
    bpm.onValueChange = [this] {
      if (!updating) pa().setCountIn(countIn.getToggleState(), bpm.getValue());
    };
    mute.onClick = [this] { pa().setGuitarMode(GuitarMode::Muted); };
    ghost.onClick = [this] { pa().setGuitarMode(GuitarMode::Ghost); };
    full.onClick = [this] { pa().setGuitarMode(GuitarMode::Full); };
    level.onValueChange = [this] {
      if (!updating) pa().setLevelDb(level.getValue());
    };
    offset.onValueChange = [this] {
      if (!updating) pa().setOffsetMs(offset.getValue());
    };
    sync.onClick = [this] { pa().setHostSync(sync.getToggleState()); };
    auto doSeek = [this] {
      const auto sn = pa().snapshot();
      if (sn.sampleRate > 0.0) pa().seekSamples(static_cast<std::int64_t>(std::llround(seek.getValue() * sn.sampleRate)));
    };
    seek.onValueChange = [this, doSeek] {
      if (!updating && !seek.isMouseButtonDown()) doSeek();  // programmatic / keyboard; a drag commits on release
    };
    seek.onDragEnd = [doSeek] { doSeek(); };
  }

  // --- processor -> controls ---------------------------------------------------------------------
  void refresh() {
    const auto s = pa().settings();
    const auto st = pa().loadStatus();
    const auto sn = pa().snapshot();
    const bool standalone = pa().standalone();
    const juce::ScopedValueSetter<bool> guard(updating, true);

    // song line
    juce::String songText = "No song loaded";
    if (st.state == PlayAlong::LoadStatus::State::Ready) songText = juce::String(st.songName) + "  " + timeText(st.lengthSeconds);
    else if (!s.songFile.empty()) songText = juce::String(juce::File(s.songFile).getFileNameWithoutExtension());
    else if (!s.folder.empty()) songText = juce::String(baseName(s.folder));
    song.setText(songText, juce::dontSendNotification);
    song.setColour(juce::Label::textColourId, st.state == PlayAlong::LoadStatus::State::Ready ? L::text() : L::dimText());

    // status line
    juce::String msg;
    juce::Colour col = L::dimText();
    switch (st.state) {
      case PlayAlong::LoadStatus::State::Separating: {
        msg = "Separating " + juce::String(juce::roundToInt(st.separationFraction * 100.0)) + "%";
        if (st.separationEtaSeconds >= 0.0) msg += "  (about " + juce::String(juce::roundToInt(st.separationEtaSeconds)) + " s left)";
        col = L::warning();
        break;
      }
      case PlayAlong::LoadStatus::State::NotSeparated: msg = juce::String(st.message); col = L::warning(); break;
      case PlayAlong::LoadStatus::State::Cancelled: msg = "Separation cancelled."; break;
      case PlayAlong::LoadStatus::State::Loading: msg = "Loading stems..."; col = L::warning(); break;
      case PlayAlong::LoadStatus::State::Failed: msg = juce::String(st.message); col = L::error(); break;
      case PlayAlong::LoadStatus::State::Ready:
        if (!st.warnings.empty()) {
          msg = juce::String(st.warnings.front());
          col = L::warning();
        } else if (st.suggestedLevelDb) {
          msg = "Level set to " + juce::String(*st.suggestedLevelDb, 1) + " dB to match the rig. Adjust to taste.";
        } else if (st.otherMappedToGuitar && (standalone || s.hostSync)) {
          msg = "4-stem song: 'other' is treated as the guitar.";
        }
        break;
      case PlayAlong::LoadStatus::State::None: msg = "Drop a song file or a folder of stems here, or LOAD SONG."; break;
    }
    if (st.state == PlayAlong::LoadStatus::State::Ready && msg.isEmpty() && !standalone && !s.hostSync)
      msg = "Backing is off. Enable SYNC TO HOST to follow the host transport.";
    status.setText(msg, juce::dontSendNotification);
    status.setTooltip(msg);
    status.setColour(juce::Label::textColourId, col);
    const bool separating = st.state == PlayAlong::LoadStatus::State::Separating;
    barProgress = st.separationFraction;
    bar.setVisible(separating);
    cancel.setVisible(separating);
    keepKeys.setVisible(!separating);
    model.setButtonText(s.fourStemModel ? "4-STEM" : "6-STEM");

    // transport
    const bool plugin = !standalone;
    const bool canPlay = sn.hasSet && !plugin;
    play.setEnabled(canPlay);
    play.setButtonText(sn.playing ? "PAUSE" : "PLAY");
    seek.setEnabled(canPlay);
    sync.setVisible(plugin);
    standaloneNote.setVisible(!plugin);
    sync.setToggleState(s.hostSync, juce::dontSendNotification);
    led.setOn(sn.playing && (standalone || sn.following));

    const double rate = sn.sampleRate > 0.0 ? sn.sampleRate : 48000.0;
    const double lenS = static_cast<double>(sn.length) / rate;
    if (std::fabs(lenS - shownLength) > 1e-9) {
      shownLength = lenS;
      seek.setRange(0.0, std::max(lenS, 0.001), 0.0);
    }
    if (!seek.isMouseButtonDown()) seek.setValue(static_cast<double>(sn.position) / rate, juce::dontSendNotification);
    position.setText(timeText(static_cast<double>(sn.position) / rate) + " / " + timeText(lenS), juce::dontSendNotification);

    // loop
    const bool loopValid = s.loopAMs >= 0.0 && s.loopBMs > s.loopAMs;
    loop.setEnabled(loopValid && sn.hasSet);
    loop.setToggleState(s.loopOn, juce::dontSendNotification);
    loopRead.setText("A " + (s.loopAMs >= 0.0 ? timeText(s.loopAMs / 1000.0) : juce::String("--:--.-")) + "  B " +
                         (s.loopBMs >= 0.0 ? timeText(s.loopBMs / 1000.0) : juce::String("--:--.-")),
                     juce::dontSendNotification);

    countIn.setToggleState(s.countIn, juce::dontSendNotification);
    if (!bpm.isMouseButtonDown()) bpm.setValue(s.bpm, juce::dontSendNotification);
    mute.setToggleState(s.guitarMode == GuitarMode::Muted, juce::dontSendNotification);
    ghost.setToggleState(s.guitarMode == GuitarMode::Ghost, juce::dontSendNotification);
    full.setToggleState(s.guitarMode == GuitarMode::Full, juce::dontSendNotification);
    keepKeys.setToggleState(s.keepOther, juce::dontSendNotification);
    if (!level.isMouseButtonDown()) level.setValue(s.levelDb, juce::dontSendNotification);
    if (!offset.isMouseButtonDown()) offset.setValue(s.offsetMs, juce::dontSendNotification);
    refreshBand();
  }

  void layout() {
    constexpr int m = 18;
    const int w = kWidth;
    title.setBounds(m, 10, 110, 30);
    led.setBounds(138, 15, 20, 20);
    song.setBounds(172, 10, 280, 30);
    model.setBounds(458, 10, 96, 30);
    status.setBounds(560, 10, 440, 30);
    bar.setBounds(560, 40, 440, 8);
    keepKeys.setBounds(w - m - 120 - 8 - 104, 10, 104, 30);
    cancel.setBounds(w - m - 120 - 8 - 104, 10, 104, 30);
    load.setBounds(w - m - 120, 10, 120, 30);

    play.setBounds(m, 52, 86, 34);
    position.setBounds(m + 86 + 10, 52, 168, 34);
    seek.setBounds(m + 86 + 10 + 168 + 14, 52, w - (m + 86 + 10 + 168 + 14) - m - 250, 34);
    sync.setBounds(w - m - 232, 52, 232, 34);
    standaloneNote.setBounds(w - m - 232, 52, 232, 34);
    layoutBand();

    constexpr int cy = 126, ch = 30, capY = 104;
    capLoop.setBounds(m, capY, 120, 14);
    setA.setBounds(m, cy, 60, ch);
    setB.setBounds(m + 68, cy, 60, ch);
    loop.setBounds(m + 136, cy, 64, ch);
    loopRead.setBounds(m + 208, cy, 210, ch);
    capCount.setBounds(448, capY, 120, 14);
    countIn.setBounds(448, cy, 100, ch);
    bpm.setBounds(556, cy, 120, ch);
    capGuitar.setBounds(704, capY, 120, 14);
    mute.setBounds(704, cy, 66, ch);
    ghost.setBounds(704 + 72, cy, 66, ch);
    full.setBounds(704 + 144, cy, 66, ch);
    capLevel.setBounds(942, capY, 120, 14);
    level.setBounds(942, cy, 150, ch);
    capOffset.setBounds(1112, capY, 120, 14);
    offset.setBounds(1112, cy, 150, ch);
  }
};

PlayAlongPanel::PlayAlongPanel(SawbladeProcessor& p) : impl_(std::make_unique<Impl>(*this, p)) {
  setOpaque(true);
  setTitle("Play-along panel");
  setSize(kWidth, kHeight);
  impl_->build();
  impl_->layout();
  impl_->refresh();
}

PlayAlongPanel::~PlayAlongPanel() = default;

void PlayAlongPanel::paint(juce::Graphics& g) {
  using L = SawbladeLookAndFeel;
  g.fillAll(L::panel());
  g.setColour(L::saw().withAlpha(0.7f));
  g.fillRect(0, 0, getWidth(), 2);
  g.setColour(L::rule());
  g.fillRect(18, 94, getWidth() - 36, 1);
  g.fillRect(0, kPlayAlongHeight, getWidth(), 1);  // the record / match band below
}

void PlayAlongPanel::resized() { impl_->layout(); }

void PlayAlongPanel::refresh() { impl_->refresh(); }

void PlayAlongPanel::showMatchArea() {
  if (!impl_->proc.matchEnabled()) impl_->showNotice("MATCH runs in the Standalone app: open the Standalone app.");
  impl_->refresh();
}

void PlayAlongPanel::chooseFolder() {
  impl_->chooser = std::make_unique<juce::FileChooser>("Choose a song file or a folder of separated stems", juce::File(),
                                                       "*.mp3;*.wav;*.flac;*.m4a;*.aac;*.aif;*.aiff;*.ogg");
  impl_->chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                                  juce::FileBrowserComponent::canSelectDirectories,
                              [this](const juce::FileChooser& fc) {
                                const juce::File f = fc.getResult();
                                if (f == juce::File() || !(f.isDirectory() || f.existsAsFile())) return;
                                impl_->pa().loadSong(f.getFullPathName().toStdString(), /*userInitiated=*/true);
                              });
}

}  // namespace sawblade::plugin
