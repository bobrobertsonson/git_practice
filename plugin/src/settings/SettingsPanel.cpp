#include "SettingsPanel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

#include "../SawbladeLookAndFeel.h"
#include "../about/CaptureList.h"
#include "LoginFlow.h"
#include "ToolRunner.h"

namespace sawblade::plugin::settings {
namespace {
using L = SawbladeLookAndFeel;
namespace fs = std::filesystem;

std::atomic<bool> gFirstRunShown{false};

juce::String ju(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
std::string su(const juce::String& s) { return s.toStdString(); }

void styleButton(juce::TextButton& b, const juce::String& text, const juce::String& tip) {
  b.setButtonText(text);
  b.setTitle(text);
  b.setTooltip(tip);
}

void styleField(juce::TextEditor& e, const juce::String& title, const juce::String& tip) {
  e.setTitle(title);
  e.setTooltip(tip);
  e.setMultiLine(false);
  e.setReturnKeyStartsNewLine(false);
  e.setScrollbarsShown(false);
  e.setFont(L::monoFont(13.0f));
  e.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff141210));
  e.setColour(juce::TextEditor::textColourId, L::text());
  e.setColour(juce::TextEditor::outlineColourId, L::chipBorder());
  e.setColour(juce::TextEditor::focusedOutlineColourId, L::saw());
  e.setColour(juce::CaretComponent::caretColourId, L::saw());
  e.setIndents(6, 6);
}

void styleCombo(juce::ComboBox& c, const juce::String& title, const juce::String& tip) {
  c.setTitle(title);
  c.setTooltip(tip);
  c.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff141210));
  c.setColour(juce::ComboBox::textColourId, L::text());
  c.setColour(juce::ComboBox::outlineColourId, L::chipBorder());
  c.setColour(juce::ComboBox::arrowColourId, L::dimText());
}

void styleLabel(juce::Label& l, juce::Font f, juce::Colour c, juce::Justification j = juce::Justification::centredLeft) {
  l.setFont(f);
  l.setColour(juce::Label::textColourId, c);
  l.setJustificationType(j);
  l.setMinimumHorizontalScale(0.8f);
  l.setInterceptsMouseClicks(false, false);
}

// Status lamp: grey unknown, green ok, amber warning, red problem.
class Dot : public juce::Component {
 public:
  enum class State { Unknown, Ok, Warn, Bad };
  void setState(State s) {
    if (s == st_) return;
    st_ = s;
    repaint();
  }
  State state() const { return st_; }
  void paint(juce::Graphics& g) override {
    juce::Colour c = L::dimText().withAlpha(0.5f);
    if (st_ == State::Ok) c = L::live();
    else if (st_ == State::Warn) c = L::warning();
    else if (st_ == State::Bad) c = L::error();
    const auto r = getLocalBounds().toFloat().reduced(3.0f);
    if (st_ != State::Unknown) {
      g.setColour(c.withAlpha(0.25f));
      g.fillEllipse(r.expanded(2.5f));
    }
    g.setColour(c);
    g.fillEllipse(r);
    g.setColour(juce::Colour(0xff0b0a09));
    g.drawEllipse(r, 1.0f);
  }

 private:
  State st_ = State::Unknown;
};

class Body : public juce::Component {
 public:
  std::function<void(juce::Graphics&)> onPaint;
  void paint(juce::Graphics& g) override {
    if (onPaint) onPaint(g);
  }
};

std::string sanitizeName(const std::string& n) {
  std::string r;
  for (char c : n) r += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') ? c : '_';
  if (r.empty() || r == "." || r == "..") r = "preset";
  return r;
}

}  // namespace

bool SettingsPanel::claimFirstRunShow() { return !gFirstRunShown.exchange(true); }
void SettingsPanel::resetFirstRunShownForTests() { gFirstRunShown.store(false); }

// ---------------------------------------------------------------------------------------------
struct SettingsPanel::Impl : private juce::Timer {
  struct CheckRow {
    Dot dot;
    juce::Label text;
    juce::TextButton fix;
  };

  Impl(SettingsPanel& o, SawbladeProcessor& p, Settings& st) : owner(o), proc(p), s(st), runner(st) {}
  ~Impl() override { stopTimer(); *alive = false; }

  SettingsPanel& owner;
  SawbladeProcessor& proc;
  Settings& s;
  ToolRunner runner;
  std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

  juce::Viewport viewport;
  Body body;
  struct Heading {
    int y;
    const char* text;
  };
  std::vector<Heading> headings;
  int bodyHeight = 0, tone3000Y = 0;

  // checklist
  juce::Label summary;
  juce::TextButton showHide, done, closeBtn;
  std::array<CheckRow, 3> rows;
  juce::TextEditor fetchLog;
  // tools
  juce::Label capVenv, venvAuto, venvStatus, testResult;
  juce::TextEditor venvField;
  juce::TextButton venvBrowse, venvAutoBtn, testTools;
  Dot venvDot;
  // tone3000
  juce::Label capId, idMsg, t3kStatus, whoamiResult;
  juce::TextEditor idField;
  juce::TextButton whoamiBtn, loginBtn;
  Dot t3kDot;
  juce::Label loginCode, loginUrl, loginStatus;
  juce::TextButton copyCode, copyUrl, openBtn, cancelBtn, retryBtn;
  // captures
  juce::Label capCache, cacheCount;
  juce::TextEditor cacheField;
  juce::TextButton cacheBrowse, cacheDefault, cacheOpen;
  // separation, takes, appearance, footer
  juce::Label capSep, sepNote, capTakes, capTheme, capScale, scaleNote;
  juce::ComboBox sepCombo, themeCombo, scaleCombo;
  juce::TextEditor takesField;
  juce::TextButton takesBrowse, takesDefault, aboutBtn;
  juce::HyperlinkButton settingsFile;

