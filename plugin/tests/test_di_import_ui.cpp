// v0.2.1 Task B (plugin part), the editor side: IMPORT DI... in the take band and in the MATCH screen, the import dialog driven by
// the mouse in a host (Standalone = false), the drops, the IMPORTED mark and the take actions on an imported take. The decode / write
// path and the matcher's arguments are tested without a display in test_di_import.cpp. Needs a display (xvfb-run in CI).
// Screenshots go to SAWBLADE_SCREENSHOT_DIR (default build/screenshots): import_di_take_band.png, import_di_dialog.png.
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "DiImport.h"
#include "ImportDialog.h"
#include "MatchGlue.h"
#include "MatchScreen.h"
#include "PlayAlongPanel.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "fake_tools.h"
#include "sawblade/wav_io.h"

using namespace sawblade;
using namespace sawblade::plugin;
using nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

[[maybe_unused]] const bool kImportUiDataDirSet = [] {
  const fs::path d = fs::temp_directory_path() / ("sawblade_di_import_ui_data_" + std::to_string(::getpid()));
  ::setenv("SAWBLADE_DATA_DIR", d.c_str(), 1);
  return true;
}();

template <class T>
void collect(juce::Component& c, std::vector<T*>& out) {
  for (auto* child : c.getChildren()) {
    if (auto* t = dynamic_cast<T*>(child)) out.push_back(t);
    collect(*child, out);
  }
}
template <class T>
std::vector<T*> all(juce::Component& root) {
  std::vector<T*> v;
  collect<T>(root, v);
  return v;
}

juce::MouseEvent mouse(juce::Component& c, juce::Point<float> pos) {
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, &c, &c, now, pos, now, 1, true);
}
// A real click (mouse down + up inside the button).
void click(juce::Button& b) {
  juce::Component& c = b;
  const auto centre = b.getLocalBounds().toFloat().getCentre();
  c.mouseDown(mouse(b, centre));
  c.mouseUp(mouse(b, centre));
}

juce::Button* buttonTitled(juce::Component& root, const juce::String& title) {
  for (auto* b : all<juce::Button>(root))
    if (b->getTitle() == title) return b;
  return nullptr;
}
juce::TextEditor* fieldTitled(juce::Component& root, const juce::String& title) {
  for (auto* e : all<juce::TextEditor>(root))
    if (e->getTitle() == title) return e;
  return nullptr;
}
bool anyLabelContains(juce::Component& root, const juce::String& text) {
  for (auto* l : all<juce::Label>(root))
    if (l->isVisible() && l->getText().contains(text)) return true;
  return false;
}

template <class Pred>
bool pumpUntil(Pred pred, int timeoutMs = 15000) {
  const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (!pred()) {
    if (juce::Time::getMillisecondCounter() > end) return false;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(15);
  }
  return true;
}

struct Tmp {
  fs::path dir;
  Tmp() {
    dir = fs::temp_directory_path() / ("sawblade_di_import_ui_" + std::to_string(juce::Random::getSystemRandom().nextInt()));
    fs::create_directories(dir);
  }
  ~Tmp() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

// A processor (a HOST instance: Standalone is off) with its editor, the takes folder and the fake match tool in a temp dir.
struct UiRig {
  SettingsEnv env{kSettingsExist};
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  Tmp tmp;
  fake_tools::Toolbox tools;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;

  UiRig() : tools(tmp.dir / "tools") {
    proc.recorder().setTakesDir(tmp.dir / "takes");
    proc.jobs().setJobsDir(tools.jobs);
    proc.matchSettings().setFile(tools.root / "settings.xml");
    proc.waitForLoader(60000ms);
    proc.prepareToPlay(48000.0, 512);
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    REQUIRE_FALSE(proc.playAlong().standalone());
    ed->setVisible(true);  // getComponentAt() skips an invisible component
  }
  ~UiRig() { base.reset(); }

