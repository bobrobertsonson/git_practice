// v0.4 Task B (docs/specs/v0_4-pedals.md): capture pedals on the pedalboard. A capture is a nam block in a pedal slot; it looks different
// from a modeled pedal (badge, outline, flat panel), is added from the picker's CAPTURES tab (the local capture cache) or the capture
// browser in insert mode, drops in at matched loudness (LEVEL MATCH), and has a LEVEL knob and a setting selector. Needs a display.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include "ExportGlue.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SawbladeLookAndFeel.h"
#include "SettingsEnv.h"
#include "browser/CaptureBrowser.h"
#include "rig/Pedalboard.h"
#include "rig/PedalKind.h"
#include "rig/PedalPicker.h"
#include "rig/RigEditorPanel.h"
#include "rig/RigModel.h"
#include "sawblade/auto_trim.h"
#include "sawblade/block_registry.h"
#include "sawblade/sha256.h"
#include "skin/FilmstripKnob.h"

using namespace sawblade;
using namespace sawblade::plugin;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kFx = SAWBLADE_FIXTURES_DIR;
constexpr auto kLoad = std::chrono::milliseconds(60000);

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
// Every component under `root` that is a pedal widget, in order.
void collectKinds(juce::Component& root, std::vector<juce::Component*>& out) {
  for (auto* child : root.getChildren()) {
    if (dynamic_cast<rig::HasPedalKind*>(child) != nullptr) out.push_back(child);
    collectKinds(*child, out);
  }
}
rig::PedalKind kindOf(juce::Component& c) {
  auto* k = dynamic_cast<rig::HasPedalKind*>(&c);
  REQUIRE(k != nullptr);
  return k->pedalKind();
}

juce::MouseEvent ev(juce::Component& c, juce::Point<float> pos) {
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, &c, &c, now, pos, now, 1, true);
}
void click(juce::Component& c) {
  const auto centre = c.getLocalBounds().toFloat().getCentre();
  c.mouseDown(ev(c, centre));
  c.mouseUp(ev(c, centre));
}
void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }
bool pumpUntil(const std::function<bool()>& pred, int timeoutMs = 60000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) return false;
    pump(10);
  }
  return true;
}