  // state
  bool firstRunMode = false, expanded = false, building = true;
  std::optional<WhoamiResult> whoami;
  LoginFlow flow;
  int loginGen = 0;
  juce::uint32 deviceCodeAtMs = 0, closeAtMs = 0;
  std::shared_ptr<ToolRunner::Job> loginJob, whoamiJob, testJob, fetchJob;
  juce::String fetchError;
  bool fetchRunning = false;
  int countGen = 0;
  long long captureCount = -1;
  double captureMB = 0.0;
  std::string shownIdMsgFor;
  std::unique_ptr<juce::FileChooser> chooser;
  std::array<Dot::State, 3> rowState{};

  // --- construction -------------------------------------------------------------------------------
  void add(juce::Component& c) { body.addAndMakeVisible(c); }
  void caption(juce::Label& l, const juce::String& t) {
    l.setText(t, juce::dontSendNotification);
    styleLabel(l, L::labelFont(10.0f), L::dimText());
    add(l);
  }

  void build() {
    body.onPaint = [this](juce::Graphics& g) { paintBody(g); };
    viewport.setViewedComponent(&body, false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollBarThickness(10);
    viewport.setTitle("Settings");
    owner.addAndMakeVisible(viewport);

    styleButton(closeBtn, juce::String::fromUTF8("\xc3\x97"), "Close settings (Esc)");
    owner.addAndMakeVisible(closeBtn);
    closeBtn.onClick = [this] { owner.close(); };

    // checklist
    styleLabel(summary, L::bodyFont(13.0f), L::text());
    add(summary);
    styleButton(showHide, "SHOW", "Show or hide the setup checklist");
    showHide.onClick = [this] {
      expanded = !expanded;
      layout();
    };
    add(showHide);
    styleButton(done, "DONE", "Close the settings and do not show the first-run checklist again");
    done.onClick = [this] { owner.close(); };
    add(done);
    static const char* names[3] = {"Tools found", "Logged in", "Captures cached"};
    static const char* fixText[3] = {"LOCATE...", "LOG IN", "FETCH CAPTURES"};
    static const char* fixTip[3] = {"Choose the match venv folder (the one that contains bin/sawblade-t3k)", "Log in to TONE3000 (device code in your browser)",
                                    "Download the TONE3000 captures this preset needs that are not on disk, then reload the preset"};
    for (size_t i = 0; i < 3; ++i) {
      add(rows[i].dot);
      rows[i].dot.setTitle(names[i]);
      styleLabel(rows[i].text, L::bodyFont(13.0f), L::text());
      rows[i].text.setTitle(names[i]);
      add(rows[i].text);
      styleButton(rows[i].fix, fixText[i], fixTip[i]);
      add(rows[i].fix);
    }
    rows[0].fix.onClick = [this] { browseVenv(); };
    rows[1].fix.onClick = [this] { startLogin(); };
    rows[2].fix.onClick = [this] { fetchCaptures(); };
    fetchLog.setMultiLine(true);
    fetchLog.setReadOnly(true);
    fetchLog.setScrollbarsShown(true);
    fetchLog.setTitle("Fetch log");
    fetchLog.setTooltip("Output of sawblade-t3k resolve");
    fetchLog.setFont(L::monoFont(11.0f));
    fetchLog.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff141210));
    fetchLog.setColour(juce::TextEditor::textColourId, L::dimText());
    fetchLog.setColour(juce::TextEditor::outlineColourId, L::chipBorder());
    add(fetchLog);

    // tools
    caption(capVenv, "MATCH VENV");
    venvField.setTextToShowWhenEmpty("not found (auto-detect)", L::dimText());
    styleField(venvField, "Match venv path", "Folder of the Python environment that has bin/sawblade-t3k and bin/sawblade-match. Empty = auto-detect.");
    venvField.onReturnKey = [this] { commitPath(venvField, s.matchVenvDir(), [this](std::optional<fs::path> p) { s.setMatchVenvDir(p); }, effectiveText(s.effectiveMatchVenvDir())); };
    venvField.onFocusLost = venvField.onReturnKey;
    venvField.onEscapeKey = [this] { venvField.giveAwayKeyboardFocus(); refresh(); };
    add(venvField);
    styleLabel(venvAuto, L::labelFont(11.0f), L::dimText());
    venvAuto.setText("(auto)", juce::dontSendNotification);
    add(venvAuto);
    styleButton(venvBrowse, "Browse...", "Choose the match venv folder");
    venvBrowse.onClick = [this] { browseVenv(); };
    add(venvBrowse);
    styleButton(venvAutoBtn, "Auto", "Forget the stored path and auto-detect the match venv");
    venvAutoBtn.onClick = [this] {
      s.setMatchVenvDir(std::nullopt);
      refresh();
    };
    add(venvAutoBtn);
    add(venvDot);
    styleLabel(venvStatus, L::bodyFont(12.0f), L::text());
    add(venvStatus);
    styleButton(testTools, "Test", "Run sawblade-t3k --help to check the tools run");
    testTools.onClick = [this] { runToolTest(); };
    add(testTools);
    styleLabel(testResult, L::monoFont(12.0f), L::dimText());
    add(testResult);