  PlayAlongPanel& panel() { return *all<PlayAlongPanel>(*ed).at(0); }
  MatchScreen& screen() { return *all<MatchScreen>(*ed).at(0); }
  TakeRecorder& rec() { return proc.recorder(); }
  ImportDialog* dialog() {
    for (auto* d : all<ImportDialog>(*ed))
      if (d->isVisible()) return d;
    return nullptr;
  }
  juce::ListBox& bandList() {
    for (auto* l : all<juce::ListBox>(panel()))
      if (l->getTitle() == "Takes") return *l;
    FAIL("no take list in the band");
    std::abort();
  }
  juce::ListBox& picker() {
    for (auto* l : all<juce::ListBox>(screen()))
      if (l->getTitle() == "Take picker") return *l;
    FAIL("no take picker on the match screen");
    std::abort();
  }
  // Clicks IMPORT in the open dialog and waits until the new take is the match DI and the dialog is gone.
  std::string importAndWait(std::size_t takesBefore) {
    auto* d = dialog();
    REQUIRE(d != nullptr);
    auto* go = buttonTitled(*d, "IMPORT");
    REQUIRE(go != nullptr);
    REQUIRE(go->isEnabled());
    click(*go);
    REQUIRE(pumpUntil([&] { return rec().listTakes().size() > takesBefore && !proc.matchSettings().selectedTake().empty() && dialog() == nullptr; }));
    pumpUntil([] { return false; }, 60);  // the deferred delete of the dialog
    return proc.matchSettings().selectedTake();
  }
};

// A DI: mono sine, or stereo with another sine on the right.
struct Di {
  std::vector<float> l, r;
};
Di writeDi(const fs::path& p, bool stereo, double rate = 48000.0, double seconds = 1.0, float amp = 0.5f) {
  const auto n = static_cast<std::size_t>(seconds * rate);
  Di d;
  d.l.resize(n);
  d.r.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    d.l[i] = amp * static_cast<float>(std::sin(0.031 * static_cast<double>(i)));
    d.r[i] = 0.5f * amp * static_cast<float>(std::sin(0.047 * static_cast<double>(i) + 1.0));
  }
  if (stereo) sawblade::writeWavFloat32Stereo(p, rate, d.l, d.r);
  else sawblade::writeWavFloat32(p, rate, d.l);
  return d;
}

fs::path writeSong(const fs::path& root, const std::string& name) {
  const fs::path dir = root / name;
  fs::create_directories(dir);
  std::vector<float> x(48000 * 8);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.3f * static_cast<float>(std::sin(0.02 * static_cast<double>(i)));
  for (const char* n : {"drums.wav", "bass.wav", "vocals.wav", "other.wav"}) sawblade::writeWavFloat32Stereo(dir / n, 48000.0, x, x);
  return dir;
}

std::vector<float> takeSamples(const TakeInfo& t) {
  const sawblade::AudioFile f = sawblade::readWav(t.wav);
  REQUIRE(f.channels == 1);
  return f.interleaved;
}

// What juce::ComponentPeer does for a file drop: the deepest component under the mouse that is a FileDragAndDropTarget and interested,
// walking up through the parents.
juce::Component* dropTargetAt(juce::Component& editor, juce::Point<int> p, const juce::StringArray& files) {
  for (auto* c = editor.getComponentAt(p); c != nullptr; c = c->getParentComponent())
    if (auto* t = dynamic_cast<juce::FileDragAndDropTarget*>(c))
      if (t->isInterestedInFileDrag(files)) return c;
  return nullptr;
}

juce::StringArray one(const fs::path& p) {
  juce::StringArray a;
  a.add(juce::String(p.string()));
  return a;
}

std::vector<std::string> argvOf(const fs::path& outDir) {
  std::ifstream f(outDir / "argv.json");
  const json j = json::parse(f, nullptr, false);
  std::vector<std::string> v;
  if (j.is_object() && j.contains("argv")) v = j["argv"].get<std::vector<std::string>>();
  return v;
}
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
std::string after(const std::vector<std::string>& v, const std::string& s) {
  const auto it = std::find(v.begin(), v.end(), s);
  return it != v.end() && it + 1 != v.end() ? *(it + 1) : std::string();
}

void savePng(const juce::Image& img, const juce::String& name) {
  const char* envDir = std::getenv("SAWBLADE_SCREENSHOT_DIR");
  const juce::File dir(envDir != nullptr && *envDir != '\0' ? envDir : SAWBLADE_SCREENSHOT_DIR);
  REQUIRE(dir.createDirectory().wasOk());
  const juce::File f = dir.getChildFile(name);
  f.deleteFile();
  juce::FileOutputStream out(f);
  REQUIRE(out.openedOk());
  juce::PNGImageFormat png;
  REQUIRE(png.writeImageToStream(img, out));
}

