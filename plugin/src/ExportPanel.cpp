#include "ExportPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <system_error>

#include "ExportGlue.h"
#include "ExportNotes.h"
#include "SawbladeLookAndFeel.h"

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;
namespace fs = std::filesystem;

const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");
const juce::String kDash = juce::String::fromUTF8(" \xe2\x80\x94 ");
const juce::String kCheck = juce::String::fromUTF8("\xe2\x9c\x93");
const juce::String kCross = juce::String::fromUTF8("\xe2\x9c\x95");

juce::String clock(double seconds) {
  if (!(seconds > 0.0)) seconds = 0.0;
  const int s = static_cast<int>(seconds + 0.5);
  return juce::String::formatted("%02d:%02d", s / 60, s % 60);
}

std::string slug(const std::string& s) {
  std::string o;
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) o.push_back(static_cast<char>(std::tolower(u)));
    else if (!o.empty() && o.back() != '-') o.push_back('-');
  }
  while (!o.empty() && o.back() == '-') o.pop_back();
  return o.empty() ? "sawblade" : o;
}

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

// A mode card of the mockup: title, one-line description, the exactness line with its dot, the files it writes.
class OptionCard : public juce::Button {
 public:
  OptionCard() : juce::Button({}) {}
  void setContent(const juce::String& title, const juce::String& desc, const juce::String& exact, juce::Colour exactColour, const juce::String& out,
                  bool selected, bool dimmed) {
    if (title_ == title && desc_ == desc && exact_ == exact && exactColour_ == exactColour && out_ == out && selected_ == selected && dimmed_ == dimmed) return;
    title_ = title;
    desc_ = desc;
    exact_ = exact;
    exactColour_ = exactColour;
    out_ = out;
    selected_ = selected;
    dimmed_ = dimmed;
    repaint();
  }
  void paintButton(juce::Graphics& g, bool over, bool) override {
    const float a = dimmed_ ? 0.55f : 1.0f;
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(juce::Colour(0xff151311).withMultipliedAlpha(a));
    g.fillRoundedRectangle(r, 8.0f);
    g.setColour((selected_ ? L::saw() : over && isEnabled() ? L::chipBorder() : L::rule()).withMultipliedAlpha(a));
    g.drawRoundedRectangle(r, 8.0f, selected_ ? 2.0f : 1.0f);
    auto in = getLocalBounds().reduced(14, 10);
    g.setColour(L::text().withMultipliedAlpha(a));
    g.setFont(L::titleFont(16.0f));
    g.drawText(title_, in.removeFromTop(22), juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xffcfc6b4).withMultipliedAlpha(a));
    g.setFont(L::bodyFont(12.5f));
    g.drawFittedText(desc_, in.removeFromTop(48), juce::Justification::topLeft, 3, 1.0f);
    auto line = in.removeFromTop(20);
    g.setColour(exactColour_.withMultipliedAlpha(a));
    g.fillEllipse(static_cast<float>(line.getX()), static_cast<float>(line.getCentreY()) - 5.0f, 10.0f, 10.0f);
    g.setFont(L::labelFont(11.0f));
    g.drawText(exact_, line.withTrimmedLeft(18), juce::Justification::centredLeft, true);
    g.setColour(L::dimText().withMultipliedAlpha(a));
    g.setFont(L::monoFont(11.0f));
    g.drawText(out_, in.removeFromTop(18), juce::Justification::centredLeft, true);
  }

 private:
  juce::String title_, desc_, exact_, out_;
  juce::Colour exactColour_ = L::live();
  bool selected_ = false, dimmed_ = false;
};

// The third card: only information (a studio blend).
class InfoCard : public juce::Component {
 public:
  void setContent(const juce::String& title, const juce::String& desc, const juce::String& exact, juce::Colour exactColour, bool highlighted) {
    title_ = title;
    desc_ = desc;
    exact_ = exact;
    exactColour_ = exactColour;
    highlighted_ = highlighted;
    repaint();
  }
  void paint(juce::Graphics& g) override {
    const float a = highlighted_ ? 1.0f : 0.55f;
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(juce::Colour(0xff151311).withMultipliedAlpha(a));
    g.fillRoundedRectangle(r, 8.0f);
    g.setColour((highlighted_ ? L::studio() : L::rule()).withMultipliedAlpha(a));
    g.drawRoundedRectangle(r, 8.0f, 1.0f);
    auto in = getLocalBounds().reduced(14, 10);
    g.setColour(L::text().withMultipliedAlpha(a));
    g.setFont(L::titleFont(16.0f));
    g.drawText(title_, in.removeFromTop(22), juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xffcfc6b4).withMultipliedAlpha(a));
    g.setFont(L::bodyFont(12.5f));
    g.drawFittedText(desc_, in.removeFromTop(48), juce::Justification::topLeft, 3, 1.0f);
    auto line = in.removeFromTop(20);
    g.setColour(exactColour_.withMultipliedAlpha(a));
    g.fillEllipse(static_cast<float>(line.getX()), static_cast<float>(line.getCentreY()) - 5.0f, 10.0f, 10.0f);
    g.setFont(L::labelFont(11.0f));
    g.drawText(exact_, line.withTrimmedLeft(18), juce::Justification::centredLeft, true);
  }

 private:
  juce::String title_, desc_, exact_;
  juce::Colour exactColour_ = L::studio();
  bool highlighted_ = false;
};

const char* kPersonalUse = "Built from TONE3000 captures: for your own use. Sharing or selling exports needs permission from the capture creators and TONE3000.";

}  // namespace

struct ExportPanel::Impl {
  Impl(ExportPanel& o, SawbladeProcessor& p) : owner(o), proc(p) {}