    // tone3000
    caption(capId, "CLIENT ID (PUBLISHABLE KEY, t3k_pub_...)");
    styleField(idField, "TONE3000 client id", "Your TONE3000 publishable key (t3k_pub_...). The secret key (t3k_cs_...) is refused and never stored.");
    idField.onReturnKey = [this] { commitId(); };
    idField.onFocusLost = [this] { commitId(); };
    idField.onEscapeKey = [this] { idField.setText(ju(s.tone3000ClientId()), false); idField.giveAwayKeyboardFocus(); };
    add(idField);
    styleLabel(idMsg, L::bodyFont(12.0f), L::error());
    add(idMsg);
    add(t3kDot);
    styleLabel(t3kStatus, L::bodyFont(12.0f), L::text());
    add(t3kStatus);
    styleButton(whoamiBtn, "Test", "Run sawblade-t3k whoami: shows who you are logged in as");
    whoamiBtn.onClick = [this] { runWhoami(); };
    add(whoamiBtn);
    styleLabel(whoamiResult, L::bodyFont(12.0f), L::dimText());
    add(whoamiResult);
    styleButton(loginBtn, "Log in to TONE3000", "Start the TONE3000 device login: a code to approve in your browser");
    loginBtn.onClick = [this] { startLogin(); };
    add(loginBtn);
    styleLabel(loginCode, L::monoFont(40.0f), L::saw(), juce::Justification::centredLeft);
    loginCode.setInterceptsMouseClicks(true, false);
    loginCode.setTitle("Login code");
    add(loginCode);
    styleLabel(loginUrl, L::monoFont(13.0f), L::text());
    add(loginUrl);
    styleLabel(loginStatus, L::bodyFont(12.0f), L::dimText());
    add(loginStatus);
    styleButton(copyCode, "COPY CODE", "Copy the login code to the clipboard");
    copyCode.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(ju(flow.code)); };
    styleButton(copyUrl, "COPY URL", "Copy the verification address to the clipboard");
    copyUrl.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(ju(flow.url)); };
    styleButton(openBtn, "OPEN", "Open the verification page in your browser");
    openBtn.onClick = [this] {
      if (!flow.openUrl.empty()) juce::URL(ju(flow.openUrl)).launchInDefaultBrowser();
    };
    styleButton(cancelBtn, "CANCEL", "Cancel the login");
    cancelBtn.onClick = [this] { cancelLogin(); };
    styleButton(retryBtn, "RETRY", "Start the login again");
    retryBtn.onClick = [this] { startLogin(); };
    for (juce::TextButton* b : {&copyCode, &copyUrl, &openBtn, &cancelBtn, &retryBtn}) add(*b);

    // captures
    caption(capCache, "CAPTURE CACHE FOLDER");
    styleField(cacheField, "Capture cache folder", "Where downloaded TONE3000 captures are kept. Default: ~/.cache/sawblade/captures");
    cacheField.onReturnKey = [this] { commitPath(cacheField, s.captureCacheDir(), [this](std::optional<fs::path> p) { s.setCaptureCacheDir(p); this->startCount(); }, effectiveText(s.effectiveCaptureCacheDir())); };
    cacheField.onFocusLost = cacheField.onReturnKey;
    add(cacheField);
    styleButton(cacheBrowse, "Browse...", "Choose the capture cache folder");
    cacheBrowse.onClick = [this] { browseDir("Choose the capture cache folder", [this](fs::path p) { s.setCaptureCacheDir(p); refresh(); startCount(); }); };
    add(cacheBrowse);
    styleButton(cacheDefault, "Default", "Use the default capture cache folder");
    cacheDefault.onClick = [this] { s.setCaptureCacheDir(std::nullopt); refresh(); startCount(); };
    add(cacheDefault);
    styleLabel(cacheCount, L::bodyFont(12.0f), L::dimText());
    add(cacheCount);
    styleButton(cacheOpen, "Open folder", "Show the capture cache folder in the file manager");
    cacheOpen.onClick = [this] {
      const juce::File f(ju(s.effectiveCaptureCacheDir().string()));
      f.createDirectory();
      f.revealToUser();
    };
    add(cacheOpen);

    // separation
    caption(capSep, "SEPARATION MODEL");
    styleCombo(sepCombo, "Separation model", "Model used to separate a song into stems");
    sepCombo.addItem("htdemucs_6s", 1);
    sepCombo.addItem("htdemucs", 2);
    sepCombo.addItem("htdemucs_ft", 3);
    sepCombo.onChange = [this] {
      if (!building) s.setSeparationModel(su(sepCombo.getText()));
    };
    add(sepCombo);
    styleLabel(sepNote, L::bodyFont(12.0f), L::dimText());
    sepNote.setText("htdemucs_6s has a guitar stem; htdemucs (4 stems) maps other->guitar", juce::dontSendNotification);
    add(sepNote);

    // takes
    caption(capTakes, "TAKES FOLDER (RECORDINGS)");
    styleField(takesField, "Takes folder", "Where recorded takes are saved");
    takesField.onReturnKey = [this] { commitPath(takesField, s.takesDir(), [this](std::optional<fs::path> p) { s.setTakesDir(p); }, effectiveText(s.effectiveTakesDir())); };
    takesField.onFocusLost = takesField.onReturnKey;
    add(takesField);
    styleButton(takesBrowse, "Browse...", "Choose the takes folder");
    takesBrowse.onClick = [this] { browseDir("Choose the takes folder", [this](fs::path p) { s.setTakesDir(p); refresh(); }); };
    add(takesBrowse);
    styleButton(takesDefault, "Default", "Use the default takes folder");
    takesDefault.onClick = [this] { s.setTakesDir(std::nullopt); refresh(); };
    add(takesDefault);

    // appearance
    caption(capTheme, "THEME");
    styleCombo(themeCombo, "Theme", "more themes later");
    themeCombo.addItem("Dark", 1);
    themeCombo.setSelectedId(1, juce::dontSendNotification);
    themeCombo.setEnabled(false);
    add(themeCombo);
    caption(capScale, "UI SCALE");
    styleCombo(scaleCombo, "UI scale", "Size of the plugin window. Applies when the window is next opened.");
    static const int pct[5] = {75, 100, 125, 150, 200};
    for (int i = 0; i < 5; ++i) scaleCombo.addItem(juce::String(pct[i]) + " %", i + 1);
    scaleCombo.onChange = [this] {
      if (!building) s.setUiScale(pct[std::clamp(scaleCombo.getSelectedId() - 1, 0, 4)] / 100.0);
    };
    add(scaleCombo);
    styleLabel(scaleNote, L::bodyFont(12.0f), L::dimText());
    scaleNote.setText("applies when the window is next opened", juce::dontSendNotification);
    add(scaleNote);

    // footer
    settingsFile.setButtonText("Settings file: " + ju(s.file().string()));
    settingsFile.setTitle("Reveal settings file");
    settingsFile.setTooltip("Show the settings file in the file manager");
    settingsFile.setFont(L::monoFont(11.0f), false);
    settingsFile.setColour(juce::HyperlinkButton::textColourId, L::dimText());
    settingsFile.setJustificationType(juce::Justification::centredLeft);
    settingsFile.onClick = [this] { juce::File(ju(s.file().string())).revealToUser(); };
    add(settingsFile);
    styleButton(aboutBtn, "About Sawblade...", "Version, licences and the captures used by this preset");
    aboutBtn.onClick = [this] {
      if (owner.onAbout) owner.onAbout();
    };
    add(aboutBtn);

    building = false;
    updateLoginUi();
  }

  // --- helpers --------------------------------------------------------------------------------------
  static std::string effectiveText(const std::optional<fs::path>& p) { return p ? p->string() : std::string(); }
  static std::string effectiveText(const fs::path& p) { return p.string(); }

  template <class Setter>
  void commitPath(juce::TextEditor& f, std::optional<fs::path> stored, Setter set, const std::string& shownAuto) {
    const std::string text = su(f.getText().trim());
    if (text.empty()) {
      if (stored) set(std::nullopt);
    } else if (stored ? stored->string() != text : text != shownAuto) {
      set(fs::path(text));
    }
    refresh();
  }

  void browseDir(const juce::String& title, std::function<void(fs::path)> done) {
    chooser = std::make_unique<juce::FileChooser>(title, juce::File(), "");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [done = std::move(done)](const juce::FileChooser& fc) {
      const juce::File f = fc.getResult();
      if (f != juce::File() && f.isDirectory()) done(fs::path(f.getFullPathName().toStdString()));
    });
  }
  void browseVenv() {
    browseDir("Choose the match venv folder", [this](fs::path p) {
      s.setMatchVenvDir(p);
      refresh();
    });
  }

  void commitId() {
    const std::string text = su(idField.getText());
    if (text == s.tone3000ClientId() && idMsg.getText().isEmpty()) return;
    if (text == s.tone3000ClientId()) return;  // keep a message that is showing
    const Result r = s.setTone3000ClientId(text);
    if (!r.ok) {
      idMsg.setColour(juce::Label::textColourId, L::error());
      idMsg.setText(ju(r.error), juce::dontSendNotification);
      idField.setText(ju(s.tone3000ClientId()), false);  // never echo a refused value
    } else if (!r.warning.empty()) {
      idMsg.setColour(juce::Label::textColourId, L::warning());
      idMsg.setText(ju(r.warning), juce::dontSendNotification);
      idField.setText(ju(s.tone3000ClientId()), false);
    } else {
      idMsg.setText({}, juce::dontSendNotification);
      idField.setText(ju(s.tone3000ClientId()), false);
    }
    refresh();
  }

  // --- jobs -----------------------------------------------------------------------------------------
  void runToolTest() {
    testTools.setEnabled(false);
    testResult.setColour(juce::Label::textColourId, L::dimText());
    testResult.setText("running...", juce::dontSendNotification);
    const auto t0 = std::chrono::steady_clock::now();
    ToolRequest r;
    r.tool = "sawblade-t3k";
    r.args = {"--help"};
    testJob = runner.run(r, nullptr, [this, t0](const ToolResult& res) {
      testTools.setEnabled(true);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      if (res.outcome == ToolResult::Outcome::Ok) {
        testResult.setColour(juce::Label::textColourId, L::live());
        testResult.setText("ok (exit 0, " + juce::String(static_cast<int>(ms)) + " ms)", juce::dontSendNotification);
      } else {
        std::string why = !res.error.empty() ? res.error : (res.lines.empty() ? "exit " + std::to_string(res.exitCode) : res.lines.back());
        testResult.setColour(juce::Label::textColourId, L::error());
        testResult.setText(ju(why), juce::dontSendNotification);
      }
    });
  }

  void runWhoami() {
    whoamiBtn.setEnabled(false);
    whoamiResult.setColour(juce::Label::textColourId, L::dimText());
    whoamiResult.setText("checking...", juce::dontSendNotification);
    ToolRequest r;
    r.tool = "sawblade-t3k";
    r.args = {"whoami", "--json"};
    whoamiJob = runner.run(r, nullptr, [this](const ToolResult& res) {
      whoamiBtn.setEnabled(true);
      whoami = parseWhoami(res);
      refresh();
    });
  }

  void startLogin() {
    if (loginJob) loginJob->cancel();
    const int gen = ++loginGen;
    flow.begin();
    ToolRequest r;
    r.tool = "sawblade-t3k";
    r.args = {"login", "--json"};
    updateLoginUi();
    loginJob = runner.run(
        r,
        [this, gen](const std::string& line) {
          if (gen != loginGen) return;
          if (flow.feedLine(line)) {
            if (flow.state == LoginFlow::State::WaitingForApproval) deviceCodeAtMs = juce::Time::getMillisecondCounter();
            if (flow.state == LoginFlow::State::LoggedIn) {
              closeAtMs = juce::Time::getMillisecondCounter() + 3000;
              whoami = WhoamiResult{true, flow.loggedInText()};
            }
            updateLoginUi();
          }
        },
        [this, gen](const ToolResult& res) {
          if (gen != loginGen) return;
          flow.finish(res);
          updateLoginUi();
          refresh();
        });
  }

  void cancelLogin() {
    ++loginGen;
    if (loginJob) loginJob->cancel();
    flow = LoginFlow{};
    updateLoginUi();
  }

  void fetchCaptures() {
    if (fetchRunning) return;
    fetchError = {};
    const Preset preset = proc.currentPreset();
    const fs::path dir = s.effectiveCaptureCacheDir() / "_resolve";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string name = sanitizeName(preset.name);
    const fs::path in = dir / (name + ".json"), out = dir / (name + ".resolved.json");
    {
      std::ofstream f(in, std::ios::binary | std::ios::trunc);
      if (!f) {
        fetchError = "Cannot write " + ju(in.string());
        refresh();
        return;
      }
      f << sawblade::toJson(preset).dump(2) << "\n";
    }
    fetchRunning = true;
    fetchLog.setText({}, false);
    ToolRequest r;
    r.tool = "sawblade-t3k";
    r.args = {"resolve", in.string(), "-o", out.string()};
    fetchJob = runner.run(
        r, [this](const std::string& line) { fetchLog.moveCaretToEnd(); fetchLog.insertTextAtCaret(ju(line) + "\n"); },
        [this, out](const ToolResult& res) {
          fetchRunning = false;
          if (res.outcome == ToolResult::Outcome::Ok) {
            std::string err;
            if (!proc.loadPresetFile(out, &err)) fetchError = "Could not load the resolved preset: " + ju(err);
          } else {
            fetchError = !res.error.empty() ? ju(res.error) : (res.lines.empty() ? "resolve failed (exit " + juce::String(res.exitCode) + ")" : ju(res.lines.back()));
          }
          layout();
          refresh();
        });
    layout();
  }

  void startCount() {
    const int gen = ++countGen;
    const fs::path dir = s.effectiveCaptureCacheDir();
    auto flag = alive;
    std::thread([this, gen, dir, flag] {
      long long n = 0;
      double bytes = 0;
      std::error_code ec;
      if (fs::exists(dir, ec)) {
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
          std::error_code e2;
          if (!it->is_regular_file(e2)) continue;
          const std::string ext = it->path().extension().string();
          if (ext == ".nam" || ext == ".wav") {
            ++n;
            const auto sz = it->file_size(e2);
            if (!e2) bytes += static_cast<double>(sz);
          }
        }
      }
      juce::MessageManager::callAsync([this, gen, n, bytes, flag] {
        if (!flag->load() || gen != countGen) return;
        captureCount = n;
        captureMB = bytes / 1.0e6;
        updateCountLabel();
      });
    }).detach();
  }

  void updateCountLabel() {
    if (captureCount < 0) cacheCount.setText("counting...", juce::dontSendNotification);
    else cacheCount.setText(juce::String(captureCount) + " captures on disk (" + juce::String(captureMB, 1) + " MB)", juce::dontSendNotification);
  }

  // --- login box ------------------------------------------------------------------------------------
  bool loginBoxShown() const { return flow.state != LoginFlow::State::Idle; }

  void updateLoginUi() {
    using S = LoginFlow::State;
    const bool shown = loginBoxShown();
    for (juce::Component* c : std::initializer_list<juce::Component*>{&loginCode, &loginUrl, &loginStatus, &copyCode, &copyUrl, &openBtn, &cancelBtn, &retryBtn})
      c->setVisible(false);
    loginBtn.setEnabled(flow.state != S::Starting && flow.state != S::WaitingForApproval);
    if (shown) {
      loginStatus.setVisible(true);
      switch (flow.state) {
        case S::Starting:
          loginStatus.setColour(juce::Label::textColourId, L::dimText());
          loginStatus.setText("Starting login...", juce::dontSendNotification);
          cancelBtn.setVisible(true);
          break;
        case S::WaitingForApproval:
          loginCode.setText(ju(flow.code), juce::dontSendNotification);
          loginUrl.setText(ju(flow.url), juce::dontSendNotification);
          for (juce::Component* c : std::initializer_list<juce::Component*>{&loginCode, &loginUrl, &copyCode, &copyUrl, &openBtn, &cancelBtn}) c->setVisible(true);
          updateCountdown();
          needScroll = true;
          break;
        case S::LoggedIn:
          loginStatus.setColour(juce::Label::textColourId, L::live());
          loginStatus.setText(ju(flow.loggedInText()), juce::dontSendNotification);
          break;
        case S::Failed:
          loginStatus.setColour(juce::Label::textColourId, L::error());
          loginStatus.setText(ju(flow.message), juce::dontSendNotification);
          retryBtn.setVisible(true);
          break;
        case S::Idle: break;
      }
    }
    if (shown && flow.state != S::Failed) startTimerHz(4);
    else stopTimer();
    layout();
    if (needScroll) {
      needScroll = false;
      scrollTo(loginBoxY - 140);  // keep the heading and the whole box in view
    }
  }
  bool needScroll = false;

  void updateCountdown() {
    const int elapsed = static_cast<int>((juce::Time::getMillisecondCounter() - deviceCodeAtMs) / 1000);
    const int left = std::max(0, flow.expiresIn - elapsed);
    loginStatus.setColour(juce::Label::textColourId, L::dimText());
    loginStatus.setText("Waiting for approval... (expires in " + juce::String(left) + " s)", juce::dontSendNotification);
  }

  void timerCallback() override {
    if (flow.state == LoginFlow::State::WaitingForApproval) updateCountdown();
    if (flow.state == LoginFlow::State::LoggedIn && juce::Time::getMillisecondCounter() >= closeAtMs) {
      flow = LoginFlow{};
      updateLoginUi();
    }
  }

  // --- evaluation -----------------------------------------------------------------------------------
  void refresh() {
    if (building) return;
    const auto venv = s.effectiveMatchVenvDir();
    const auto stored = s.matchVenvDir();
    auto ex = [this](const fs::path& p) { return s.env().exists && s.env().exists(p); };
    const bool hasT3k = venv && ex(*venv / "bin" / "sawblade-t3k"), hasMatch = venv && ex(*venv / "bin" / "sawblade-match");

    // tools section
    if (!venvField.hasKeyboardFocus(true)) {
      const juce::String t = ju(effectiveText(venv));
      if (venvField.getText() != t) venvField.setText(t, false);
      venvField.setColour(juce::TextEditor::textColourId, stored ? L::text() : L::dimText());
      venvField.applyColourToAllText(stored ? L::text() : L::dimText());
    }
    venvAuto.setVisible(!stored && venv.has_value());
    venvStatus.setColour(juce::Label::textColourId, L::text());
    if (hasT3k) {
      venvDot.setState(hasMatch ? Dot::State::Ok : Dot::State::Warn);
      venvStatus.setText(juce::String("found: sawblade-t3k") + (hasMatch ? ", sawblade-match" : " (sawblade-match missing)"), juce::dontSendNotification);
    } else {
      venvDot.setState(Dot::State::Bad);
      venvStatus.setText(venv ? "not found: no bin/sawblade-t3k in " + ju(venv->string()) : juce::String("not found: no match venv detected. Browse to it."), juce::dontSendNotification);
    }

    // tone3000 section
    if (!idField.hasKeyboardFocus(true) && idField.getText() != ju(s.tone3000ClientId())) idField.setText(ju(s.tone3000ClientId()), false);
    const bool token = ex(s.tokenFile());
    t3kDot.setState(token ? Dot::State::Ok : Dot::State::Bad);
    t3kStatus.setText(token ? "Token file present (" + ju(s.tokenFile().string()) + ")" : juce::String("Not logged in"), juce::dontSendNotification);
    if (whoami && !whoamiBtnBusy()) {
      whoamiResult.setColour(juce::Label::textColourId, whoami->ok ? L::live() : L::error());
      whoamiResult.setText(ju(whoami->text), juce::dontSendNotification);
    }

    // captures section
    const auto cache = s.effectiveCaptureCacheDir();
    const auto storedCache = s.captureCacheDir();
    if (!cacheField.hasKeyboardFocus(true)) {
      const juce::String t = ju(cache.string());
      if (cacheField.getText() != t) cacheField.setText(t, false);
      cacheField.applyColourToAllText(storedCache ? L::text() : L::dimText());
    }
    if (captureCount < 0 && countGen == 0) startCount();

    // separation / takes / appearance
    building = true;
    const std::string sep = s.separationModel();
    sepCombo.setSelectedId(sep == "htdemucs" ? 2 : sep == "htdemucs_ft" ? 3 : 1, juce::dontSendNotification);
    const int pct = static_cast<int>(std::lround(s.uiScale() * 100.0));
    int idx = 1;
    {
      int best = 1000;
      static const int vals[5] = {75, 100, 125, 150, 200};
      for (int i = 0; i < 5; ++i)
        if (std::abs(vals[i] - pct) < best) {
          best = std::abs(vals[i] - pct);
          idx = i;
        }
    }
    scaleCombo.setSelectedId(idx + 1, juce::dontSendNotification);
    building = false;
    if (!takesField.hasKeyboardFocus(true)) {
      const juce::String t = ju(s.effectiveTakesDir().string());
      if (takesField.getText() != t) takesField.setText(t, false);
      takesField.applyColourToAllText(s.takesDir() ? L::text() : L::dimText());
    }
    settingsFile.setButtonText("Settings file: " + ju(s.file().string()));

    // checklist
    const auto captures = about::listCaptures(proc.currentPreset());
    int total = 0, missing = 0;
    for (const auto& c : captures)
      if (c.provider == "tone3000") {
        ++total;
        if (!c.onDisk) ++missing;
      }
    std::error_code ec;
    const bool cacheExists = fs::exists(cache, ec);

    struct R {
      Dot::State st;
      juce::String text;
    } res[3];
    if (hasT3k && hasMatch) res[0] = {Dot::State::Ok, "sawblade-t3k, sawblade-match in " + ju(venv->string())};
    else if (hasT3k) res[0] = {Dot::State::Warn, "sawblade-match missing in " + ju(venv->string())};
    else res[0] = {Dot::State::Bad, "match venv not found"};
    if (!token) res[1] = {Dot::State::Bad, "not logged in"};
    else if (whoami) res[1] = {whoami->ok ? Dot::State::Ok : Dot::State::Bad, ju(whoami->text)};
    else res[1] = {Dot::State::Ok, "token file present"};
    if (!fetchError.isEmpty()) res[2] = {Dot::State::Bad, fetchError};
    else if (fetchRunning) res[2] = {Dot::State::Warn, "fetching captures..."};
    else if (total == 0) res[2] = {Dot::State::Ok, "no TONE3000 captures in this preset"};
    else if (missing > 0) res[2] = {Dot::State::Bad, juce::String(missing) + " of " + juce::String(total) + " captures missing"};
    else if (!cacheExists) res[2] = {Dot::State::Warn, "no cache dir yet"};
    else res[2] = {Dot::State::Ok, "all " + juce::String(total) + " captures of this preset on disk"};
    int problems = 0;
    for (size_t i = 0; i < 3; ++i) {
      rows[i].dot.setState(res[i].st);
      rows[i].text.setText(res[i].text, juce::dontSendNotification);
      rows[i].text.setColour(juce::Label::textColourId, res[i].st == Dot::State::Bad ? L::error() : res[i].st == Dot::State::Warn ? L::warning() : L::text());
      if (res[i].st != Dot::State::Ok) ++problems;
      rowState[i] = res[i].st;
    }
    rows[2].fix.setEnabled(!fetchRunning && missing > 0);
    rows[1].fix.setEnabled(flow.state != LoginFlow::State::Starting && flow.state != LoginFlow::State::WaitingForApproval);
    summary.setText(problems == 0 ? juce::String("Setup: 3/3 ok") : "Setup: " + juce::String(problems) + (problems == 1 ? " problem" : " problems"), juce::dontSendNotification);
    summary.setColour(juce::Label::textColourId, problems == 0 ? L::live() : L::warning());
    updateCountLabel();
  }
  bool whoamiBtnBusy() const { return !whoamiBtn.isEnabled(); }

  // --- layout ---------------------------------------------------------------------------------------
  void scrollTo(int y) { viewport.setViewPosition(0, std::max(0, y - 8)); }

  void layout() {
    if (building) return;
    const int w = std::max(100, owner.getWidth() - 10 - 0);
    constexpr int m = 20;
    headings.clear();
    int y = 12;
    auto heading = [&](const char* t) {
      headings.push_back({y, t});
      y += 24;
    };
    auto place = [&](juce::Component& c, int x, int yy, int ww, int hh) { c.setBounds(x, yy, ww, hh); };

    // --- checklist
    const int headY = y;
    heading("SETUP");
    const bool showRows = expanded;
    showHide.setButtonText(expanded ? "HIDE" : "SHOW");
    showHide.setTitle(showHide.getButtonText());
    summary.setVisible(!showRows);
    for (auto& r : rows) {
      r.dot.setVisible(showRows);
      r.text.setVisible(showRows);
      r.fix.setVisible(showRows);
    }
    done.setVisible(showRows && firstRunMode);
    const bool logShown = showRows && (fetchRunning || fetchLog.getText().isNotEmpty());
    fetchLog.setVisible(logShown);
    place(showHide, w - m - 80 - 44, headY - 2, 80, 24);  // left of the close button
    if (!showRows) {
      place(summary, m, y, 400, 28);
      y += 36;
    } else {
      y += 4;
      static const int fixW[3] = {120, 100, 150};
      for (size_t i = 0; i < 3; ++i) {
        place(rows[i].dot, m, y + 6, 22, 22);
        place(rows[i].text, m + 30, y, w - 2 * m - 30 - fixW[i] - 10, 34);
        place(rows[i].fix, w - m - fixW[i], y + 2, fixW[i], 30);
        y += 38;
      }
      if (logShown) {
        place(fetchLog, m, y, w - 2 * m, 72);
        y += 80;
      }
      if (firstRunMode) {
        place(done, w - m - 100, y + 4, 100, 32);
        y += 44;
      }
    }
    y += 8;

    // --- tools
    heading("TOOLS");
    place(capVenv, m, y, 300, 14);
    y += 16;
    place(venvField, m, y, w - 2 * m - 90 - 8 - 70 - 8, 30);
    place(venvAuto, m + w - 2 * m - 90 - 8 - 70 - 8 - 56, y + 6, 50, 18);
    place(venvBrowse, w - m - 90 - 8 - 70, y, 90, 30);
    place(venvAutoBtn, w - m - 70, y, 70, 30);
    y += 38;
    place(venvDot, m, y + 4, 22, 22);
    place(venvStatus, m + 30, y, w - 2 * m - 30, 30);
    y += 36;
    place(testTools, m, y, 80, 30);
    place(testResult, m + 92, y, w - 2 * m - 92, 30);
    y += 44;

    // --- tone3000
    tone3000Y = y;
    heading("TONE3000");
    place(capId, m, y, 400, 14);
    y += 16;
    place(idField, m, y, w - 2 * m, 30);
    y += 32;
    place(idMsg, m, y, w - 2 * m, 18);
    y += 24;
    place(t3kDot, m, y + 4, 22, 22);
    place(t3kStatus, m + 30, y, w - 2 * m - 30, 30);
    y += 36;
    place(whoamiBtn, m, y, 80, 30);
    place(whoamiResult, m + 92, y, 300, 30);
    place(loginBtn, w - m - 200, y, 200, 30);
    y += 40;
    if (loginBoxShown()) {
      const int bh = flow.state == LoginFlow::State::WaitingForApproval ? 168 : 60;
      loginBoxY = y;
      loginBoxH = bh;
      if (flow.state == LoginFlow::State::WaitingForApproval) {
        place(loginCode, m + 14, y + 8, w - 2 * m - 28, 54);
        place(loginUrl, m + 14, y + 64, w - 2 * m - 28, 20);
        place(copyCode, m + 14, y + 92, 110, 30);
        place(copyUrl, m + 14 + 118, y + 92, 110, 30);
        place(openBtn, m + 14 + 236, y + 92, 90, 30);
        place(cancelBtn, w - m - 14 - 100, y + 92, 100, 30);
        place(loginStatus, m + 14, y + 130, w - 2 * m - 28, 28);
      } else {
        place(loginStatus, m + 14, y + 14, w - 2 * m - 28 - 110, 30);
        place(cancelBtn, w - m - 14 - 100, y + 14, 100, 30);
        place(retryBtn, w - m - 14 - 100, y + 14, 100, 30);
      }
      y += bh + 12;
    } else {
      loginBoxH = 0;
    }
    y += 4;

    // --- captures
    heading("CAPTURES");
    place(capCache, m, y, 300, 14);
    y += 16;
    place(cacheField, m, y, w - 2 * m - 90 - 8 - 80 - 8, 30);
    place(cacheBrowse, w - m - 90 - 8 - 80, y, 90, 30);
    place(cacheDefault, w - m - 80, y, 80, 30);
    y += 38;
    place(cacheCount, m, y, 420, 30);
    place(cacheOpen, w - m - 130, y, 130, 30);
    y += 48;

    // --- separation
    heading("SEPARATION");
    place(sepCombo, m, y, 200, 30);
    place(sepNote, m + 214, y, w - 2 * m - 214, 30);
    y += 48;

    // --- recording
    heading("RECORDING");
    place(capTakes, m, y, 300, 14);
    y += 16;
    place(takesField, m, y, w - 2 * m - 90 - 8 - 80 - 8, 30);
    place(takesBrowse, w - m - 90 - 8 - 80, y, 90, 30);
    place(takesDefault, w - m - 80, y, 80, 30);
    y += 48;

    // --- appearance
    heading("APPEARANCE");
    place(capTheme, m, y, 100, 14);
    place(capScale, m + 220, y, 100, 14);
    y += 16;
    place(themeCombo, m, y, 200, 30);
    place(scaleCombo, m + 220, y, 140, 30);
    place(scaleNote, m + 374, y, w - 2 * m - 374, 30);
    y += 54;

    // --- footer
    place(settingsFile, m, y, w - 2 * m - 190, 28);
    place(aboutBtn, w - m - 170, y, 170, 30);
    y += 48;

    bodyHeight = y;
    body.setSize(w, std::max(bodyHeight, owner.getHeight()));
    body.repaint();
  }
  int loginBoxY = 0, loginBoxH = 0;

  void paintBody(juce::Graphics& g) {
    g.fillAll(L::panel());
    for (const auto& h : headings) {
      g.setColour(L::saw());
      g.setFont(L::labelFont(12.0f));
      g.drawText(h.text, 20, h.y, 300, 18, juce::Justification::centredLeft);
      g.setColour(L::rule());
      g.fillRect(20, h.y + 20, body.getWidth() - 40, 1);
    }
    if (loginBoxH > 0) {
      const auto r = juce::Rectangle<int>(20, loginBoxY, body.getWidth() - 40, loginBoxH).toFloat();
      g.setColour(L::panelDeep());
      g.fillRoundedRectangle(r, 6.0f);
      g.setColour(L::saw().withAlpha(0.5f));
      g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.0f);
    }
  }
};