juce::String importTitle() { return juce::String::fromUTF8("IMPORT DI\xe2\x80\xa6"); }

}  // namespace

TEST_CASE("import ui: the take band and the MATCH screen both have IMPORT DI..., a files-only chooser, in a host too", "[editor][import]") {
  UiRig rig;
  rig.ed->setPlayAlongOpen(true);
  auto* band = buttonTitled(rig.panel(), importTitle());
  REQUIRE(band != nullptr);
  CHECK(band->isVisible());
  CHECK(band->isEnabled());
  CHECK(band->getTooltip().isNotEmpty());
  rig.ed->openMatchScreen();
  auto* scr = buttonTitled(rig.screen(), importTitle());
  REQUIRE(scr != nullptr);
  CHECK(scr->isVisible());
  CHECK(scr->isEnabled());  // no Standalone gate
  CHECK(scr->getTooltip().isNotEmpty());
  // The button sits in section 2 (right of REC), not over the song section.
  CHECK(scr->getY() > 250);
  CHECK(scr->getBounds().getX() > buttonTitled(rig.screen(), "REC")->getBounds().getRight());

  // The chooser: files only (never a folder), WAV / AIFF / FLAC; on macOS the filter is "*" and the pick is validated afterwards.
  const auto otherOs = DiImporter::chooserSpec(false), mac = DiImporter::chooserSpec(true);
  for (const auto& s : {otherOs, mac}) {
    CHECK((s.flags & juce::FileBrowserComponent::canSelectFiles) != 0);
    CHECK((s.flags & juce::FileBrowserComponent::canSelectDirectories) == 0);
    CHECK((s.flags & juce::FileBrowserComponent::openMode) != 0);
  }
  for (const char* ext : {"*.wav", "*.aif", "*.aiff", "*.flac"}) CHECK(otherOs.filter.contains(ext));
  CHECK_FALSE(otherOs.filter.contains("*.mp3"));
  CHECK(mac.filter == "*");

  // A pick that is not importable is refused with a one-line reason before any dialog (the macOS filter lets everything through).
  { std::ofstream(rig.tmp.dir / "notes.txt") << "x"; }
  CHECK_FALSE(rig.screen().diImporter().handlePicked(juce::File(juce::String((rig.tmp.dir / "notes.txt").string()))));
  CHECK(rig.dialog() == nullptr);
  rig.screen().refresh();
  CHECK(anyLabelContains(rig.screen(), "WAV, AIFF or FLAC"));
  rig.ed->setPlayAlongOpen(true);
  CHECK_FALSE(rig.panel().diImporter().handlePicked(juce::File(juce::String((rig.tmp.dir / "notes.mp3").string()))));  // missing file
  CHECK_FALSE(rig.panel().diImporter().handlePicked(juce::File()));                                                  // cancelled: silent
  CHECK(rig.dialog() == nullptr);
  CHECK(rig.rec().listTakes().empty());
}