struct TempDir {
  fs::path dir;
  TempDir() {
    dir = fs::temp_directory_path() / ("sawblade_capturepedal_tests_" + std::to_string(juce::Random::getSystemRandom().nextInt64() & 0xffffffff));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

struct ToneModel {
  std::string id, name, fixture;
};
// <cache>/<tone>/meta.json + the model files, as `sawblade-t3k fetch` writes them.
void writeTone(const fs::path& cache, const std::string& tone, const std::string& gear, const std::string& title, const std::vector<ToneModel>& models) {
  fs::create_directories(cache / tone);
  json meta = {{"tone", {{"title", title}, {"gear", gear}, {"license", "cc-by-nc"}, {"url", "https://www.tone3000.com/tones/" + tone}, {"user", {{"username", "someone"}}}}},
               {"creatorUsername", "someone"}, {"models", json::object()}};
  for (const auto& m : models) {
    fs::copy_file(kFx / "nam" / m.fixture, cache / tone / (m.id + ".nam"), fs::copy_options::overwrite_existing);
    // the real hash: a capture made from this entry carries it, and the load verifies it (a made-up one is a load error)
    std::ifstream in(cache / tone / (m.id + ".nam"), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    meta["models"][m.id] = {{"file", m.id + ".nam"}, {"sha256", sha256Hex(bytes.data(), bytes.size())}, {"model", {{"name", m.name}}}};
  }
  std::ofstream(cache / tone / "meta.json") << meta.dump(2);
}

json namAmp(const std::string& id) { return {{"id", id}, {"type", "nam"}, {"slot", "amp"}, {"model", {{"file", (kFx / "nam" / "linear_identity.nam").string()}}}}; }
json tsBlock(const std::string& id) {
  return {{"id", id}, {"type", "pedal.ts"}, {"slot", "boost"}, {"modelVersion", 1}, {"params", {{"drive", 3}, {"tone", 6}, {"level", 8}}}};
}
json capBlock(const std::string& id, const fs::path& cache, const std::string& tone, const std::string& model, const std::string& title) {
  return {{"id", id}, {"type", "nam"}, {"slot", "pedal"},
          {"model", {{"file", (cache / tone / (model + ".nam")).string()},
                     {"source", {{"provider", "tone3000"}, {"id", tone}, {"modelId", model}, {"title", title}, {"creator", "someone"}, {"license", "cc-by-nc"}}}}}};
}
json rigJson(const std::vector<json>& a, const std::vector<json>& b, bool bOn) {
  json ja = json::array(), jb = json::array();
  for (const json& blk : a) ja.push_back(blk);
  for (const json& blk : b) jb.push_back(blk);
  return {{"schema", "sawblade.preset"},
          {"version", 3},
          {"name", "capture pedal test"},
          {"paths", {{"a", {{"role", "saw"}, {"blocks", ja}}}, {"b", {{"role", "body"}, {"enabled", bOn}, {"blocks", jb}}}}},
          {"align", {{"mode", "off"}}},
          {"blend", bOn ? 0.5 : 0.0},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", (kFx / "ir" / "impulse.wav").string()}}}}}};
}

struct Rig {
  SettingsEnv env{kSettingsExist, /*isolateHome=*/true};
  TempDir tmp;
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;
  int files = 0;
  fs::path cache;

  Rig() {
    cache = env.dir / "cache";  // SettingsEnv points SAWBLADE_CACHE_DIR here: the capture cache of this test
    fs::create_directories(cache);
    proc.prepareToPlay(48000.0, 512);
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  }
  ~Rig() { base.reset(); }

  void settle() {
    REQUIRE(proc.waitForLoader(kLoad));
    INFO("the processor's load error: " << proc.status().error);
    REQUIRE(proc.status().error.empty());
    pump(40);
    ed->refreshNow();
  }
  void load(const json& j) {
    const fs::path f = tmp.dir / ("p" + std::to_string(files++) + ".json");
    std::ofstream(f) << j.dump(2);
    REQUIRE(proc.loadPresetFile(f));
    settle();
    proc.historyClear();
  }
  rig::Pedalboard& board() { return ed->pedalboard(); }
  Preset preset() { return proc.currentPreset(); }
};

// An environment variable for the life of the guard.
struct EnvGuard {
  std::string key;
  std::optional<std::string> old;
  EnvGuard(const std::string& k, const std::string& v) : key(k) {
    if (const char* c = std::getenv(k.c_str())) old = c;
    ::setenv(k.c_str(), v.c_str(), 1);
  }
  ~EnvGuard() {
    if (old) ::setenv(key.c_str(), old->c_str(), 1);
    else ::unsetenv(key.c_str());
  }
};
// How many times the fake tool was called with `cmd` (FAKE_T3K_LOG: one JSON argv per line).
int callsOf(const fs::path& log, const std::string& cmd) {
  int n = 0;
  std::ifstream in(log);
  for (std::string line; std::getline(in, line);)
    if (const auto j = json::parse(line, nullptr, false); j.is_array() && !j.empty() && j[0] == cmd) ++n;
  return n;
}
// A mouse drag on a knob in `moves` steps of `dy` pixels (negative = up).
void dragKnob(juce::Component& k, float dy, int moves) {
  const juce::Point<float> start(15.0f, 15.0f);
  const auto mk = [&](juce::Point<float> pos) {
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, &k, &k, now, start, now, 1, true);
  };
  k.mouseDown(mk(start));
  for (int i = 1; i <= moves; ++i) k.mouseDrag(mk({15.0f, 15.0f + dy * static_cast<float>(i)}));
  k.mouseUp(mk({15.0f, 15.0f + dy * static_cast<float>(moves)}));
}

const NamBlockParams& namOf(const Block& b) { return static_cast<const NamBlockParams&>(*b.params); }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("capture pedals: every pedal widget exposes its kind; a capture draws the badge and the cream outline, a modeled pedal and an amp do not", "[editor][capturepedals]") {
  Rig rig;
  writeTone(rig.cache, "777", "pedal", "Fuzz Tone", {{"7771", "Gain 2", "linear_identity.nam"}, {"7772", "Gain 8", "linear_identity.nam"}});
  rig.load(rigJson({tsBlock("a1"), capBlock("a2", rig.cache, "777", "7771", "Fuzz Tone"), namAmp("a3")}, {}, false));
  auto& pb = rig.board();
  REQUIRE(pb.tileCount(0) == 2);
  const juce::Colour cream = SawbladeLookAndFeel::capture();
  CHECK(cream == juce::Colour(0xffe8e1d2));

  // board tiles
  rig::BoardTile* modeled = pb.tile(0, 0);
  rig::BoardTile* capture = pb.tile(0, 1);
  CHECK(modeled->pedalKind() == rig::PedalKind::Modeled);
  CHECK_FALSE(modeled->isCapture());
  CHECK(capture->pedalKind() == rig::PedalKind::Capture);
  CHECK(capture->isCapture());
  const auto top = [&](rig::BoardTile& t) {  // the outline (2 px, cream) runs along the tile's edge
    const juce::Image img = t.createComponentSnapshot(t.getLocalBounds(), true, 1.0f);
    return img.getPixelAt(t.getWidth() / 2, 1);
  };
  const auto badge = [&](rig::BoardTile& t) {  // inside the filled CAPTURE badge, left of its text
    const juce::Image img = t.createComponentSnapshot(t.getLocalBounds(), true, 1.0f);
    return img.getPixelAt(juce::roundToInt(static_cast<float>(t.getWidth()) * 0.08f + 2.0f), juce::roundToInt(static_cast<float>(t.getHeight()) * 0.08f));
  };
  CHECK(top(*capture) == cream);
  CHECK(badge(*capture) == cream);
  CHECK_FALSE(top(*modeled) == cream);
  CHECK_FALSE(badge(*modeled) == cream);
  CHECK(rig::badgeText(rig::PedalKind::Capture) == "CAPTURE");
  CHECK(rig::badgeText(rig::PedalKind::Modeled).isEmpty());

  // the rig editor's slot cards: modeled pedal, capture pedal, amp (an amp capture is not a pedal: no badge)
  rig.ed->setRigEditorOpen(true);
  rig.ed->rigEditor().setTab(rig::RigEditorPanel::Tab::Chain);
  rig.ed->rigEditor().refresh();
  std::vector<juce::Component*> cards;
  collectKinds(rig.ed->rigEditor(), cards);
  REQUIRE(cards.size() == 3);  // path A's three blocks (path B is empty)
  CHECK(kindOf(*cards[0]) == rig::PedalKind::Modeled);
  CHECK(kindOf(*cards[1]) == rig::PedalKind::Capture);
  CHECK(kindOf(*cards[2]) == rig::PedalKind::Modeled);
  const auto edge = [&](juce::Component& c) { return c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f).getPixelAt(c.getWidth() / 2, 0); };
  const auto edge1 = [&](juce::Component& c) { return c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f).getPixelAt(c.getWidth() / 2, 1); };
  CHECK((edge(*cards[1]) == cream || edge1(*cards[1]) == cream));
  CHECK_FALSE((edge(*cards[0]) == cream || edge1(*cards[0]) == cream));
  CHECK_FALSE((edge(*cards[2]) == cream || edge1(*cards[2]) == cream));
  rig.ed->setRigEditorOpen(false);

  // picker rows
  click(pb.addButton(0));
  REQUIRE(pb.pickerOpen());
  rig::PedalPicker& picker = *pb.picker();
  CHECK(kindOf(picker.modelButton(0)) == rig::PedalKind::Modeled);
  REQUIRE(picker.captureRowCount() == 1);
  CHECK(kindOf(picker.captureRow(0)) == rig::PedalKind::Capture);
  juce::Component& row = picker.captureRow(0);
  CHECK(row.createComponentSnapshot(row.getLocalBounds(), true, 1.0f).getPixelAt(row.getWidth() / 2, 1) == cream);
  juce::Component& mrow = picker.modelButton(0);
  CHECK_FALSE(mrow.createComponentSnapshot(mrow.getLocalBounds(), true, 1.0f).getPixelAt(mrow.getWidth() / 2, 1) == cream);
}

TEST_CASE("capture pedals: the tile shows title, creator, licence tag, CAPTURE . FIXED TONE and a LEVEL knob, and no other knob", "[editor][capturepedals]") {
  Rig rig;
  writeTone(rig.cache, "777", "pedal", "Fuzz Tone", {{"7771", "Gain 2", "linear_identity.nam"}, {"7772", "Gain 8", "linear_identity.nam"}});
  rig.load(rigJson({capBlock("a1", rig.cache, "777", "7771", "Fuzz Tone"), namAmp("a2")}, {}, false));
  auto& pb = rig.board();
  rig::BoardTile* t = pb.tile(0, 0);
  REQUIRE(t != nullptr);
  CHECK(t->captureName() == "Fuzz Tone");
  CHECK(t->captureCreator() == "@someone");
  CHECK(t->captureLicence() == "CC-BY-NC");  // the licence tag (a -nc capture stays marked by its licence)
  CHECK(t->name() == "FUZZ TONE");
  CHECK(t->toneId() == "777");
  CHECK(t->modelId() == "7771");
  CHECK(t->isCircuit() == false);

  // exactly one knob: LEVEL, bound to the block's output level
  const auto knobs = all<skin::FilmstripKnob>(*t);
  REQUIRE(knobs.size() == 1);
  REQUIRE(t->levelKnob() != nullptr);
  CHECK(&t->levelKnob()->knob() == knobs[0]);
  CHECK(t->levelKnob()->knob().getTitle().startsWith("Level"));
  CHECK(namOf(rig.preset().a.blocks[0]).outputGainDb == 0.0);
  t->levelKnob()->knob().setValue(6.0, juce::sendNotificationSync);
  CHECK(namOf(rig.preset().a.blocks[0]).outputGainDb == 6.0);  // the nam block's output level, live
  CHECK(namOf(rig.preset().a.blocks[0]).inputGainDb == 0.0);
  CHECK(namOf(rig.preset().a.blocks[0]).makeupDb == 0.0);
  rig.proc.waitForLoader(kLoad);
  rig.ed->refreshNow();
  CHECK(pb.tile(0, 0)->levelKnob()->knob().getValue() == 6.0);  // the board shows it

  // the footswitch (and its LED) and the setting selector are the tile's only other controls
  CHECK(all<juce::Button>(*t).size() == 2);
  CHECK(t->hasSelector());  // two cached settings: the selector shows
  CHECK(t->selectorButton().isVisible());
}

TEST_CASE("capture pedals: the CAPTURES tab lists the cache (pedals only, by title) and a SEARCH row; adding from either tab is one undo step", "[editor][capturepedals][picker]") {
  Rig rig;
  writeTone(rig.cache, "777", "pedal", "Zeta Boost", {{"7771", "Gain 2", "linear_identity.nam"}, {"7772", "Gain 8", "linear_identity.nam"}});
  writeTone(rig.cache, "778", "pedal", "alpha fuzz", {{"7781", "Standard", "linear_identity.nam"}});
  writeTone(rig.cache, "779", "amp", "Some Amp", {{"7791", "Clean", "linear_identity.nam"}});
  rig.load(rigJson({tsBlock("a1"), namAmp("a2")}, {tsBlock("b1"), namAmp("b2")}, true));
  auto& pb = rig.board();
  const Preset before = rig.preset();

  click(pb.addButton(0));
  REQUIRE(pb.pickerOpen());
  rig::PedalPicker& picker = *pb.picker();
  REQUIRE(picker.tabCount() == 2);
  CHECK(picker.tabButton(0).getButtonText() == "MODELED");
  CHECK(picker.tabButton(1).getButtonText() == "CAPTURES");
  CHECK(picker.selectedTab() == 0);
  click(picker.tabButton(1));
  CHECK(picker.selectedTab() == 1);
  REQUIRE(picker.captureRowCount() == 2);  // the amp tone is not listed
  CHECK(picker.captureRow(0).getButtonText() == "alpha fuzz");  // by title, case-insensitively
  CHECK(picker.captureRow(1).getButtonText() == "Zeta Boost");
  CHECK(picker.captureRow(1).getTooltip().containsIgnoreCase("cc-by-nc"));  // the licence is shown
  CHECK(picker.captureRow(1).getTooltip().contains("@someone"));
  CHECK(picker.captureRow(1).getTooltip().contains("2 SETTINGS"));
  CHECK(picker.searchRow().getButtonText().contains("SEARCH TONE3000"));
  CHECK(picker.searchRow().isVisible());

  // add from CAPTURES: Zeta Boost goes in before the amp with its first setting, licence and creator kept
  click(picker.captureRow(1));
  CHECK_FALSE(pb.pickerOpen());
  rig.settle();
  const Preset a1 = rig.preset();
  REQUIRE(a1.a.blocks.size() == 3);
  const Block& nb = a1.a.blocks[1];
  CHECK(nb.type == "nam");
  CHECK(nb.slot == "pedal");
  REQUIRE(namOf(nb).model.source.has_value());
  CHECK(namOf(nb).model.source->id == "777");
  CHECK(namOf(nb).model.source->modelId == "7771");  // the first setting in ladder order
  CHECK(namOf(nb).model.source->license == "cc-by-nc");
  CHECK(namOf(nb).model.source->creator == "someone");
  CHECK(a1.a.blocks.back().id == "a2");  // the amp stays last
  CHECK(pb.tileCount(0) == 2);
  CHECK(pb.tile(0, 1)->pedalKind() == rig::PedalKind::Capture);
  CHECK(pb.tile(0, 1)->hasSelector());  // two cached settings
  // (the level match for it lands in the background: waited for in the next test)
  REQUIRE(pumpUntil([&] { return rig.proc.levelWorker().idle(); }));
  pump(60);
  CHECK(rig.proc.undoSteps() == 1);

  // add from MODELED on the other board
  rig.proc.historyClear();
  click(pb.addButton(1));
  REQUIRE(pb.pickerOpen());
  CHECK(pb.picker()->path() == 1);
  CHECK(pb.picker()->selectedTab() == 0);  // opens on MODELED
  click(pb.picker()->modelButton(3));       // BIG FUZZ
  rig.settle();
  REQUIRE(rig.preset().b.blocks.size() == 3);
  CHECK(rig.preset().b.blocks[1].type == "pedal.muff");
  CHECK(rig.proc.undoSteps() == 1);
  CHECK(pb.tile(1, 1)->pedalKind() == rig::PedalKind::Modeled);  // no badge on a modeled pedal
  (void)before;

  // SEARCH TONE3000...: the editor is asked to open the browser in insert mode at the end of the board
  int askedPath = -1, askedIndex = -1;
  pb.onSearchRequest = [&](int p, int i) {
    askedPath = p;
    askedIndex = i;
  };
  click(pb.addButton(0));
  click(pb.picker()->tabButton(1));
  click(pb.picker()->searchRow());
  CHECK_FALSE(pb.pickerOpen());
  CHECK(askedPath == 0);
  CHECK(askedIndex == pb.tileCount(0));
}

TEST_CASE("capture pedals: the SEARCH row opens the capture browser in insert mode for that board", "[editor][capturepedals][picker][browser]") {
  Rig rig;
  rig.load(rigJson({tsBlock("a1"), namAmp("a2")}, {}, false));
  auto& pb = rig.board();
  click(pb.addButton(0));
  click(pb.picker()->tabButton(1));
  click(pb.picker()->searchRow());
  REQUIRE(rig.ed->captureBrowserOpen());
  auto browsers = all<CaptureBrowser>(*rig.ed);
  REQUIRE(browsers.size() == 1);
  auto& ctl = browsers[0]->controller();
  CHECK(ctl.insertMode());
  CHECK(ctl.slot() == Slot::SawPedal);
  CHECK(ctl.state().gear == "pedal");  // fixed to pedals
  ctl.setGear("amp");
  CHECK(ctl.state().gear == "pedal");
  const auto targets = ctl.targets();
  REQUIRE(targets.size() == 1);
  CHECK(targets[0].kind == SlotTarget::Kind::InsertNamBlock);
  CHECK(targets[0].path == 'a');
  CHECK(targets[0].blockIndex == 1);  // before the amp, after the pedal
  rig.ed->closeAllOverlaysForTests();
  REQUIRE(pumpUntil([&] { return all<CaptureBrowser>(*rig.ed).empty(); }, 20000));
}

TEST_CASE("capture pedals: a new capture drops in at matched loudness: the make-up lands without an extra undo step", "[editor][capturepedals][levelmatch]") {
  Rig rig;
  // model 7771 (the one the CAPTURES row adds) is quieter than the identity: 0.5 / 0.25 FIR taps, -2.6 LU on the reference DI (measured).
  // (linear_identity_loud24.nam would be no test: its metadata loudness makes the NAM block normalise it back to the identity's level.)
  writeTone(rig.cache, "777", "pedal", "Quiet Pedal", {{"7771", "Quiet", "linear_05_025.nam"}, {"7772", "Other", "linear_identity.nam"}});
  rig.load(rigJson({tsBlock("a1"), namAmp("a2")}, {}, false));
  REQUIRE(rig.proc.levelMatchEnabled());
  auto& pb = rig.board();
  const Preset before = rig.preset();

  click(pb.addButton(0));
  click(pb.picker()->tabButton(1));
  REQUIRE(pb.picker()->captureRowCount() == 1);
  click(pb.picker()->captureRow(0));
  rig.settle();
  REQUIRE(rig.preset().a.blocks.size() == 3);
  CHECK(rig.proc.undoSteps() == 1);  // the add
  const Preset added = rig.preset();

  // the make-up arrives later (background measurement) and brings the path back to its loudness: about +2.6 dB for this pedal
  REQUIRE(pumpUntil([&] { return slotMakeupOf(rig.preset(), 0, 1) != 0.0; }));
  const double mk = slotMakeupOf(rig.preset(), 0, 1);
  CHECK(mk > 2.0);
  CHECK(mk < 3.2);
  CHECK(mk <= kMaxSlotMakeupDb);
  CHECK(rig.proc.undoSteps() == 1);  // still one step: the make-up added none
  rig.settle();
  CHECK(pb.tileCount(0) == 2);

  // undo removes the pedal (the exact preset before), redo brings it back WITH its make-up
  REQUIRE(rig.ed->rigController().undo());
  rig.settle();
  CHECK(rig.preset() == before);
  CHECK_FALSE(rig.ed->rigController().canUndo());
  REQUIRE(rig.ed->rigController().redo());
  rig.settle();
  REQUIRE(rig.preset().a.blocks.size() == 3);
  CHECK(slotMakeupOf(rig.preset(), 0, 1) == mk);
  CHECK(rig.proc.undoSteps() == 1);
  (void)added;
}

TEST_CASE("capture pedals: the setting selector lists the cached models in order and swaps the capture in one undo step", "[editor][capturepedals][selector]") {
  Rig rig;
  writeTone(rig.cache, "777", "pedal", "Ladder Pedal", {{"7772", "Gain 8", "linear_identity.nam"}, {"7771", "Gain 2", "linear_identity.nam"}, {"7773", "Gain 5", "linear_identity.nam"}});
  writeTone(rig.cache, "778", "pedal", "Single Pedal", {{"7781", "Only", "linear_identity.nam"}});
  rig.load(rigJson({capBlock("a1", rig.cache, "777", "7771", "Ladder Pedal"), capBlock("a2", rig.cache, "778", "7781", "Single Pedal"), namAmp("a3")}, {}, false));
  auto& pb = rig.board();
  rig::BoardTile* ladder = pb.tile(0, 0);
  rig::BoardTile* single = pb.tile(0, 1);
  CHECK_FALSE(single->hasSelector());  // one model: no selector
  CHECK_FALSE(single->selectorButton().isVisible());
  REQUIRE(ladder->hasSelector());
  CHECK(ladder->selectorButton().isVisible());
  REQUIRE(ladder->settings().size() == 3);
  CHECK(ladder->settings()[0].name == "Gain 2");  // the v0.2 ladder order (by the number), not the model-id order
  CHECK(ladder->settings()[1].name == "Gain 5");
  CHECK(ladder->settings()[2].name == "Gain 8");
  CHECK(ladder->selectorButton().getButtonText().containsIgnoreCase("GAIN 2"));
  {
    const juce::PopupMenu m = ladder->settingsMenu();
    std::vector<juce::String> texts;
    std::vector<bool> ticks;
    juce::PopupMenu::MenuItemIterator it(m);
    while (it.next()) {
      texts.push_back(it.getItem().text);
      ticks.push_back(it.getItem().isTicked);
    }
    CHECK(texts == std::vector<juce::String>{"Gain 2", "Gain 5", "Gain 8"});
    CHECK(ticks == std::vector<bool>{true, false, false});
  }

  const Preset before = rig.preset();
  ladder->chooseSetting("7773");
  REQUIRE(pumpUntil([&] { return rig.preset().a.blocks[0].params && namOf(rig.preset().a.blocks[0]).model.source->modelId == "7773"; }));
  rig.settle();
  REQUIRE(rig.proc.waitForLoader(kLoad));
  const Preset after = rig.preset();
  CHECK(namOf(after.a.blocks[0]).model.source->modelId == "7773");
  CHECK(namOf(after.a.blocks[0]).model.source->title == "Ladder Pedal");
  CHECK(namOf(after.a.blocks[0]).model.source->license == "cc-by-nc");  // licence and creator kept
  CHECK(namOf(after.a.blocks[0]).model.file.find("7773.nam") != std::string::npos);
  CHECK(after.a.blocks[1] == before.a.blocks[1]);  // nothing else changed
  CHECK(after.a.blocks[0].id == "a1");
  CHECK(rig.proc.undoSteps() == 1);
  CHECK(pb.tile(0, 0)->modelId() == "7773");
  CHECK(pb.tile(0, 0)->selectorButton().getButtonText().containsIgnoreCase("GAIN 5"));

  REQUIRE(rig.ed->rigController().undo());
  rig.settle();
  CHECK(rig.preset() == before);
  CHECK_FALSE(rig.ed->rigController().canUndo());
  REQUIRE(rig.ed->rigController().redo());
  rig.settle();
  CHECK(namOf(rig.preset().a.blocks[0]).model.source->modelId == "7773");
}

TEST_CASE("capture pedals: a capture is a nam block for the export (NAM-trainable), its licence travels", "[editor][capturepedals][export]") {
  Rig rig;
  writeTone(rig.cache, "777", "pedal", "Fuzz Tone", {{"7771", "Gain 2", "linear_identity.nam"}});
  rig.load(rigJson({tsBlock("a1"), namAmp("a2")}, {}, false));
  click(rig.board().addButton(0));
  click(rig.board().picker()->tabButton(1));
  click(rig.board().picker()->captureRow(0));
  rig.settle();
  REQUIRE(pumpUntil([&] { return rig.proc.levelWorker().idle(); }));
  const Preset p = rig.preset();
  REQUIRE(p.a.blocks.size() == 3);
  CHECK(p.a.blocks[1].type == "nam");
  const BlockType* nam = BlockRegistry::instance().find("nam");
  REQUIRE(nam != nullptr);
  CHECK(nam->traits.namTrainable);
  const RigSummary s = summariseRig(p);
  CHECK(s.nonCommercial);  // cc-by-nc: the rig and anything exported from it is marked non-commercial
  bool credited = false;
  for (const auto& l : s.licences) credited = credited || (l.title == "Fuzz Tone" && l.creator == "someone" && l.license == "cc-by-nc" && l.nonCommercial);
  CHECK(credited);
  bool inChain = false;
  for (const auto& line : s.chainLines) inChain = inChain || line.find("Fuzz Tone") != std::string::npos;
  CHECK(inChain);
}

TEST_CASE("capture pedals: one LEVEL drag gesture on a capture tile is exactly one undo step", "[editor][capturepedals][undo]") {
  Rig rig;
  writeTone(rig.cache, "777", "pedal", "Fuzz Tone", {{"7771", "Gain 2", "linear_identity.nam"}});
  rig.load(rigJson({capBlock("a1", rig.cache, "777", "7771", "Fuzz Tone"), namAmp("a2")}, {}, false));
  const Preset before = rig.preset();
  REQUIRE(rig.proc.undoSteps() == 0);
  dragKnob(rig.board().tile(0, 0)->levelKnob()->knob(), -30.0f, 6);  // six mouse moves
  rig.settle();
  const double level = namOf(rig.preset().a.blocks[0]).outputGainDb;
  REQUIRE(level != 0.0);
  CHECK(rig.proc.undoSteps() == 1);
  REQUIRE(rig.ed->rigController().undo());
  rig.settle();
  CHECK(rig.preset() == before);
  CHECK_FALSE(rig.ed->rigController().canUndo());
  REQUIRE(rig.ed->rigController().redo());
  rig.settle();
  CHECK(namOf(rig.preset().a.blocks[0]).outputGainDb == level);
}

TEST_CASE("capture pedals: the selector lists the cached models plus the tone's online ones; an uncached choice is fetched and swapped in one undo step", "[editor][capturepedals][selector][online]") {
  Rig rig;
  const fs::path log = rig.tmp.dir / "calls.log";
  const EnvGuard net("SAWBLADE_NO_NETWORK", "0"), n("FAKE_T3K_MODELS_N", "3"), lg("FAKE_T3K_LOG", log.string());
  rig.board().setModelsExecutable([] { return std::string(SAWBLADE_FAKE_T3K); });
  writeTone(rig.cache, "777", "pedal", "Online Pedal", {{"7771", "Gain 1", "linear_identity.nam"}});  // one cached model; the tool knows three
  rig.load(rigJson({capBlock("a1", rig.cache, "777", "7771", "Online Pedal"), namAmp("a2")}, {}, false));
  auto& pb = rig.board();
  REQUIRE(pumpUntil([&] { return pb.tile(0, 0)->settings().size() == 3; }));
  rig.ed->refreshNow();
  rig.ed->refreshNow();
  CHECK(callsOf(log, "models") == 1);  // once per tone per session, however often the board refreshes
  rig::BoardTile* t = pb.tile(0, 0);
  CHECK(t->hasSelector());
  CHECK(t->selectorButton().isVisible());
  CHECK(t->settings()[0].name == "Gain 1");
  CHECK(t->settings()[1].name == "Gain 2");
  CHECK(t->settings()[2].name == "Gain 3");

  const Preset before = rig.preset();
  t->chooseSetting("7773");  // not in the cache
  CHECK(t->fetching());
  CHECK(t->selectorButton().getButtonText().containsIgnoreCase("FETCHING"));
  CHECK_FALSE(t->selectorButton().isEnabled());
  REQUIRE(pumpUntil([&] { return namOf(rig.preset().a.blocks[0]).model.source && namOf(rig.preset().a.blocks[0]).model.source->modelId == "7773"; }));
  rig.settle();
  CHECK(callsOf(log, "fetch") == 1);
  const Preset after = rig.preset();
  CHECK(after.a.blocks[0].id == "a1");
  CHECK(after.a.blocks[1] == before.a.blocks[1]);
  CHECK(namOf(after.a.blocks[0]).model.source->license == "cc-by");  // the fetch's licence and creator travel with it
  CHECK(namOf(after.a.blocks[0]).model.source->creator == "fakecreator");
  CHECK(rig.proc.undoSteps() == 1);
  CHECK_FALSE(pb.tile(0, 0)->fetching());
  CHECK(pb.tile(0, 0)->modelId() == "7773");
  CHECK(pb.tile(0, 0)->settings().size() == 3);
  CHECK(callsOf(log, "models") == 1);  // the rebuilt tile did not ask again

  REQUIRE(rig.ed->rigController().undo());
  rig.settle();
  CHECK(rig.preset() == before);
  CHECK_FALSE(rig.ed->rigController().canUndo());
}

TEST_CASE("capture pedals: the online list is asked once; a failing or disabled tool leaves the cached settings only", "[editor][capturepedals][selector][online]") {
  const bool fail = GENERATE(true, false);
  Rig rig;
  const fs::path log = rig.tmp.dir / "calls.log";
  const EnvGuard net("SAWBLADE_NO_NETWORK", fail ? "0" : "1"), mode("FAKE_T3K_MODE", "error"), lg("FAKE_T3K_LOG", log.string());
  rig.board().setModelsExecutable([] { return std::string(SAWBLADE_FAKE_T3K); });
  writeTone(rig.cache, "777", "pedal", "Offline Pedal", {{"7771", "Gain 1", "linear_identity.nam"}, {"7772", "Gain 2", "linear_identity.nam"}});
  rig.load(rigJson({capBlock("a1", rig.cache, "777", "7771", "Offline Pedal"), namAmp("a2")}, {}, false));
  pump(1500);  // a reply (or none) has time to land
  for (int i = 0; i < 3; ++i) {
    rig.ed->refreshNow();
    pump(100);
  }
  CHECK(callsOf(log, "models") == (fail ? 1 : 0));  // asked once and never again after a failure; not at all with network tools off
  REQUIRE(rig.board().tile(0, 0)->settings().size() == 2);  // the cache
  CHECK(rig.board().tile(0, 0)->hasSelector());
}