// ---------------------------------------------------------------------------------------------
SettingsPanel::SettingsPanel(SawbladeProcessor& p, Settings& s) : impl_(std::make_unique<Impl>(*this, p, s)) {
  setOpaque(true);
  setTitle("Settings panel");
  setWantsKeyboardFocus(true);
  setSize(kWidth, kHeight);
  impl_->build();
  resized();
  impl_->refresh();
}

SettingsPanel::~SettingsPanel() = default;

void SettingsPanel::paint(juce::Graphics& g) {
  g.fillAll(L::panel());
  g.setColour(L::saw().withAlpha(0.7f));
  g.fillRect(0, 0, getWidth(), 2);
  g.setColour(L::rule());
  g.fillRect(getWidth() - 1, 0, 1, getHeight());
}

void SettingsPanel::resized() {
  impl_->viewport.setBounds(0, 2, getWidth(), getHeight() - 2);
  impl_->closeBtn.setBounds(getWidth() - 10 - 30 - 12, 8, 30, 28);
  impl_->closeBtn.toFront(false);
  impl_->layout();
}

bool SettingsPanel::keyPressed(const juce::KeyPress& k) {
  if (k == juce::KeyPress::escapeKey) {
    close();
    return true;
  }
  return false;
}

void SettingsPanel::open(bool firstRun) {
  impl_->firstRunMode = firstRun;
  impl_->expanded = firstRun;
  setVisible(true);
  toFront(false);
  impl_->layout();
  impl_->refresh();
  impl_->startCount();
  if (isShowing()) grabKeyboardFocus();
}

void SettingsPanel::close() {
  if (impl_->s.isFirstRun()) impl_->s.markFirstRunCompleted();
  impl_->firstRunMode = false;
  setVisible(false);
  if (onClosed) onClosed();
}

void SettingsPanel::refresh() { impl_->refresh(); }
bool SettingsPanel::checklistExpanded() const { return impl_->expanded; }
int SettingsPanel::checklistLight(int row) const { return row >= 0 && row < 3 ? static_cast<int>(impl_->rowState[static_cast<size_t>(row)]) : 0; }
bool SettingsPanel::loginRunning() const { return impl_->loginJob && impl_->loginJob->isRunning(); }

}  // namespace sawblade::plugin::settings