TEST_CASE("import ui: the dialog by mouse: defaults, validation, same performance and don't know (take band, host mode)", "[editor][import]") {
  UiRig rig;
  rig.ed->setPlayAlongOpen(true);
  const fs::path src = rig.tmp.dir / "My DI.wav";
  const Di di = writeDi(src, /*stereo=*/false);

  REQUIRE(rig.panel().diImporter().handlePicked(juce::File(juce::String(src.string()))));
  ImportDialog* d = rig.dialog();
  REQUIRE(d != nullptr);
  CHECK(anyLabelContains(*d, "My DI.wav"));
  // Defaults.
  auto* same = buttonTitled(*d, "Same performance as the song");
  auto* dontKnow = buttonTitled(*d, "Don't know where it starts");
  auto* field = fieldTitled(*d, "DI start time");
  REQUIRE(same != nullptr);
  REQUIRE(dontKnow != nullptr);
  REQUIRE(field != nullptr);
  CHECK(same->getButtonText() == "same performance as the song (my own recording)");
  CHECK(dontKnow->getButtonText() == "don't know");
  CHECK_FALSE(same->getToggleState());
  CHECK_FALSE(dontKnow->getToggleState());
  CHECK(field->getText() == "0:00.000");
  CHECK(anyLabelContains(*d, "a DI bounced from the start of the song: leave 0:00"));
  CHECK(same->getTooltip().isNotEmpty());
  CHECK(dontKnow->getTooltip().isNotEmpty());
  CHECK(field->getTooltip().isNotEmpty());
  // A mono file: no channel choice.
  for (const char* t : {"Left channel", "Right channel", "Sum of both channels"}) {
    auto* b = buttonTitled(*d, t);
    REQUIRE(b != nullptr);
    CHECK_FALSE(b->isVisible());
  }
  // The start time only matters for the same performance: off, it is greyed out.
  CHECK_FALSE(field->isEnabled());
  CHECK_FALSE(dontKnow->isEnabled());
  click(*same);
  CHECK(same->getToggleState());
  CHECK(field->isEnabled());
  CHECK(dontKnow->isEnabled());

  // A bad time is refused in the dialog; nothing is imported and the dialog stays.
  field->setText("1:75", true);
  click(*buttonTitled(*d, "IMPORT"));
  CHECK(anyLabelContains(*d, "for example 1:23.500"));
  CHECK(rig.dialog() == d);
  CHECK_FALSE(d->importing());
  CHECK(rig.rec().listTakes().empty());
  // "don't know" takes the time field out of play (a bad time no longer matters).
  click(*dontKnow);
  CHECK(dontKnow->getToggleState());
  CHECK_FALSE(field->isEnabled());
  click(*dontKnow);
  CHECK(field->isEnabled());

  // Same performance, starts at 0:12.345.
  field->setText("0:12.345", true);
  const std::string a = rig.importAndWait(0);
  CHECK(a == "My DI");
  auto takes = rig.rec().listTakes();
  REQUIRE(takes.size() == 1);
  CHECK(takes[0].imported.present);
  CHECK(takes[0].imported.source == "My DI.wav");
  CHECK(takes[0].imported.channel == "mono");
  CHECK(takes[0].imported.samePerformance);
  REQUIRE(takes[0].offsetMs().has_value());
  CHECK(*takes[0].offsetMs() == Catch::Approx(12345.0));
  CHECK(takeSamples(takes[0]) == di.l);
  CHECK(takes[0].sampleRate == 48000.0);
  CHECK(fs::exists(src));  // a copy

  // The band chose and selected it, and marks it IMPORTED.
  rig.panel().refresh();
  REQUIRE(rig.bandList().getListBoxModel()->getNumRows() == 1);
  CHECK(rig.bandList().getSelectedRow() == 0);
  CHECK(rig.proc.matchSettings().selectedTake() == a);
  CHECK(takeOriginText(takes[0]) == "IMPORTED");

  // The same file again, "same performance" + "don't know": a new take, no offset.
  REQUIRE(rig.panel().diImporter().handlePicked(juce::File(juce::String(src.string()))));
  d = rig.dialog();
  REQUIRE(d != nullptr);
  click(*buttonTitled(*d, "Same performance as the song"));
  click(*buttonTitled(*d, "Don't know where it starts"));
  const std::string b = rig.importAndWait(1);
  CHECK(b == "My DI-2");
  takes = rig.rec().listTakes();
  REQUIRE(takes.size() == 2);
  const auto tb = *std::find_if(takes.begin(), takes.end(), [&](const TakeInfo& t) { return t.name == b; });
  CHECK(tb.imported.samePerformance);
  CHECK_FALSE(tb.offsetMs().has_value());

  // And once more with the defaults: a different performance, no timing.
  REQUIRE(rig.panel().diImporter().handlePicked(juce::File(juce::String(src.string()))));
  const std::string c = rig.importAndWait(2);
  CHECK(c == "My DI-3");
  takes = rig.rec().listTakes();
  const auto tc = *std::find_if(takes.begin(), takes.end(), [&](const TakeInfo& t) { return t.name == c; });
  CHECK_FALSE(tc.imported.samePerformance);
  CHECK_FALSE(tc.offsetMs().has_value());
  rig.panel().refresh();
  CHECK(rig.bandList().getListBoxModel()->getNumRows() == 3);
  CHECK(rig.bandList().getSelectedRow() == 0);  // newest first: the new take is row 0 and selected
}