  ExportPanel& owner;
  SawbladeProcessor& proc;
  ExportSettings cur;
  std::uint64_t seenSerial = 0;
  View view = View::Configure;
  std::string exportError;
  ExportPlan plan;
  ResumeOffer resumeOffer;
  // planExport / findResumableExport rebuild the preset JSON and hash it: recomputed only when something they depend on
  // changed (a preset load, the settings, the takes folder, the job), and at most once a second otherwise (a live edit
  // in the rig editor changes the preset without any of those).
  struct PlanKey {
    std::uint64_t userLoad = 0, generation = 0, settingsSerial = 0;
    int jobState = -1;
    std::string jobDir;
    bool jobResumable = false;
    long long takesStamp = 0;
    std::size_t takesCount = 0;
    bool operator==(const PlanKey&) const = default;
  };
  PlanKey planKey;
  bool planValid = false;
  juce::uint32 planAt = 0;

  juce::Label title, subtitle;
  juce::TextButton closeBtn;
  // configure
  juce::Label capMode, capSize, capDi, capComp, capOut, capRig, capChecks, capCredits;
  OptionCard noCab, withCab;
  InfoCard studio;
  juce::TextButton feather, lite, standard, diTake, diBuiltin, compDrop, compKeep, chooseFolder, exeLocate, trainBtn, resumeBtn;
  juce::Label lastRun[3], diNote, compNote, folderLabel, rigChain, rigCab, notice, message;
  juce::Label checkMark[6], checkText[6];
  juce::Label credits, ncBadge;
  // export notes (v0.4 Task E)
  juce::Label capNotes, notesSource;
  juce::TextEditor notesBox;
  juce::TextButton copyBtn;
  juce::String notesTextShown;   // what the box shows (= what COPY copies)
  bool notesFromReport = false;
  std::string notesKey;
  std::string liveRigHash;       // of the LIVE preset (not the export preset: DROP COMP hides the comp from that one), with the plan
  // right column
  juce::Label capRight, stage, detail, bestEsr, timing, status, statusSummary, numEsr, numLtas, outPath, sidecarLabel, licenceNote, wallLabel, willWrite;
  Bar bar;
  juce::TextButton cancelBtn, revealBtn, openFolderBtn, abBtn;
  std::unique_ptr<juce::FileChooser> chooser;