TEST_CASE("import ui: a stereo file asks left / right / sum (left by default)", "[editor][import]") {
  UiRig rig;
  rig.ed->setPlayAlongOpen(true);
  const fs::path src = rig.tmp.dir / "stereo.wav";
  const Di di = writeDi(src, /*stereo=*/true, 44100.0, 0.5);
  auto want = [&](int which) {
    std::vector<float> w(di.l.size());
    for (std::size_t i = 0; i < w.size(); ++i) w[i] = which == 0 ? di.l[i] : which == 1 ? di.r[i] : 0.5f * (di.l[i] + di.r[i]);
    return w;
  };
  struct Pick { const char* title; int which; const char* channel; };
  std::size_t before = 0;
  for (const Pick& pk : {Pick{nullptr, 0, "left"}, Pick{"Right channel", 1, "right"}, Pick{"Sum of both channels", 2, "sum"}}) {
    REQUIRE(rig.panel().diImporter().handlePicked(juce::File(juce::String(src.string()))));
    ImportDialog* d = rig.dialog();
    REQUIRE(d != nullptr);
    auto* left = buttonTitled(*d, "Left channel");
    auto* right = buttonTitled(*d, "Right channel");
    auto* sum = buttonTitled(*d, "Sum of both channels");
    for (auto* b : {left, right, sum}) {
      REQUIRE(b != nullptr);
      CHECK(b->isVisible());
      CHECK(b->getTooltip().isNotEmpty());
    }
    CHECK(left->getToggleState());  // the default
    CHECK_FALSE(right->getToggleState());
    CHECK_FALSE(sum->getToggleState());
    if (pk.title != nullptr) {
      click(*buttonTitled(*d, pk.title));
      CHECK_FALSE(left->getToggleState());  // one of three
      CHECK(buttonTitled(*d, pk.title)->getToggleState());
    }
    const std::string name = rig.importAndWait(before++);
    const auto takes = rig.rec().listTakes();
    const auto t = *std::find_if(takes.begin(), takes.end(), [&](const TakeInfo& x) { return x.name == name; });
    INFO(pk.channel);
    CHECK(t.imported.channel == pk.channel);
    CHECK(t.sampleRate == 44100.0);  // the file's own rate, no resampling
    CHECK(takeSamples(t) == want(pk.which));
  }
}

TEST_CASE("import ui: a silent or a clipped file says why in the dialog, imports nothing and CANCEL closes it", "[editor][import]") {
  UiRig rig;
  rig.ed->setPlayAlongOpen(true);
  const fs::path quiet = rig.tmp.dir / "quiet.wav";
  writeDi(quiet, false, 48000.0, 1.0, 0.0004f);  // -68 dBFS
  const fs::path hot = rig.tmp.dir / "hot.wav";
  {
    Di d = writeDi(hot, false);
    for (std::size_t i = 100; i < 120; ++i) d.l[i] = 1.0f;
    sawblade::writeWavFloat32(hot, 48000.0, d.l);
  }
  for (const auto& [file, word] : {std::pair<fs::path, const char*>{quiet, "silent"}, {hot, "clipped"}}) {
    INFO(file.string());
    REQUIRE(rig.panel().diImporter().handlePicked(juce::File(juce::String(file.string()))));
    ImportDialog* d = rig.dialog();
    REQUIRE(d != nullptr);
    click(*buttonTitled(*d, "IMPORT"));
    REQUIRE(pumpUntil([&] { return anyLabelContains(*d, word); }));
    CHECK_FALSE(d->importing());
    CHECK(buttonTitled(*d, "IMPORT")->isEnabled());  // usable again
    CHECK(rig.dialog() == d);
    CHECK(rig.rec().listTakes().empty());
    for (auto* l : all<juce::Label>(*d))
      if (l->getText().contains(word)) CHECK_FALSE(l->getText().containsChar('\n'));  // one line
    click(*buttonTitled(*d, "CANCEL IMPORT"));
    CHECK(rig.dialog() == nullptr);
    pumpUntil([&] { return all<ImportDialog>(*rig.ed).empty(); }, 2000);
    CHECK(all<ImportDialog>(*rig.ed).empty());
  }
  CHECK(rig.rec().listTakes().empty());
  CHECK(rig.proc.matchSettings().selectedTake().empty());
}

TEST_CASE("import ui: a file dropped on the take list imports; on the song area it is a song, not a DI", "[editor][import][drop]") {
  UiRig rig;
  rig.ed->setPlayAlongOpen(true);
  rig.panel().refresh();
  const fs::path wav = rig.tmp.dir / "dropped di.wav";
  writeDi(wav, false);
  const fs::path song = writeSong(rig.tmp.dir, "Dropped Song");
  juce::StringArray mp3 = one(rig.tmp.dir / "x.mp3");
  { std::ofstream(rig.tmp.dir / "x.mp3") << "x"; }

  auto& list = rig.bandList();
  const auto listPt = rig.ed->getLocalArea(&list, list.getLocalBounds()).getCentre();
  // On the take list: a WAV / AIFF / FLAC is taken by the list itself ...
  for (const char* ext : {"wav", "WAV", "aif", "aiff", "flac"}) {
    juce::StringArray f;
    f.add(juce::String((rig.tmp.dir / (std::string("a.") + ext)).string()));
    INFO(ext);
    CHECK(dropTargetAt(*rig.ed, listPt, f) == &list);
  }
  // ... a folder or an mp3 there is not a DI: it falls through to the panel's song drop.
  CHECK(dropTargetAt(*rig.ed, listPt, one(song)) == &rig.panel());
  CHECK(dropTargetAt(*rig.ed, listPt, mp3) == &rig.panel());
  // On the song area (the top of the panel) a WAV is a song file: the panel takes it, never the take list.
  const auto songPt = rig.ed->getLocalArea(&rig.panel(), juce::Rectangle<int>(640, 60, 1, 1)).getTopLeft();
  CHECK(dropTargetAt(*rig.ed, songPt, one(wav)) == &rig.panel());
  CHECK(dropTargetAt(*rig.ed, songPt, one(song)) == &rig.panel());
  // The rig area outside the panel: the editor, as before.
  CHECK(dropTargetAt(*rig.ed, {640, 300}, one(wav)) == rig.ed);

  // Drop the WAV on the list: the import dialog opens (no take yet), IMPORT makes the take.
  auto* t = dynamic_cast<juce::FileDragAndDropTarget*>(dropTargetAt(*rig.ed, listPt, one(wav)));
  REQUIRE(t != nullptr);
  t->filesDropped(one(wav), 10, 10);
  REQUIRE(rig.dialog() != nullptr);
  CHECK(rig.rec().listTakes().empty());
  CHECK(rig.dialog()->isInterestedInFileDrag(one(song)));  // modal: a second drop onto the dialog is swallowed, not a song load
  const std::string name = rig.importAndWait(0);
  CHECK(name == "dropped di");
  CHECK(rig.rec().listTakes().at(0).imported.source == "dropped di.wav");

  // Drop a stems folder on the song area: the song loads, nothing is imported.
  auto* sp = dynamic_cast<juce::FileDragAndDropTarget*>(dropTargetAt(*rig.ed, songPt, one(song)));
  REQUIRE(sp != nullptr);
  sp->filesDropped(one(song), 10, 10);
  REQUIRE(rig.proc.playAlong().waitForLoader());
  CHECK(rig.proc.playAlong().settings().folder == song.string());
  CHECK(rig.dialog() == nullptr);
  CHECK(rig.rec().listTakes().size() == 1);
}

TEST_CASE("import ui: on the MATCH screen a DI file dropped on section 2 imports, anywhere else it is the song", "[editor][import][drop]") {
  UiRig rig;
  rig.ed->openMatchScreen();
  MatchScreen& screen = rig.screen();
  const fs::path wav = rig.tmp.dir / "match di.wav";
  writeDi(wav, false);
  const fs::path song = writeSong(rig.tmp.dir, "Screen Song");

  const juce::Point<int> section1(100, 120), section2(100, 300);
  CHECK(screen.isInterestedInFileDrag(one(wav)));
  CHECK(screen.dropSectionAt(one(wav), section2) == 2);  // a DI over section 2
  CHECK(screen.dropSectionAt(one(wav), section1) == 1);  // the same file over section 1 is a song
  CHECK(screen.dropSectionAt(one(song), section2) == 1);  // a folder is never a DI
  // The real drop target walk, from the editor's coordinates (the screen sits below the 58 px top bar).
  const auto toEditor = [&](juce::Point<int> p) { return rig.ed->getLocalArea(&screen, juce::Rectangle<int>(p.x, p.y, 1, 1)).getTopLeft(); };
  CHECK(dropTargetAt(*rig.ed, toEditor(section2), one(wav)) == &screen);

  screen.fileDragEnter(one(wav), section2.x, section2.y);
  screen.fileDragMove(one(wav), section1.x, section1.y);
  screen.fileDragExit(one(wav));
  screen.filesDropped(one(wav), section2.x, section2.y);
  REQUIRE(rig.dialog() != nullptr);
  CHECK(rig.rec().listTakes().empty());
  const std::string name = rig.importAndWait(0);  // "after import the new take is chosen"
  CHECK(name == "match di");
  screen.refresh();
  CHECK(rig.picker().getListBoxModel()->getNumRows() == 1);
  CHECK(rig.picker().getSelectedRow() == 0);
  CHECK(anyLabelContains(screen, "IMPORTED"));

  // A song dropped over section 1 loads in place; no dialog.
  screen.filesDropped(one(song), section1.x, section1.y);
  REQUIRE(rig.proc.playAlong().waitForLoader());
  CHECK(rig.proc.playAlong().settings().folder == song.string());
  CHECK(rig.dialog() == nullptr);
  CHECK(rig.rec().listTakes().size() == 1);
}