  // ---- helpers --------------------------------------------------------------------------------------------
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
  void button(juce::Button& b, const juce::String& t, const juce::String& tip, bool toggle = false) {
    b.setButtonText(t);
    b.setTitle(t);
    b.setTooltip(tip);
    if (toggle) b.setClickingTogglesState(false);
    if (auto* tb = dynamic_cast<juce::TextButton*>(&b)) tb->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff6b2f12));
    owner.addAndMakeVisible(b);
  }
  static void setText(juce::Label& l, const juce::String& t) {
    if (l.getText() != t) l.setText(t, juce::dontSendNotification);
  }
  static void setColour(juce::Label& l, juce::Colour c) {
    if (l.findColour(juce::Label::textColourId) != c) l.setColour(juce::Label::textColourId, c);
  }
  static void setToggle(juce::Button& b, bool on) { b.setToggleState(on, juce::dontSendNotification); }

  void build() {
    title.setFont(L::titleFont(22.0f));
    title.setColour(juce::Label::textColourId, L::saw());
    title.setText("EXPORT NAM", juce::dontSendNotification);
    subtitle.setFont(L::labelFont(11.0f));
    subtitle.setColour(juce::Label::textColourId, L::dimText());
    subtitle.setText("TRAIN ONE MODEL OF THIS RIG FOR A LOADER PEDAL", juce::dontSendNotification);
    for (juce::Label* l : {&title, &subtitle}) {
      l->setInterceptsMouseClicks(false, false);
      owner.addAndMakeVisible(*l);
    }
    button(closeBtn, juce::String::fromUTF8("\xe2\x80\xb9 RIG"), "Close this panel (a running export keeps going)");

    caption(capMode, "MODE");
    button(noCab, "NO CAB", "Train the rig without the cab; the cab IR is written as a .wav (exact when both paths share one cab)");
    button(withCab, "WITH CAB", "Train the rig including the cab (exact for any rig; for pedals without an IR slot)");
    owner.addAndMakeVisible(studio);

    caption(capSize, "SIZE");
    button(feather, "FEATHER", "Smallest model: fastest to train, not judged against the acceptance limits");
    button(lite, "LITE", "Small model: not judged against the acceptance limits");
    button(standard, "STANDARD", "Standard-size model: judged against the acceptance limits");
    for (auto& l : lastRun) text(l, 10.5f, L::dimText(), true);
    caption(capDi, "VALIDATION DI");
    button(diTake, "LAST TAKE", "Validate against the newest recorded take");
    button(diBuiltin, "BUILT-IN SIGNAL", "Validate against the built-in training signal");
    text(diNote, 11.0f, L::dimText(), true);
    caption(capComp, "BUS COMP");
    button(compDrop, "DROP COMP", "Leave the comp out of the model: exact");
    button(compKeep, "KEEP COMP", "Judge the model against the rig with the comp: inexact, the error is reported (--allow-inexact)");
    text(compNote, 11.0f, L::dimText());
    caption(capOut, "OUTPUT FOLDER");
    text(folderLabel, 11.0f, L::text(), true);
    button(chooseFolder, "CHOOSE...", "Choose the folder the export is written to");

    caption(capRig, "RIG");
    text(rigChain, 12.0f, L::text(), true);
    text(rigCab, 12.0f, L::dimText(), true);
    caption(capChecks, "WHAT GOES INTO THE MODEL");
    for (int i = 0; i < 6; ++i) {
      text(checkMark[i], 13.0f, L::live(), true);
      text(checkText[i], 13.0f);
    }
    caption(capCredits, "CREDITS (EMBEDDED IN THE .nam METADATA)");
    text(credits, 11.0f, juce::Colour(0xffcfc6b4), true);
    text(ncBadge, 11.0f, juce::Colour(0xff140a04), false, true);
    ncBadge.setText("NON-COMMERCIAL", juce::dontSendNotification);
    ncBadge.setJustificationType(juce::Justification::centred);
    ncBadge.setColour(juce::Label::backgroundColourId, L::studio());
    text(notice, 12.5f, juce::Colour(0xffffd2ad));
    notice.setText(kPersonalUse, juce::dontSendNotification);
    text(message, 12.0f, L::warning());
    caption(capNotes, "EXPORT NOTES: WHAT IS NOT IN THE MODEL");
    text(notesSource, 10.5f, L::dimText());
    notesBox.setMultiLine(true, true);
    notesBox.setReadOnly(true);
    notesBox.setCaretVisible(false);
    notesBox.setScrollbarsShown(true);
    notesBox.setPopupMenuEnabled(true);
    notesBox.setFont(L::monoFont(10.5f));
    notesBox.setColour(juce::TextEditor::backgroundColourId, L::background());
    notesBox.setColour(juce::TextEditor::textColourId, L::text());
    notesBox.setColour(juce::TextEditor::outlineColourId, L::rule());
    notesBox.setColour(juce::TextEditor::focusedOutlineColourId, L::rule());
    notesBox.setTitle("Export notes");
    owner.addAndMakeVisible(notesBox);
    button(copyBtn, "COPY", "Copy the export notes as text (what to add around the loader pedal)");
    button(trainBtn, "TRAIN EXPORT", "Train a NAM model of the loaded rig (runs sawblade-export)");
    button(resumeBtn, "RESUME", "Continue the cancelled run of this rig from its checkpoint");
    button(exeLocate, "LOCATE...", "Choose the sawblade-export executable");
    trainBtn.setColour(juce::TextButton::buttonColourId, L::saw());
    trainBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff140a04));
    resumeBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a1a0e));
    resumeBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffffb27a));

    caption(capRight, "READY");
    text(stage, 18.0f, L::text(), false, true);
    text(detail, 13.0f, L::text(), true);
    text(bestEsr, 13.0f, L::text(), true);
    text(timing, 12.0f, L::dimText(), true);
    owner.addAndMakeVisible(bar);
    button(cancelBtn, "CANCEL", "Stop the export: the checkpoint is kept, so EXPORT can RESUME it");
    text(status, 26.0f, L::live(), false, true);
    text(statusSummary, 12.0f, L::dimText());
    text(numEsr, 13.0f, L::text(), true);
    text(numLtas, 13.0f, L::text(), true);
    text(outPath, 11.0f, L::dimText(), true);
    text(sidecarLabel, 11.0f, L::dimText(), true);
    text(wallLabel, 11.0f, L::dimText(), true);
    text(licenceNote, 11.5f, juce::Colour(0xffffd2ad));
    text(willWrite, 12.0f, L::dimText());
    button(revealBtn, "REVEAL", "Show the exported model in the file manager");
    button(openFolderBtn, "OPEN FOLDER", "Open the export folder");
    button(abBtn, "A/B LISTEN", "Play the listening file: your rig, then the export");
    wire();
    applyView();
  }

  // ---- settings -------------------------------------------------------------------------------------------
  void save() {
    proc.setExportSettings(cur);
    seenSerial = proc.exportSettingsSerial();
  }
  void loadSettings() {
    cur = proc.exportSettings();
    seenSerial = proc.exportSettingsSerial();
  }

  void startExport(bool resume) {
    ExportRequest r;
    std::string err;
    bool ok = resume ? buildResumeRequest(proc, resumeOffer, r, &err) : buildExportRequest(proc, cur, plan, r, &err);
    if (ok) ok = proc.jobs().startExport(r, &err);
    exportError = ok ? std::string() : err;
    refresh();
  }

  void wire() {
    closeBtn.onClick = [this] { owner.close(); };
    noCab.onClick = [this] {
      if (!noCab.isEnabled()) return;
      cur.mode = "nocab";
      save();
      refresh();
    };
    withCab.onClick = [this] {
      cur.mode = "withcab";
      save();
      refresh();
    };
    auto size = [this](const char* s) {
      return [this, s] {
        cur.size = s;
        save();
        refresh();
      };
    };
    feather.onClick = size("feather");
    lite.onClick = size("lite");
    standard.onClick = size("standard");
    diTake.onClick = [this] {
      cur.diSource = "take";
      save();
      refresh();
    };
    diBuiltin.onClick = [this] {
      cur.diSource = "builtin";
      save();
      refresh();
    };
    compDrop.onClick = [this] {
      cur.compChoice = "drop";
      save();
      refresh();
    };
    compKeep.onClick = [this] {
      cur.compChoice = "keep";
      save();
      refresh();
    };
    chooseFolder.onClick = [this] {
      chooser = std::make_unique<juce::FileChooser>("Choose the export folder", juce::File(exportsRootFor(cur).string()));
      chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
        const juce::File f = fc.getResult();
        if (f == juce::File()) return;
        cur.outputFolder = f.getFullPathName().toStdString();
        save();
        refresh();
      });
    };
    exeLocate.onClick = [this] {
      chooser = std::make_unique<juce::FileChooser>("Choose sawblade-export", juce::File(), "*");
      chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
        const juce::File f = fc.getResult();
        if (f == juce::File()) return;
        proc.matchSettings().setExportExecutable(fs::path(f.getFullPathName().toStdString()));
        refresh();
      });
    };
    copyBtn.onClick = [this] {
      if (owner.copyToClipboard) owner.copyToClipboard(notesTextShown);
    };
    trainBtn.onClick = [this] { startExport(false); };
    resumeBtn.onClick = [this] { startExport(true); };
    cancelBtn.onClick = [this] { proc.jobs().cancel(JobKind::Export); };
    revealBtn.onClick = [this] {
      const auto s = proc.jobs().snapshot(JobKind::Export);
      if (s.outDir.empty() || !owner.reveal) return;
      const fs::path nam = s.result.namFile.empty() ? fs::path() : s.outDir / s.result.namFile;
      owner.reveal(juce::File((nam.empty() ? s.outDir : nam).string()));
    };
    openFolderBtn.onClick = [this] {
      const auto s = proc.jobs().snapshot(JobKind::Export);
      if (!s.outDir.empty() && owner.openFolder) owner.openFolder(juce::File(s.outDir.string()));
    };
    abBtn.onClick = [this] {
      const auto s = proc.jobs().snapshot(JobKind::Export);
      if (!s.result.listen.empty() && owner.openFile) owner.openFile(juce::File(s.result.listen.string()));
    };
  }

  // ---- views ----------------------------------------------------------------------------------------------
  void applyView() {
    const bool cfg = view == View::Configure, trn = view == View::Training, res = view == View::Result;
    for (juce::Component* c : std::initializer_list<juce::Component*>{&stage, &detail, &bestEsr, &timing, &bar, &cancelBtn}) c->setVisible(trn);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&status, &statusSummary, &numEsr, &numLtas, &outPath, &sidecarLabel, &wallLabel, &licenceNote,
                                                                      &revealBtn, &openFolderBtn})
      c->setVisible(res);
    willWrite.setVisible(cfg);
    if (!res) abBtn.setVisible(false);
    // stage doubles as the headline of a cancelled / failed run in the configure view
    layout();
  }

  static juce::String stageName(const std::string& s) {
    if (s == "plan") return "Planning the export";
    if (s == "signal") return "Preparing the training signal";
    if (s == "render") return "Rendering the rig's output";
    if (s == "train" || s == "training") return "Training";
    if (s == "validate" || s == "validating") return "Validating";
    if (s == "done") return "Done";
    if (s == "cancelled") return "Cancelled";
    if (s == "error") return "Error";
    return s.empty() ? juce::String("Starting...") : juce::String(s);
  }

  // ---- export notes -----------------------------------------------------------------------------------------
  static juce::String fromUtf8(const std::string& s) { return juce::String::fromUTF8(s.data(), static_cast<int>(s.size())); }

  // The notes box and its source line: the report's `exportNotes` of a finished run when it is of the known version, else
  // the plugin's own notes (mode and DROP COMP as shown before training, or as the finished run had them).
  void updateNotes(const JobSnapshot& snap) {
    const bool done = view == View::Result;
    const ExportResult& res = snap.result;
    // Rebuilt only when what the notes depend on changed: the mode, DROP COMP, the live rig (liveRigHash, refreshed with the
    // plan, at most once a second) and, for a finished run, its folder / model / report.
    const std::string key = liveRigHash + "|" + (done ? "|r|" + snap.outDir.string() + "|" + res.namFile + "|" + snap.exportMode + (snap.allowInexact ? "|i|" : "|e|") + res.exportNotesJson
                                                              : "|c|" + plan.mode + (plan.dropComp ? "|d" : "|k"));
    if (key == notesKey) return;
    notesKey = key;
    // The finished run's own preset (the resolved file it exported) names the header; the live preset only without it.
    std::string presetName = proc.status().presetName;
    if (done && !snap.source.empty()) {
      std::ifstream in(snap.source, std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const nlohmann::json pj = nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
      if (pj.is_object() && pj.contains("name") && pj["name"].is_string()) presetName = pj["name"].get<std::string>();
    }
    // Did the run drop the comp from the preset it exported? (a no-cab export without --allow-inexact with the comp on:
    // prepareExportSource sets busComp.enabled = false; same rule as ExportGlue's buildResumeRequest, judged on the live rig.)
    // The exporter's own notes are then built from that comp-less preset, so they lack the comp the model does not contain either.
    const std::string runMode = done && !snap.exportMode.empty() ? snap.exportMode : plan.mode;
    const bool runDropped = done && runMode == "nocab" && plan.rig.compOn && !snap.allowInexact;
    const bool nonCommercial = plan.rig.nonCommercial || (done && res.nonCommercial);
    const std::string licence = std::string(kPersonalUse) + (nonCommercial ? "  Non-commercial: a cc-by-nc capture is in this rig." : "");
    nlohmann::json notes;
    bool fromReport = false;
    if (done && !res.exportNotesJson.empty()) {
      notes = nlohmann::json::parse(res.exportNotesJson, nullptr, /*allow_exceptions=*/false);
      fromReport = exportNotesUsable(notes);
      if (fromReport && runDropped) {
        bool hasComp = false;
        for (const auto& st : notes["stages"])
          if (st.is_object() && st.contains("stage") && st["stage"].is_string() && st["stage"].get<std::string>() == "busComp") hasComp = true;
        if (!hasComp) fromReport = false;  // the dropped comp would vanish from the notes: show the plugin's (un-dropped rig)
      }
    }
    if (!fromReport) {
      const std::string mode = runMode;
      const bool drop = done ? runDropped : plan.dropComp;
      std::string nam, ir;
      if (done) {
        nam = res.namFile;
        std::error_code ec;
        if (mode == "nocab")
          for (fs::directory_iterator it(snap.outDir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string f = it->path().filename().string();
            if (f.size() > 7 && f.compare(f.size() - 7, 7, ".ir.wav") == 0) ir = f;
          }
      }
      notes = buildExportNotes(proc.currentPreset(), mode, drop, nam, ir);
    }
    notesFromReport = fromReport;
    notesTextShown = fromUtf8(formatNotesTxt(notes, presetName, licence));
    if (notesBox.getText() != notesTextShown) notesBox.setText(notesTextShown, juce::dontSendNotification);
    setText(notesSource, fromReport ? juce::String("from the export report (sawblade-export)")
                         : done    ? juce::String("(computed by the plugin)")
                                   : juce::String("Follows the export settings above: set these on your pedal chain around the loader."));
  }

  void refresh() {
    if (proc.exportSettingsSerial() != seenSerial) loadSettings();
    const JobSnapshot snap = proc.jobs().snapshot(JobKind::Export);
    const ToolCheck tools = proc.jobs().checkTools(JobKind::Export);
    {
      PlanKey k;
      k.userLoad = proc.userLoadSerial();
      k.generation = proc.presetGeneration();
      k.settingsSerial = proc.exportSettingsSerial();
      k.jobState = static_cast<int>(snap.state);
      k.jobDir = snap.dir.string();
      k.jobResumable = snap.resumable;
      std::error_code ec;
      const fs::path takes = proc.recorder().takesDir();
      if (const auto t = fs::last_write_time(takes, ec); !ec) k.takesStamp = static_cast<long long>(t.time_since_epoch().count());
      for (fs::directory_iterator it(takes, ec), end; !ec && it != end; it.increment(ec)) ++k.takesCount;
      const juce::uint32 now = juce::Time::getMillisecondCounter();
      if (!planValid || !(k == planKey) || now - planAt > 1000u) {
        plan = planExport(proc, cur);
        resumeOffer = findResumableExport(proc);
        planKey = k;
        liveRigHash = std::to_string(std::hash<std::string>{}(toJson(proc.currentPreset()).dump()));
        planValid = true;
        planAt = now;
      }
    }
    const RigSummary& rig = plan.rig;
    const bool active = snap.active();
    const View newView = active ? View::Training : snap.state == JobState::Succeeded ? View::Result : View::Configure;
    if (newView != view) {
      view = newView;
      applyView();
    }
    const std::string stem = slug(proc.status().presetName) + (rig.nonCommercial ? "-nc" : "");
    updateNotes(snap);

    // ---- mode cards
    const bool exactNoCab = rig.noCabExact;
    noCab.setContent("NO-CAB + IR", "Model of both paths without the cab, plus the cab IR as a .wav. Load both into your pedal.",
                     exactNoCab ? "EXACT" + juce::String(juce::CharPointer_UTF8(" \xe2\x80\x94 LIVE-COMPATIBLE BLEND")) : juce::String("NOT AVAILABLE: PER-PATH CABS"),
                     exactNoCab ? L::live() : L::studio(), juce::String(stem + "-nocab-" + cur.size + ".nam + .ir.wav"), plan.mode == "nocab", !exactNoCab);
    withCab.setContent("WITH CAB", "One model of the whole rig including the cab. For pedals without an IR slot.", "EXACT", L::live(), juce::String(stem + "-withcab-" + cur.size + ".nam"),
                       plan.mode == "withcab", false);
    studio.setContent("STUDIO BLEND", "Per-path cabs: only the with-cab export is exact. The no-cab option is disabled for this preset type.",
                      exactNoCab ? juce::String("NO-CAB NOT EXACT (NOT THIS RIG)") : juce::String("NO-CAB NOT EXACT: THIS RIG"), L::studio(), !exactNoCab);
    setToggle(noCab, plan.mode == "nocab");
    setToggle(withCab, plan.mode == "withcab");
    noCab.setEnabled(exactNoCab && !active);
    withCab.setEnabled(!active);

    // ---- size, DI, comp, folder
    const std::string sizes[3] = {"feather", "lite", "standard"};
    juce::TextButton* sizeBtn[3] = {&feather, &lite, &standard};
    for (int i = 0; i < 3; ++i) {
      setToggle(*sizeBtn[i], cur.size == sizes[i]);
      sizeBtn[i]->setEnabled(!active);
      const double sec = proc.matchSettings().exportWallSeconds(sizes[i]);
      setText(lastRun[i], sec > 0.0 ? (sec < 60.0 ? juce::String("last run: <1 min") : "last run: " + juce::String(static_cast<int>(std::lround(sec / 60.0))) + " min") : juce::String("no run yet"));
    }
    setToggle(diTake, !plan.diBuiltin);
    setToggle(diBuiltin, plan.diBuiltin);
    diTake.setEnabled(plan.take.has_value() && !active);
    diBuiltin.setEnabled(!active);
    setText(diNote, plan.take ? (plan.diBuiltin ? juce::String("newest take: " + plan.take->name + " (not used)") : juce::String(plan.take->name) + kDot + juce::String(plan.take->lengthSeconds(), 1) + " s")
                              : juce::String("no take yet: the built-in signal is used"));
    const bool compRow = rig.compOn;
    for (juce::Component* c : std::initializer_list<juce::Component*>{&capComp, &compNote}) c->setVisible(compRow);
    const bool noCabMode = plan.mode == "nocab";
    compDrop.setVisible(compRow && noCabMode);
    compKeep.setVisible(compRow && noCabMode);
    setToggle(compDrop, cur.compChoice != "keep");
    setToggle(compKeep, cur.compChoice == "keep");
    compDrop.setEnabled(!active);
    compKeep.setEnabled(!active);
    if (compRow) {
      const juce::String rel = juce::String(juce::roundToInt(rig.compReleaseMs)) + " ms";
      if (noCabMode) {
        setText(compNote, cur.compChoice == "keep" ? juce::String("KEEP: the model leaves the comp out but is judged against the rig with it: inexact, the error is reported.")
                                                   : juce::String("DROP: the comp (after the cab) is left out of the preset that is trained: exact."));
        setColour(compNote, cur.compChoice == "keep" ? L::warning() : L::dimText());
      } else if (rig.compTrainable) {
        setText(compNote, "The bus comp is trained into the model (release " + rel + juce::String(juce::CharPointer_UTF8(" \xe2\x89\xa4 150 ms).")));
        setColour(compNote, L::dimText());
      } else {
        setText(compNote, "Refused: the bus comp release (" + rel + ") is over 150 ms and cannot be trained into a NAM model. Shorten it or turn the comp off.");
        setColour(compNote, L::error());
      }
    }
    setText(folderLabel, juce::String(plan.exportsRoot.string()));
    chooseFolder.setEnabled(!active);

    // ---- rig summary, checklist, credits
    juce::String chain;
    for (const auto& l : rig.chainLines) chain += juce::String(l) + "\n";
    setText(rigChain, chain.trimEnd());
    setText(rigCab, juce::String(rig.cabLine));
    int row = 0;
    auto check = [&](const juce::String& m, juce::Colour c, const juce::String& t) {
      if (row >= 6) return;
      setText(checkMark[row], m);
      setColour(checkMark[row], c);
      setText(checkText[row], t);
      ++row;
    };
    check(kCheck, L::live(), plan.mode == "withcab" ? "Pedals, amps, path EQ, blend, cab, post EQ" : "Pedals, amps, path EQ, blend (the cab IR and post EQ go into the .wav)");
    if (!rig.compOn) check("-", L::dimText(), "Bus comp" + kDash + "off in this rig");
    else if (noCabMode) check(cur.compChoice == "keep" ? "i" : kCross, cur.compChoice == "keep" ? L::warning() : L::error(),
                              cur.compChoice == "keep" ? "Bus comp (release " + juce::String(juce::roundToInt(rig.compReleaseMs)) + " ms)" + kDash + "kept in the reference, inexact"
                                                       : "Bus comp" + kDash + "dropped (it sits after the cab): exact");
    else if (rig.compTrainable) check(kCheck, L::live(), "Bus comp (release " + juce::String(juce::roundToInt(rig.compReleaseMs)) + " ms" + kDash + "trainable)");
    else check(kCross, L::error(), "Bus comp (release " + juce::String(juce::roundToInt(rig.compReleaseMs)) + " ms)" + kDash + "too long to train: refused");
    check(kCross, L::error(), "Noise gate" + kDash + "left out, set it on your pedal");
    check(kCross, L::error(), "Time effects (delay / reverb / modulation)" + kDash + "none in this rig");
    check("i", L::warning(), "Training uses the standard NAM capture signal at 48 kHz");
    {
      const double sec = proc.matchSettings().exportWallSeconds(cur.size);
      juce::String name = cur.size == "feather" ? "Feather" : cur.size == "lite" ? "Lite" : "Standard";
      check("i", L::warning(), "Model size: " + name + kDot + (sec > 0.0 ? "last run " + juce::String(static_cast<int>(std::lround(sec / 60.0))) + " min on this machine" : juce::String("no run yet on this machine")));
    }
    for (; row < 6; ++row) {
      setText(checkMark[row], {});
      setText(checkText[row], {});
    }
    juce::String cr;
    for (const auto& l : rig.licences) cr += juce::String(l.text()) + "\n";
    setText(credits, cr.isEmpty() ? juce::String("No captures in this rig.") : cr.trimEnd());
    ncBadge.setVisible(rig.nonCommercial);

    // ---- buttons and messages
    juce::String msg;
    juce::Colour msgColour = L::warning();
    if (!tools.ok()) {
      msg = tools.message;
      msgColour = L::error();
    } else if (!plan.blocked.empty()) {
      msg = plan.blocked;
      msgColour = L::error();
    } else if (!exportError.empty()) {
      msg = exportError;
      msgColour = L::error();
    }
    setText(message, msg);
    setColour(message, msgColour);
    exeLocate.setVisible(tools.missing == ToolCheck::Missing::Executable);
    // WITH CAB with a comp whose release cannot be trained: the exporter would refuse, so do not start (the note says why).
    const bool refused = plan.mode == "withcab" && rig.compOn && !rig.compTrainable;
    trainBtn.setEnabled(tools.ok() && !active && plan.blocked.empty() && !refused);
    const bool offer = resumeOffer.available && !active;
    resumeBtn.setVisible(offer);
    resumeBtn.setEnabled(tools.ok() && offer);
    if (offer) {
      const juce::String t = "RESUME  epoch " + juce::String(resumeOffer.epoch) + " of " + juce::String(resumeOffer.epochs) + kDot + juce::String(resumeOffer.mode == "nocab" ? "NO CAB" : "WITH CAB") + kDot +
                             juce::String(resumeOffer.size).toUpperCase();
      resumeBtn.setButtonText(t);
    }

    // ---- right column
    if (view == View::Training) {
      setText(capRight, "TRAINING");
      const auto& pr = snap.progress;
      setText(stage, snap.state == JobState::Starting ? juce::String("Starting...") : stageName(pr.stage));
      const bool known = pr.fraction >= 0.0;
      bar.set(std::max(0.0, pr.fraction), !known, L::saw());
      juce::String ep = pr.epochs > 0 ? "epoch " + juce::String(pr.epoch) + " / " + juce::String(pr.epochs) : juce::String("epoch " + std::to_string(pr.epoch));
      if (known) ep += kDot + juce::String(juce::roundToInt(pr.fraction * 100.0)) + " %";
      setText(detail, ep);
      setText(bestEsr, pr.bestEsr ? "best ESR " + juce::String(*pr.bestEsr, 4) : juce::String("best ESR: after the first epoch"));
      juce::String t = "elapsed " + clock(snap.elapsedSeconds);
      t += pr.etaSeconds >= 0.0 ? kDot + "ETA " + clock(pr.etaSeconds) : kDot + "ETA: estimating";
      setText(timing, t);
      cancelBtn.setEnabled(snap.active());
      (void)tools;
    } else if (view == View::Result) {
      setText(capRight, "RESULT");
      const ExportResult& r = snap.result;
      const juce::String st = juce::String(r.status.empty() ? std::string("NOT JUDGED") : r.status);
      setText(status, st);
      setColour(status, r.status == "MET" ? L::live() : r.status == "NOT MET" ? L::error() : L::warning());
      setText(statusSummary, r.summary.empty() ? juce::String(r.haveReport ? "This size is not judged against the acceptance limits." : "No acceptance report was written.") : juce::String(r.summary));
      auto num = [](const std::optional<double>& v, int dp) { return v ? juce::String(*v, dp) : juce::String("n/a"); };
      setText(numEsr, "held-out ESR   " + num(r.heldOutEsr, 4) + "   (limit " + num(r.esrLimit, 3) + ")");
      setText(numLtas, "DI LTAS error  " + num(r.diLtasDb, 2) + " dB (limit " + num(r.ltasLimitDb, 2) + " dB)");
      setColour(numEsr, r.heldOutEsr && r.esrLimit && r.status != "NOT JUDGED" ? (*r.heldOutEsr <= *r.esrLimit ? L::live() : L::error()) : L::text());
      setColour(numLtas, r.diLtasDb && r.ltasLimitDb && r.status != "NOT JUDGED" ? (*r.diLtasDb <= *r.ltasLimitDb ? L::live() : L::error()) : L::text());
      setText(outPath, "model   " + juce::String((snap.outDir / r.namFile).string()));
      setText(sidecarLabel, snap.sidecar.empty() ? juce::String("sidecar   (not written)") : "sidecar   " + juce::String(snap.sidecar.string()));
      setText(wallLabel, "trained in " + juce::String(r.wallSeconds / 60.0, 1) + " min" + kDot + juce::String(snap.exportMode == "nocab" ? "NO CAB" : "WITH CAB") + kDot +
                             juce::String(snap.exportSize).toUpperCase());
      setText(licenceNote, juce::String(kPersonalUse) + (r.nonCommercial ? juce::String("  Non-commercial: a cc-by-nc capture is in this rig.") : juce::String()));
      revealBtn.setEnabled(!snap.outDir.empty());
      openFolderBtn.setEnabled(!snap.outDir.empty());
      const bool ab = !r.listen.empty();
      abBtn.setVisible(ab);
      abBtn.setEnabled(ab);
      ncBadge.setVisible(r.nonCommercial || rig.nonCommercial);
    } else {
      juce::String h = "Ready";
      const std::string base = stem + "-" + plan.mode + "-" + cur.size;
      juce::String body = "Will write to " + juce::String(plan.exportsRoot.string()) + ":\n  " + juce::String(base + ".nam") +
                          (plan.mode == "nocab" ? juce::String("\n  " + stem + "-nocab.ir.wav (the cab IR and post EQ)") : juce::String()) + "\n  export_report.json\n  " +
                          juce::String(base + ".sawblade.json") + " (the resolved preset)\n\n" +
                          "TRAIN EXPORT writes the model, the IR and an acceptance report into the output folder. The preset sha, mode, size, licences and the non-commercial flag go into the model's metadata.";
      juce::Colour hc = L::text();
      if (snap.state == JobState::Failed) {
        h = "Failed";
        hc = L::error();
        body = juce::String(snap.message);
      } else if (snap.state == JobState::Cancelled) {
        h = "Cancelled";
        hc = L::warning();
        body = resumeOffer.available ? "The checkpoint of epoch " + juce::String(resumeOffer.epoch) + " is kept: RESUME continues this run." : juce::String("The run was cancelled; no checkpoint of this rig is left to resume.");
      }
      setText(capRight, "STATUS");
      setText(stage, h);
      setColour(stage, hc);
      setText(willWrite, body);
      stage.setVisible(true);
      detail.setVisible(false);
    }
    if (view != View::Configure) setColour(stage, L::text());
    owner.repaint();
  }

  void layout() {
    constexpr int lx = 34, rx = 880, rw = 366;
    title.setBounds(124, 10, 400, 28);
    subtitle.setBounds(124, 38, 760, 16);
    closeBtn.setBounds(18, 14, 90, 34);
    capMode.setBounds(lx, 76, 300, 14);
    noCab.setBounds(lx, 94, 258, 128);
    withCab.setBounds(lx + 271, 94, 258, 128);
    studio.setBounds(lx + 542, 94, 258, 128);

    capSize.setBounds(lx, 236, 300, 14);
    feather.setBounds(lx, 254, 94, 30);
    lite.setBounds(lx + 100, 254, 94, 30);
    standard.setBounds(lx + 200, 254, 104, 30);
    lastRun[0].setBounds(lx, 288, 100, 14);
    lastRun[1].setBounds(lx + 100, 288, 100, 14);
    lastRun[2].setBounds(lx + 200, 288, 120, 14);
    capDi.setBounds(lx + 330, 236, 300, 14);
    diTake.setBounds(lx + 330, 254, 110, 30);
    diBuiltin.setBounds(lx + 446, 254, 146, 30);
    diNote.setBounds(lx + 330, 288, 470, 14);

    capComp.setBounds(lx, 314, 300, 14);
    compDrop.setBounds(lx, 332, 110, 28);
    compKeep.setBounds(lx + 116, 332, 110, 28);
    compNote.setBounds(lx + 240, 322, 270, 48);
    capOut.setBounds(lx + 530, 314, 270, 14);
    folderLabel.setBounds(lx + 530, 334, 170, 28);
    chooseFolder.setBounds(lx + 704, 332, 96, 28);

    capRig.setBounds(lx, 380, 300, 14);
    rigChain.setBounds(lx, 398, 440, 50);
    rigCab.setBounds(lx, 450, 440, 16);
    capChecks.setBounds(lx, 480, 400, 14);
    for (int i = 0; i < 6; ++i) {
      checkMark[i].setBounds(lx, 498 + i * 22, 20, 20);
      checkText[i].setBounds(lx + 22, 498 + i * 22, 430, 20);
    }
    capCredits.setBounds(lx + 480, 380, 330, 14);
    credits.setBounds(lx + 480, 398, 322, 200);
    ncBadge.setBounds(lx + 480, 604, 130, 20);

    notice.setBounds(lx + 8, 636, 786, 36);
    trainBtn.setBounds(lx, 684, 200, 40);
    resumeBtn.setBounds(lx + 212, 684, 300, 40);
    message.setBounds(lx + 524, 680, 190, 48);
    exeLocate.setBounds(lx + 704, 690, 96, 28);

    capRight.setBounds(rx, 76, rw, 14);
    stage.setBounds(rx, 98, rw, 26);
    bar.setBounds(rx, 134, rw, 14);
    detail.setBounds(rx, 158, rw, 20);
    bestEsr.setBounds(rx, 182, rw, 20);
    timing.setBounds(rx, 206, rw, 18);
    cancelBtn.setBounds(rx, 240, 140, 36);
    willWrite.setBounds(rx, 132, rw, 200);

    status.setBounds(rx, 98, rw, 34);
    statusSummary.setBounds(rx, 136, rw, 46);
    numEsr.setBounds(rx, 190, rw, 20);
    numLtas.setBounds(rx, 214, rw, 20);
    wallLabel.setBounds(rx, 240, rw, 16);
    revealBtn.setBounds(rx, 268, 100, 34);
    openFolderBtn.setBounds(rx + 108, 268, 130, 34);
    abBtn.setBounds(rx + 246, 268, 120, 34);
    outPath.setBounds(rx, 314, rw, 44);
    sidecarLabel.setBounds(rx, 360, rw, 44);
    licenceNote.setBounds(rx, 420, rw, 64);

    // The notes box fills the rest of the right column: below the status text, or (result view) below the licence note.
    const int ny = view == View::Result ? 494 : 342;
    capNotes.setBounds(rx, ny + 4, rw - 104, 14);
    copyBtn.setBounds(rx + rw - 96, ny, 96, 24);
    notesBox.setBounds(rx, ny + 30, rw, 676 - (ny + 30));
    notesSource.setBounds(rx, 680, rw, 32);
  }
};

ExportPanel::ExportPanel(SawbladeProcessor& p) : impl_(std::make_unique<Impl>(*this, p)) {
  setOpaque(true);
  setTitle("Export panel");
  reveal = [](const juce::File& f) { f.revealToUser(); };
  openFolder = [](const juce::File& f) { f.startAsProcess(); };
  openFile = [](const juce::File& f) { f.startAsProcess(); };
  copyToClipboard = [](const juce::String& t) { juce::SystemClipboard::copyTextToClipboard(t); };
  setSize(kWidth, kHeight);
  impl_->build();
  setVisible(false);
}

ExportPanel::~ExportPanel() = default;

ExportPanel::View ExportPanel::view() const { return impl_->view; }
ExportSettings ExportPanel::settings() const { return impl_->cur; }

void ExportPanel::open() {
  impl_->exportError.clear();
  impl_->planValid = false;
  impl_->proc.jobs().attachExisting();
  impl_->loadSettings();
  setVisible(true);
  toFront(false);
  impl_->refresh();
}

void ExportPanel::close() {
  setVisible(false);
  if (onClose) onClose();
}

void ExportPanel::refresh() {
  if (isVisible()) impl_->refresh();
}

juce::String ExportPanel::notesText() const { return impl_->notesTextShown; }
bool ExportPanel::notesFromReport() const { return impl_->notesFromReport; }

void ExportPanel::resized() { impl_->layout(); }

void ExportPanel::paint(juce::Graphics& g) {
  g.fillAll(L::background());
  g.setColour(L::panel());
  g.fillRect(0, 0, getWidth(), 60);
  g.setColour(L::rule());
  g.fillRect(0, 60, getWidth(), 1);
  g.setColour(L::panelDeep());
  g.fillRoundedRectangle(20.0f, 68.0f, 836.0f, static_cast<float>(getHeight() - 84), 6.0f);
  g.fillRoundedRectangle(868.0f, 68.0f, 392.0f, static_cast<float>(getHeight() - 84), 6.0f);
  g.setColour(L::rule());
  g.fillRect(34, 372, 802, 1);
  g.setColour(juce::Colour(0xff2a1a0e));
  g.fillRoundedRectangle(34.0f, 632.0f, 802.0f, 44.0f, 6.0f);
}

}  // namespace sawblade::plugin