TEST_CASE("import ui: load a song, import a same-performance DI on the MATCH screen and START MATCH passes --matched mono and the offset", "[editor][import][match]") {
  using A = song_input::Action;
  UiRig rig;
  rig.tools.cfgMatch({{"progressJson", true}});
  rig.ed->openMatchScreen();
  MatchScreen& screen = rig.screen();
  auto* start = buttonTitled(screen, "START MATCH");
  REQUIRE(start != nullptr);
  CHECK_FALSE(start->isEnabled());

  const fs::path song = writeSong(rig.tmp.dir, "Song (stems)");
  REQUIRE(screen.handlePicked(A::StemsFolder, juce::File(juce::String(song.string()))));
  REQUIRE(rig.proc.playAlong().waitForLoader());
  screen.refresh();
  CHECK_FALSE(start->isEnabled());
  CHECK(anyLabelContains(screen, "record or import a DI"));

  const fs::path src = rig.tmp.dir / "bounce.wav";
  writeDi(src, false, 48000.0, 2.0);

  // 1. Same performance, starts 0:12.345 into the song.
  REQUIRE(screen.diImporter().handlePicked(juce::File(juce::String(src.string()))));
  click(*buttonTitled(*rig.dialog(), "Same performance as the song"));
  fieldTitled(*rig.dialog(), "DI start time")->setText("0:12.345", true);
  const std::string a = rig.importAndWait(0);
  screen.refresh();
  CHECK(start->isEnabled());
  CHECK_FALSE(anyLabelContains(screen, "record or import a DI"));
  CHECK(anyLabelContains(screen, "same performance as the song: starts 12.3"));
  CHECK(rig.picker().getSelectedRow() == 0);
  click(*start);
  REQUIRE(rig.proc.jobs().waitFinished(JobKind::Match, 15000ms));
  auto argv = argvOf(rig.proc.jobs().snapshot(JobKind::Match).dir);
  CHECK(after(argv, "--di") == (rig.tmp.dir / "takes" / (a + ".wav")).string());
  CHECK(after(argv, "--ref-channel") == "mid");
  CHECK(after(argv, "--matched") == "mono");
  CHECK(std::stod(after(argv, "--offset-ms")) == Catch::Approx(12345.0));

  // 2. "don't know": still a matched pair, but no offset (the matcher searches the whole song).
  REQUIRE(screen.diImporter().handlePicked(juce::File(juce::String(src.string()))));
  click(*buttonTitled(*rig.dialog(), "Same performance as the song"));
  click(*buttonTitled(*rig.dialog(), "Don't know where it starts"));
  const std::string b = rig.importAndWait(1);
  CHECK(b != a);
  screen.refresh();
  REQUIRE(start->isEnabled());
  click(*start);
  REQUIRE(rig.proc.jobs().waitFinished(JobKind::Match, 15000ms));
  argv = argvOf(rig.proc.jobs().snapshot(JobKind::Match).dir);
  CHECK(after(argv, "--di") == (rig.tmp.dir / "takes" / (b + ".wav")).string());
  CHECK(after(argv, "--matched") == "mono");
  CHECK_FALSE(has(argv, "--offset-ms"));

  // 3. A different performance (the default): unmatched, no offset: as before.
  REQUIRE(screen.diImporter().handlePicked(juce::File(juce::String(src.string()))));
  const std::string c = rig.importAndWait(2);
  screen.refresh();
  REQUIRE(start->isEnabled());
  click(*start);
  REQUIRE(rig.proc.jobs().waitFinished(JobKind::Match, 15000ms));
  argv = argvOf(rig.proc.jobs().snapshot(JobKind::Match).dir);
  CHECK(after(argv, "--di") == (rig.tmp.dir / "takes" / (c + ".wav")).string());
  CHECK_FALSE(has(argv, "--matched"));
  CHECK_FALSE(has(argv, "--offset-ms"));
  CHECK(rig.ed->matchScreenOpen());  // never left the screen
}

TEST_CASE("import ui: USE FOR MATCH, RENAME and DELETE work on an imported take, in the band and in the picker", "[editor][import][take]") {
  UiRig rig;
  rig.ed->setPlayAlongOpen(true);
  const fs::path src = rig.tmp.dir / "keeper.wav";
  writeDi(src, false);
  REQUIRE(rig.panel().diImporter().handlePicked(juce::File(juce::String(src.string()))));
  const std::string a = rig.importAndWait(0);
  REQUIRE(rig.panel().diImporter().handlePicked(juce::File(juce::String(src.string()))));
  const std::string b = rig.importAndWait(1);
  CHECK(a == "keeper");
  CHECK(b == "keeper-2");

  PlayAlongPanel& panel = rig.panel();
  panel.refresh();
  juce::ListBox& list = rig.bandList();
  REQUIRE(list.getListBoxModel()->getNumRows() == 2);
  for (const char* t : {"RENAME", "DELETE", "USE FOR MATCH"}) {
    INFO(t);
    CHECK(buttonTitled(panel, t)->isEnabled());
  }
  // USE FOR MATCH on the older imported take.
  list.selectRow(1);
  click(*buttonTitled(panel, "USE FOR MATCH"));
  CHECK(rig.proc.matchSettings().selectedTake() == a);

  // RENAME / DELETE go through the glue the band's dialogs call: the selection follows a rename; a delete clears it.
  std::string err;
  REQUIRE(renameTakeForMatch(rig.proc, a, "my bounce", &err));
  CHECK(rig.proc.matchSettings().selectedTake() == "my bounce");
  CHECK(fs::exists(rig.tmp.dir / "takes" / "my bounce.wav"));
  CHECK(fs::exists(rig.tmp.dir / "takes" / "my bounce.json"));
  panel.refresh();
  rig.ed->openMatchScreen();
  rig.screen().refresh();
  CHECK(rig.picker().getListBoxModel()->getNumRows() == 2);
  CHECK(rig.screen().isVisible());
  bool renamedShown = false;
  for (const auto& t : rig.rec().listTakes())
    if (t.name == "my bounce") renamedShown = t.imported.present && t.imported.source == "keeper.wav";
  CHECK(renamedShown);  // the sidecar moved with the take: still IMPORTED, still knows its source
  REQUIRE(deleteTakeForMatch(rig.proc, "my bounce"));
  CHECK(rig.proc.matchSettings().selectedTake().empty());
  CHECK_FALSE(fs::exists(rig.tmp.dir / "takes" / "my bounce.wav"));
  CHECK_FALSE(fs::exists(rig.tmp.dir / "takes" / "my bounce.json"));
  rig.screen().refresh();
  CHECK(rig.picker().getListBoxModel()->getNumRows() == 1);
  CHECK(fs::exists(src));  // the original is never touched
}

TEST_CASE("import ui: screenshots of the take band with an imported take and of the dialog", "[editor][import][screenshot]") {
  UiRig rig;
  rig.ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  rig.ed->setPlayAlongOpen(true);
  const fs::path src = rig.tmp.dir / "Rhythm DI take 3.wav";
  writeDi(src, true);
  REQUIRE(rig.panel().diImporter().handlePicked(juce::File(juce::String(src.string()))));
  juce::Image dialogShot = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 1.0f);
  CHECK(dialogShot.getWidth() == SawbladeEditor::kDesignWidth);
  savePng(dialogShot, "import_di_dialog.png");
  click(*buttonTitled(*rig.dialog(), "Same performance as the song"));
  rig.importAndWait(0);
  rig.panel().refresh();
  const juce::Image band = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 1.0f);
  savePng(band, "import_di_take_band.png");
  CHECK(band.getHeight() == SawbladeEditor::kDesignHeight);
}
