// v0.4 Task C (docs/specs/v0_4-pedals.md): the pedalboard. Mouse-driven: every gesture is a synthesized mouse event on the real tile,
// translated into the board's coordinates. Covers reorder within a path, moves A -> B and B -> A, adding from the picker, removing by
// drag-off and by the menu, bypass, one undo step per edit (undo restores the exact preset, redo re-applies it), a refresh during a drag,
// full paths, 6+ pedals per path (>= 110 px, scrolling), the render and the latency report after a move. Needs a display (xvfb-run).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "latency_stub.h"
#include "pedals/PedalFace.h"
#include "rig/Pedalboard.h"
#include "rig/PedalPicker.h"
#include "rig/RigModel.h"
#include "skin/RigView.h"

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

juce::MouseEvent ev(juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, int mods = juce::ModifierKeys::leftButtonModifier) {
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(mods), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, downPos,
                          now, 1, true);
}
void click(juce::Component& c) {
  const auto centre = c.getLocalBounds().toFloat().getCentre();
  c.mouseDown(ev(c, centre, centre));
  c.mouseUp(ev(c, centre, centre));
}
void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

struct TempDir {
  fs::path dir;
  TempDir() {
    dir = fs::temp_directory_path() / ("sawblade_pedalboard_tests_" + std::to_string(juce::Random::getSystemRandom().nextInt64() & 0xffffffff));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

// --- presets (identity fixtures and modeled pedals) --------------------------------------------------------------------------------
json namAmp(const std::string& id) { return {{"id", id}, {"type", "nam"}, {"slot", "amp"}, {"model", {{"file", (kFx / "nam" / "linear_identity.nam").string()}}}}; }
json hmBlock(const std::string& id, bool bypass = false) {
  json b = {{"id", id}, {"type", "pedal.hm"}, {"slot", "pedal"}, {"modelVersion", 1}, {"params", {{"level", 6}, {"low", 6}, {"high", 5}, {"distortion", 7}}}};
  if (bypass) b["bypass"] = true;
  return b;
}
json tsBlock(const std::string& id, double drive = 2, bool bypass = false) {
  json b = {{"id", id}, {"type", "pedal.ts"}, {"slot", "boost"}, {"modelVersion", 1}, {"params", {{"drive", drive}, {"tone", 6}, {"level", 8}}}};
  if (bypass) b["bypass"] = true;
  return b;
}
json eqBlock(const std::string& id) {
  return {{"id", id}, {"type", "eq"}, {"slot", "fx"}, {"bands", json::array({{{"type", "peak"}, {"freq", 1000}, {"gainDb", 3}, {"q", 1}}})}};
}
json stubBlock(const std::string& id, int latency) { return {{"id", id}, {"type", "test.latency"}, {"slot", "pedal"}, {"latency", latency}}; }
json sharedCab() { return {{"mode", "shared"}, {"ir", {{"file", (kFx / "ir" / "impulse.wav").string()}}}}; }

json rigJson(const std::vector<json>& a, const std::vector<json>& b, bool bOn) {
  json ja = json::array(), jb = json::array();
  for (const json& blk : a) ja.push_back(blk);
  for (const json& blk : b) jb.push_back(blk);
  return {{"schema", "sawblade.preset"},
          {"version", 3},
          {"name", "pedalboard test"},
          {"paths", {{"a", {{"role", "saw"}, {"blocks", ja}}}, {"b", {{"role", "body"}, {"enabled", bOn}, {"blocks", jb}}}}},
          {"align", {{"mode", "off"}}},
          {"blend", bOn ? 0.5 : 0.0},
          {"cab", sharedCab()}};
}
// SAW: chainsaw a1, overdrive a2 (bypassed), EQ a3, amp a4.  BODY: overdrive b1, amp b2.
json standard() { return rigJson({hmBlock("a1"), tsBlock("a2", 2, true), eqBlock("a3"), namAmp("a4")}, {tsBlock("b1", 4), namAmp("b2")}, true); }

std::vector<std::string> ids(const PathPreset& p) {
  std::vector<std::string> v;
  for (const Block& b : p.blocks) v.push_back(b.id);
  return v;
}
using Ids = std::vector<std::string>;

struct Rig {
  SettingsEnv env{kSettingsExist, /*isolateHome=*/true};
  TempDir tmp;
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;
  int files = 0;

  Rig() {
    sawblade::test::registerLatencyStub();
    proc.prepareToPlay(48000.0, 512);
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  }
  ~Rig() { base.reset(); }

  void settle() {
    REQUIRE(proc.waitForLoader(kLoad));
    REQUIRE(proc.status().error.empty());
    pump(40);
    ed->refreshNow();
  }
  void load(const json& j) {
    const fs::path f = tmp.dir / ("p" + std::to_string(files++) + ".json");
    std::ofstream(f) << j.dump(2);
    REQUIRE(proc.loadPresetFile(f));
    settle();
    proc.historyClear();  // a test starts from an empty undo history, whatever its earlier steps were
  }
  rig::Pedalboard& board() { return ed->pedalboard(); }
  rig::RigController& ctl() { return ed->rigController(); }
  Preset preset() { return proc.currentPreset(); }

  // The point just left of tile `index`'s centre (dropping there puts the dragged pedal before it) / just right of the last tile.
  juce::Point<float> before(int path, int index) {
    const auto b = board().tileBounds(*board().tile(path, index));
    return {static_cast<float>(b.getX() + 4), static_cast<float>(b.getCentreY())};
  }
  juce::Point<float> afterLast(int path) {
    const auto b = board().tileBounds(*board().tile(path, board().tileCount(path) - 1));
    return {static_cast<float>(b.getRight() + 4), static_cast<float>(b.getCentreY())};
  }
};

// A mouse gesture on a tile's body: press in the middle, move to board point `to`, optionally release there.
struct Gesture {
  Rig& rig;
  rig::BoardTile* tile;
  juce::Point<float> down;
  explicit Gesture(Rig& r, rig::BoardTile& t) : rig(r), tile(&t), down(t.getLocalBounds().toFloat().getCentre()) { tile->mouseDown(ev(*tile, down, down)); }
  juce::Point<float> local(juce::Point<float> boardPoint) { return tile->getLocalPoint(&rig.board(), boardPoint); }
  void moveTo(juce::Point<float> boardPoint) {
    const auto to = local(boardPoint);
    tile->mouseDrag(ev(*tile, (down + to) * 0.5f, down));
    tile->mouseDrag(ev(*tile, to, down));
  }
  void release(juce::Point<float> boardPoint) { tile->mouseUp(ev(*tile, local(boardPoint), down)); }  // the tile may be gone after this
};

// One edit on the board = exactly one undo step: undo restores `before` exactly, redo re-applies `after`.
void checkOneUndoStep(Rig& rig, const Preset& before, const Preset& after) {
  REQUIRE(rig.ctl().canUndo());
  REQUIRE(rig.ctl().undo());
  rig.settle();
  CHECK(rig.preset() == before);
  CHECK_FALSE(rig.ctl().canUndo());  // exactly one step
  REQUIRE(rig.ctl().canRedo());
  REQUIRE(rig.ctl().redo());
  rig.settle();
  CHECK(rig.preset() == after);
  CHECK_FALSE(rig.ctl().canRedo());
}

bool labelContains(juce::Component& root, const juce::String& text) {
  for (auto* l : all<juce::Label>(root))
    if (l->isVisible() && l->getText().contains(text)) return true;
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("pedalboard: dragging a pedal reorders its path (one undo step; the ghost, target outline and insertion index while dragging)", "[editor][pedalboard]") {
  Rig rig;
  rig.load(standard());
  auto& pb = rig.board();
  REQUIRE(pb.tileCount(0) == 3);
  const Preset before = rig.preset();
  REQUIRE(ids(before.a) == Ids{"a1", "a2", "a3", "a4"});
  CHECK_FALSE(rig.ctl().canUndo());

  rig::BoardTile* first = pb.tile(0, 0);
  Gesture g(rig, *first);
  CHECK(pb.dragInfo().pressed);
  CHECK_FALSE(pb.dragInfo().active);
  g.moveTo(rig.afterLast(0));
  {
    const auto d = pb.dragInfo();
    CHECK(d.active);
    CHECK(d.tile == first);
    CHECK_FALSE(d.removing);
    CHECK_FALSE(d.refused);
    CHECK(d.targetPath == 0);
    CHECK(d.insertIndex == 2);  // the final index among the SAW pedals
    CHECK(first->getAlpha() < 1.0f);  // the dragged tile is dimmed where it was
  }
  g.release(rig.afterLast(0));
  rig.settle();
  CHECK_FALSE(pb.dragInfo().pressed);
  const Preset after = rig.preset();
  CHECK(ids(after.a) == Ids{"a2", "a3", "a1", "a4"});  // the amp stays last
  CHECK(ids(after.b) == ids(before.b));
  REQUIRE(pb.tileCount(0) == 3);
  CHECK(pb.tile(0, 0)->blockId() == "a2");
  CHECK(pb.tile(0, 2)->blockId() == "a1");
  CHECK(after.a.blocks[0].bypass);  // every block keeps its own state
  checkOneUndoStep(rig, before, after);

  // and back to the front, onto the first tile's left half
  Gesture back(rig, *pb.tile(0, 2));
  back.moveTo(rig.before(0, 0));
  CHECK(pb.dragInfo().insertIndex == 0);
  back.release(rig.before(0, 0));
  rig.settle();
  CHECK(ids(rig.preset().a) == Ids{"a1", "a2", "a3", "a4"});
}

TEST_CASE("pedalboard: a drag shorter than 6 px is a click: it selects and edits nothing", "[editor][pedalboard]") {
  Rig rig;
  rig.load(standard());
  auto& pb = rig.board();
  const Preset before = rig.preset();
  rig::BoardTile* t = pb.tile(0, 1);
  Gesture g(rig, *t);
  CHECK(rig.ed->selectedBlockId() == "a2");  // the press selected it
  const auto c = t->getLocalBounds().toFloat().getCentre();
  t->mouseDrag(ev(*t, c + juce::Point<float>(3.0f, 0.0f), c));
  CHECK_FALSE(pb.dragInfo().active);
  t->mouseDrag(ev(*t, c + juce::Point<float>(5.0f, 0.0f), c));
  CHECK_FALSE(pb.dragInfo().active);
  t->mouseUp(ev(*t, c + juce::Point<float>(5.0f, 0.0f), c));
  rig.settle();
  CHECK(rig.preset() == before);
  CHECK_FALSE(rig.ctl().canUndo());

  Gesture g2(rig, *pb.tile(0, 1));
  const auto c2 = pb.tile(0, 1)->getLocalBounds().toFloat().getCentre();
  pb.tile(0, 1)->mouseDrag(ev(*pb.tile(0, 1), c2 + juce::Point<float>(7.0f, 0.0f), c2));
  CHECK(pb.dragInfo().active);  // past 6 px
  // dropped where it was: nothing changes
  g2.release(rig.before(0, 1));
  rig.settle();
  CHECK(rig.preset() == before);
  CHECK_FALSE(rig.ctl().canUndo());
  (void)g;
}

TEST_CASE("pedalboard: a drag from SAW to BODY moves the pedal (fresh id, everything else kept, one undo step)", "[editor][pedalboard]") {
  Rig rig;
  rig.load(standard());
  auto& pb = rig.board();
  const Preset before = rig.preset();

  Gesture g(rig, *pb.tile(0, 1));  // a2, the bypassed overdrive
  g.moveTo(rig.before(1, 0));
  CHECK(pb.dragInfo().targetPath == 1);
  CHECK(pb.dragInfo().insertIndex == 0);
  g.release(rig.before(1, 0));
  rig.settle();
  const Preset after = rig.preset();
  CHECK(ids(after.a) == Ids{"a1", "a3", "a4"});
  REQUIRE(after.b.blocks.size() == 3);
  CHECK(after.b.blocks[0].id == "b3");  // newBlockId
  CHECK(ids(after.b) == Ids{"b3", "b1", "b2"});
  const Block& moved = after.b.blocks[0];
  const Block& orig = before.a.blocks[1];
  CHECK(moved.type == orig.type);
  CHECK(moved.slot == orig.slot);
  CHECK(moved.bypass == orig.bypass);  // the bypass travels with it
  REQUIRE(moved.params != nullptr);
  CHECK(moved.params->equals(*orig.params));
  CHECK(pb.tileCount(0) == 2);
  CHECK(pb.tileCount(1) == 2);
  CHECK(pb.tile(1, 0)->blockId() == "b3");
  CHECK(pb.tile(1, 0)->bypassed());
  checkOneUndoStep(rig, before, after);
}

TEST_CASE("pedalboard: a drag from BODY to SAW goes in before the SAW amp (one undo step)", "[editor][pedalboard]") {
  Rig rig;
  rig.load(standard());
  auto& pb = rig.board();
  const Preset before = rig.preset();
  Gesture g(rig, *pb.tile(1, 0));  // b1
  g.moveTo(rig.afterLast(0));
  CHECK(pb.dragInfo().targetPath == 0);
  CHECK(pb.dragInfo().insertIndex == 3);
  g.release(rig.afterLast(0));
  rig.settle();
  const Preset after = rig.preset();
  CHECK(ids(after.a) == Ids{"a1", "a2", "a3", "a5", "a4"});  // inserted before the amp
  CHECK(ids(after.b) == Ids{"b2"});
  CHECK(after.a.blocks[3].type == "pedal.ts");
  CHECK(after.a.blocks[3].params->equals(*before.b.blocks[0].params));
  CHECK(pb.tileCount(0) == 4);
  CHECK(pb.tileCount(1) == 0);
  CHECK_FALSE(pb.pathOff(1));  // path B is still on, just without pedals
  checkOneUndoStep(rig, before, after);
}

TEST_CASE("pedalboard: + PEDAL opens the picker; MODELED lists the five generic pedals; picking adds one before the amp (one undo step)", "[editor][pedalboard][picker]") {
  Rig rig;
  rig.load(standard());
  auto& pb = rig.board();
  CHECK_FALSE(pb.pickerOpen());
  CHECK(pb.addButton(0).isEnabled());
  click(pb.addButton(0));
  REQUIRE(pb.pickerOpen());
  rig::PedalPicker& picker = *pb.picker();
  CHECK(picker.tabCount() == 1);
  CHECK(picker.tabButton(0).getButtonText() == "MODELED");
  CHECK(picker.path() == 0);
  const auto& models = rig::PedalPicker::modeledModels();
  REQUIRE(models.size() == 5);
  const char* names[] = {"CHAINSAW", "MODDED SAW", "ONE-KNOB SAW", "BIG FUZZ", "GREEN OVERDRIVE"};
  const char* types[] = {"pedal.hm", "pedal.hmx", "pedal.eye", "pedal.muff", "pedal.ts"};
  for (int i = 0; i < 5; ++i) {
    CHECK(picker.modelButton(i).getButtonText() == names[i]);
    CHECK(models[static_cast<std::size_t>(i)].type == types[i]);
    CHECK(picker.modelButton(i).getTitle().isNotEmpty());
    CHECK(picker.modelButton(i).getTooltip().isNotEmpty());
    for (const char* bad : {"boss", "hm-2", "swollen", "pickle", "muff", "wrath", "torcher", "eyemaster", "tc electronic", "dunwich", "abominable"}) {
      INFO(models[static_cast<std::size_t>(i)].blurb << " / " << bad);
      CHECK_FALSE(models[static_cast<std::size_t>(i)].blurb.containsIgnoreCase(bad));
      CHECK_FALSE(picker.modelButton(i).getTooltip().containsIgnoreCase(bad));
    }
  }
  // Escape closes it
  CHECK(picker.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
  CHECK_FALSE(pb.pickerOpen());

  // every model can be added, on either board; the new tile carries the model's name; the amp stays last
  for (int path = 0; path < 2; ++path)
    for (int i = 0; i < 5; ++i) {
      INFO("path " << path << " model " << names[i]);
      rig.load(standard());
      const Preset before = rig.preset();
      click(pb.addButton(path));
      REQUIRE(pb.pickerOpen());
      CHECK(pb.picker()->path() == path);
      click(pb.picker()->modelButton(i));
      CHECK_FALSE(pb.pickerOpen());  // picking closes it
      rig.settle();
      const Preset after = rig.preset();
      const PathPreset& pp = path == 0 ? after.a : after.b;
      const PathPreset& pb0 = path == 0 ? before.a : before.b;
      REQUIRE(pp.blocks.size() == pb0.blocks.size() + 1);
      const std::size_t at = pp.blocks.size() - 2;  // just before the amp
      CHECK(pp.blocks[at].type == types[i]);
      CHECK(pp.blocks.back().id == pb0.blocks.back().id);
      CHECK(pb.tileCount(path) == (path == 0 ? 4 : 2));
      CHECK(pb.tile(path, pb.tileCount(path) - 1)->name() == names[i]);
      CHECK(pb.tile(path, pb.tileCount(path) - 1)->blockId() == pp.blocks[at].id);
      // unique id
      std::vector<std::string> all2 = ids(after.a);
      for (const auto& id : ids(after.b)) all2.push_back(id);
      std::sort(all2.begin(), all2.end());
      CHECK(std::adjacent_find(all2.begin(), all2.end()) == all2.end());
      checkOneUndoStep(rig, before, after);
    }

  // a path that is off has no picker
  rig.load(rigJson({hmBlock("a1"), namAmp("a2")}, {tsBlock("b1")}, false));
  pb.showPicker(1);
  CHECK_FALSE(pb.pickerOpen());
  CHECK_FALSE(pb.addButton(1).isVisible());
}

TEST_CASE("pedalboard: dropping outside both boards removes the pedal (the ghost says REMOVE); the menu has BYPASS and REMOVE", "[editor][pedalboard]") {
  Rig rig;
  rig.load(standard());
  auto& pb = rig.board();
  const Preset before = rig.preset();

  // drag off: onto the heads
  Gesture g(rig, *pb.tile(0, 2));  // a3, the EQ
  const juce::Point<float> off(470.0f, 100.0f);
  g.moveTo(off);
  CHECK(pb.dragInfo().removing);
  CHECK(pb.dragInfo().targetPath == -1);
  g.release(off);
  rig.settle();
  const Preset after = rig.preset();
  CHECK(ids(after.a) == Ids{"a1", "a2", "a4"});
  CHECK(pb.tileCount(0) == 2);
  checkOneUndoStep(rig, before, after);
  rig.proc.historyClear();

  // the menu
  const Preset s1 = rig.preset();  // SAW: a1, a2, a4
  {
    const juce::PopupMenu m = pb.menuFor(*pb.tile(0, 0));
    std::vector<juce::String> texts;
    std::vector<int> itemIds;
    bool ticked = false;
    juce::PopupMenu::MenuItemIterator it(m);
    while (it.next()) {
      texts.push_back(it.getItem().text);
      itemIds.push_back(it.getItem().itemID);
      if (it.getItem().text == "BYPASS") ticked = it.getItem().isTicked;
    }
    CHECK(texts == std::vector<juce::String>{"BYPASS", "REMOVE"});
    CHECK(itemIds == std::vector<int>{rig::Pedalboard::kMenuBypass, rig::Pedalboard::kMenuRemove});
    CHECK_FALSE(ticked);
  }
  pb.applyMenuChoice(0, "a1", rig::Pedalboard::kMenuBypass);
  rig.settle();
  CHECK(rig.preset().a.blocks[0].bypass);
  CHECK(pb.tile(0, 0)->bypassed());
  {
    juce::PopupMenu::MenuItemIterator it(pb.menuFor(*pb.tile(0, 0)));
    while (it.next())
      if (it.getItem().text == "BYPASS") CHECK(it.getItem().isTicked);  // ticked while bypassed
  }
  REQUIRE(rig.ctl().undo());
  rig.settle();
  CHECK(rig.preset() == s1);
  CHECK_FALSE(rig.ctl().canUndo());

  pb.applyMenuChoice(0, "a1", rig::Pedalboard::kMenuRemove);
  rig.settle();
  const Preset s2 = rig.preset();
  CHECK(ids(s2.a) == Ids{"a2", "a4"});
  CHECK(pb.tileCount(0) == 1);
  checkOneUndoStep(rig, s1, s2);

  // a right-click (or ctrl-click) opens the menu: it neither selects nor starts a drag
  rig::BoardTile* rt = pb.tile(0, 0);
  int menus = 0;
  rt->onContextMenu = [&menus](rig::BoardTile&) { ++menus; };  // (the real handler shows a native popup, which a headless display cannot)
  const auto c = rt->getLocalBounds().toFloat().getCentre();
  const std::string selectedBefore = rig.ed->selectedBlockId();
  rt->mouseDown(ev(*rt, c, c, juce::ModifierKeys::rightButtonModifier));
  rt->mouseDrag(ev(*rt, c + juce::Point<float>(40.0f, 0.0f), c, juce::ModifierKeys::rightButtonModifier));
  rt->mouseUp(ev(*rt, c + juce::Point<float>(40.0f, 0.0f), c, juce::ModifierKeys::rightButtonModifier));
  CHECK(menus == 1);
  CHECK_FALSE(pb.dragInfo().pressed);
  CHECK(rig.ed->selectedBlockId() == selectedBefore);
}

TEST_CASE("pedalboard: the footswitch bypasses (one undo step); the LED follows", "[editor][pedalboard]") {
  Rig rig;
  rig.load(standard());
  auto& pb = rig.board();
  const Preset before = rig.preset();
  rig::BoardTile* t = pb.tile(0, 0);
  CHECK(t->led().isOn());
  click(t->footswitch());
  rig.settle();
  CHECK(pb.tile(0, 0) == t);  // not rebuilt
  CHECK(t->bypassed());
  CHECK_FALSE(t->led().isOn());
  const Preset after = rig.preset();
  CHECK(after.a.blocks[0].bypass);
  CHECK(ids(after.a) == ids(before.a));
  checkOneUndoStep(rig, before, after);
  CHECK_FALSE(pb.tile(0, 0)->bypassed());  // the board follows the undo

  // a footswitch press is not a drag
  click(pb.tile(0, 0)->footswitch());
  CHECK_FALSE(pb.dragInfo().pressed);
  rig.settle();
}

TEST_CASE("pedalboard: a refresh during a drag keeps the dragged tile", "[editor][pedalboard]") {
  Rig rig;
  rig.load(standard());
  auto& pb = rig.board();
  rig::BoardTile* t = pb.tile(0, 0);
  Gesture g(rig, *t);
  g.moveTo(rig.afterLast(0));
  REQUIRE(pb.dragInfo().active);
  rig.ed->refreshNow();
  rig.ed->refreshNow();
  CHECK(pb.tile(0, 0) == t);
  CHECK(pb.dragInfo().tile == t);

  // even when the rig changed underneath (a preset load, the host): the widget under the hand is not destroyed
  const Preset other = [&] {
    Preset p = rig.preset();
    p.a.blocks.erase(p.a.blocks.begin() + 2);  // the EQ
    return p;
  }();
  rig.proc.loadPreset(other);
  REQUIRE(rig.proc.waitForLoader(kLoad));
  pump(30);
  rig.ed->refreshNow();
  CHECK(pb.tile(0, 0) == t);
  CHECK(pb.tileCount(0) == 3);  // still the old board: the rebuild waits for the mouse-up
  CHECK(pb.dragInfo().active);

  // dropped where it was: nothing to do, and the board then follows the rig
  g.release(rig.before(0, 0));
  rig.settle();
  CHECK(pb.tileCount(0) == 2);
  CHECK(ids(rig.preset().a) == Ids{"a1", "a2", "a4"});
  CHECK_FALSE(pb.dragInfo().pressed);
}

TEST_CASE("pedalboard: a full path greys + PEDAL and refuses drops from the other path with a status message", "[editor][pedalboard]") {
  Rig rig;
  // SAW: 7 pedals and the amp = 8 blocks
  std::vector<json> a;
  for (int i = 1; i <= 7; ++i) a.push_back(tsBlock("a" + std::to_string(i), i));
  a.push_back(namAmp("a8"));
  rig.load(rigJson(a, {hmBlock("b1"), namAmp("b2")}, true));
  auto& pb = rig.board();
  REQUIRE(pb.tileCount(0) == 7);
  CHECK(pb.pathFull(0));
  CHECK_FALSE(pb.pathFull(1));
  CHECK_FALSE(pb.addButton(0).isEnabled());  // greyed
  CHECK(pb.addButton(1).isEnabled());
  click(pb.addButton(0));
  CHECK_FALSE(pb.pickerOpen());
  CHECK_FALSE(pb.addModeledPedal(0, "pedal.hm"));

  const Preset before = rig.preset();
  Gesture g(rig, *pb.tile(1, 0));
  g.moveTo(rig.before(0, 3));
  CHECK(pb.dragInfo().refused);
  CHECK(pb.dragInfo().targetPath == 0);
  CHECK_FALSE(pb.dragInfo().removing);
  g.release(rig.before(0, 3));
  rig.settle();
  CHECK(rig.preset() == before);  // nothing moved
  CHECK_FALSE(rig.ctl().canUndo());
  CHECK(labelContains(*rig.ed, "path full: 8 blocks"));  // the status line says why

  // reordering inside the full path is fine (the board scrolls: bring its end into view first)
  pb.viewport(0).setViewPosition(pb.viewport(0).getViewedComponent()->getWidth(), 0);
  Gesture r(rig, *pb.tile(0, 0));
  r.moveTo(rig.afterLast(0));
  CHECK_FALSE(pb.dragInfo().refused);
  CHECK(pb.dragInfo().insertIndex == 6);
  r.release(rig.afterLast(0));
  rig.settle();
  CHECK(ids(rig.preset().a).back() == "a8");
  CHECK(rig.preset().a.blocks[6].id == "a1");

  // removing one frees the + PEDAL slot again
  pb.removePedal(0, "a1");
  rig.settle();
  CHECK(pb.addButton(0).isEnabled());
}

TEST_CASE("pedalboard: BODY off is not a drop target", "[editor][pedalboard]") {
  Rig rig;
  rig.load(rigJson({hmBlock("a1"), tsBlock("a2"), namAmp("a3")}, {tsBlock("b1"), namAmp("b2")}, false));
  auto& pb = rig.board();
  REQUIRE(pb.pathOff(1));
  CHECK_FALSE(pb.viewport(1).isVisible());
  const Preset before = rig.preset();
  Gesture g(rig, *pb.tile(0, 0));
  const auto inB = rig::Pedalboard::boardBounds(1).getCentre().toFloat();
  g.moveTo(inB);
  const auto d = pb.dragInfo();
  CHECK(d.active);
  CHECK(d.targetPath == -1);
  CHECK_FALSE(d.removing);  // over the off board: not a remove either, a cancel
  CHECK_FALSE(d.refused);
  g.release(inB);
  rig.settle();
  CHECK(rig.preset() == before);
  CHECK_FALSE(rig.ctl().canUndo());
  CHECK_FALSE(pb.movePedal(0, "a1", 1, 0));
  CHECK_FALSE(pb.addModeledPedal(1, "pedal.hm"));
}

TEST_CASE("pedalboard: 6 pedals per path stay at least 110 px wide and the board scrolls; the live face follows its tile", "[editor][pedalboard]") {
  Rig rig;
  std::vector<json> a = {hmBlock("a1")};
  for (int i = 2; i <= 6; ++i) a.push_back(tsBlock("a" + std::to_string(i), i));
  a.push_back(namAmp("a7"));
  std::vector<json> b;
  for (int i = 1; i <= 6; ++i) b.push_back(i == 1 ? hmBlock("b1") : tsBlock("b" + std::to_string(i), i));
  b.push_back(namAmp("b7"));
  rig.load(rigJson(a, b, true));
  auto& pb = rig.board();
  for (int path = 0; path < 2; ++path) {
    INFO("path " << path);
    REQUIRE(pb.tileCount(path) == 6);
    for (int i = 0; i < 6; ++i) {
      CHECK(pb.tile(path, i)->getWidth() >= 110);
      CHECK(pb.tile(path, i)->getWidth() <= 180);
    }
    juce::Viewport& vp = pb.viewport(path);
    REQUIRE(vp.getViewedComponent() != nullptr);
    CHECK(vp.getViewedComponent()->getWidth() > vp.getWidth());  // more than fits: it scrolls
    CHECK(vp.getViewPositionX() == 0);
    const auto first0 = pb.tileBounds(*pb.tile(path, 0));
    vp.setViewPosition(120, 0);
    CHECK(vp.getViewPositionX() == 120);
    CHECK(pb.tileBounds(*pb.tile(path, 0)).getX() == first0.getX() - 120);
    vp.setViewPosition(0, 0);
  }
  // the board's tiles stay inside their board's frame
  CHECK(pb.viewport(0).getBounds().getRight() <= rig::Pedalboard::boardBounds(0).getRight());

  // the face lies over the first circuit tile only while that tile is fully in view
  auto faces = all<PedalFace>(*rig.ed);
  REQUIRE(faces.size() == 1);
  PedalFace& face = *faces[0];
  rig.ed->refreshNow();
  REQUIRE(face.isVisible());
  CHECK(face.getBounds() == pb.tileBounds(*pb.tile(0, 0)) + pb.getPosition());
  pb.viewport(0).setViewPosition(400, 0);  // the chainsaw tile scrolls out of view
  CHECK_FALSE(face.isVisible());
  pb.viewport(0).setViewPosition(0, 0);
  CHECK(face.isVisible());

  // dragging works on a scrolled board: move the last tile to the front
  pb.viewport(0).setViewPosition(0, 0);
  pb.viewport(0).setViewPosition(pb.viewport(0).getViewedComponent()->getWidth(), 0);  // fully right
  const int viewX = pb.viewport(0).getViewPositionX();
  REQUIRE(viewX > 0);
  const Preset before = rig.preset();
  Gesture g(rig, *pb.tile(0, 5));
  const auto firstVisible = pb.tileBounds(*pb.tile(0, 4));  // in view at the far right
  const juce::Point<float> to(static_cast<float>(firstVisible.getX() + 4), static_cast<float>(firstVisible.getCentreY()));
  g.moveTo(to);
  CHECK(pb.dragInfo().active);
  CHECK(pb.dragInfo().targetPath == 0);
  g.release(to);
  rig.settle();
  CHECK(rig.preset() != before);
  CHECK(rig.preset().a.blocks.size() == before.a.blocks.size());
}

// ---------------------------------------------------------------------------------------------------------------------------------
namespace {
std::vector<float> renderFresh(SawbladeProcessor& p) {
  p.setRateAndBufferSizeDetails(48000.0, 512);
  p.prepareToPlay(48000.0, 512);
  REQUIRE(p.waitForLoader(kLoad));
  juce::AudioBuffer<float> buf(2, 512);
  juce::MidiBuffer midi;
  std::vector<float> out;
  std::uint32_t seed = 12345;
  for (int block = 0; block < 12; ++block) {
    buf.clear();
    float* ch0 = buf.getWritePointer(0);
    for (int i = 0; i < 512; ++i) {
      seed = seed * 1664525u + 1013904223u;
      const float noise = static_cast<float>(seed >> 8) / 16777216.0f - 0.5f;
      const double t = static_cast<double>(block * 512 + i) / 48000.0;
      ch0[i] = 0.25f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 196.0 * t)) + 0.1f * noise;
    }
    p.processBlock(buf, midi);
    out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 512);
    out.insert(out.end(), buf.getReadPointer(1), buf.getReadPointer(1) + 512);
  }
  return out;
}
bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}
double rmsOf(const std::vector<float>& v) {
  double s = 0.0;
  for (float x : v) s += static_cast<double>(x) * static_cast<double>(x);
  return std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, v.size())));
}
}  // namespace

TEST_CASE("pedalboard: the rendered chain after a drag equals a preset built with that order directly", "[editor][pedalboard][render]") {
  // within SAW: [chainsaw, overdrive, overdrive(4), amp] -> [overdrive(4), chainsaw, overdrive, amp]
  const auto render = [](const json& start, const std::function<void(Rig&)>& drag, const json& direct) {
    std::vector<float> viaBoard, built;
    {
      Rig rig;
      rig.load(start);
      drag(rig);
      REQUIRE(rig.proc.waitForLoader(kLoad));
      viaBoard = renderFresh(rig.proc);
    }
    {
      Rig other;
      other.load(direct);
      built = renderFresh(other.proc);
    }
    CHECK(rmsOf(viaBoard) > 1e-4);
    CHECK(sameBits(viaBoard, built));
    return viaBoard;
  };
  const json start = rigJson({hmBlock("a1"), tsBlock("a2", 2), tsBlock("a3", 4), namAmp("a4")}, {tsBlock("b1", 6), namAmp("b2")}, true);
  const auto reorder = render(
      start,
      [](Rig& r) {
        auto& pb = r.board();
        Gesture g(r, *pb.tile(0, 2));
        g.moveTo(r.before(0, 0));
        g.release(r.before(0, 0));
      },
      rigJson({tsBlock("a3", 4), hmBlock("a1"), tsBlock("a2", 2), namAmp("a4")}, {tsBlock("b1", 6), namAmp("b2")}, true));
  // A -> B: the chainsaw leaves SAW and goes in after BODY's overdrive
  const auto across = render(
      start,
      [](Rig& r) {
        auto& pb = r.board();
        Gesture g(r, *pb.tile(0, 0));
        g.moveTo(r.afterLast(1));
        g.release(r.afterLast(1));
      },
      rigJson({tsBlock("a2", 2), tsBlock("a3", 4), namAmp("a4")}, {tsBlock("b1", 6), hmBlock("b3"), namAmp("b2")}, true));
  CHECK_FALSE(sameBits(reorder, across));  // the two moves really sound different
}

TEST_CASE("pedalboard: the reported latency after a move equals the core's compensated latency of the new preset", "[editor][pedalboard][latency]") {
  // SAW: a 100-sample block and the amp; BODY: a 40-sample block and the amp. The chain reports the longer path.
  const json start = rigJson({stubBlock("a1", 100), namAmp("a2")}, {stubBlock("b1", 40), namAmp("b2")}, true);
  Rig rig;
  rig.load(start);
  auto& pb = rig.board();
  REQUIRE(pb.tileCount(0) == 1);
  CHECK(rig.proc.status().latencySamples == 100);
  CHECK(rig.proc.getLatencySamples() == 100);

  // move SAW's 100 onto BODY (after its 40): BODY is now the long path, 140
  Gesture g(rig, *pb.tile(0, 0));
  g.moveTo(rig.afterLast(1));
  g.release(rig.afterLast(1));
  rig.settle();
  const int moved = rig.proc.status().latencySamples;
  CHECK(moved == 140);
  CHECK(rig.proc.getLatencySamples() == moved);  // what the host is told
  // the same preset loaded directly reports the same
  {
    Rig other;
    other.load(rigJson({namAmp("a2")}, {stubBlock("b1", 40), stubBlock("b3", 100), namAmp("b2")}, true));
    CHECK(other.proc.status().latencySamples == moved);
  }
  // undo: back to 100, and the report follows
  REQUIRE(rig.ctl().undo());
  rig.settle();
  CHECK(rig.proc.status().latencySamples == 100);
  CHECK(rig.proc.getLatencySamples() == 100);
}

TEST_CASE("pedalboard: Cmd / Ctrl + Z does not undo under an open picker", "[editor][pedalboard][picker]") {
  Rig rig;
  rig.load(standard());
  auto& pb = rig.board();
  click(pb.tile(0, 0)->footswitch());
  rig.settle();
  REQUIRE(rig.ctl().canUndo());
  const juce::KeyPress undoKey('z', juce::ModifierKeys::commandModifier, 0);
  click(pb.addButton(0));
  REQUIRE(pb.pickerOpen());
  CHECK_FALSE(rig.ed->keyPressed(undoKey));
  CHECK(rig.preset().a.blocks[0].bypass);
  rig.ed->closeAllOverlaysForTests();
  CHECK_FALSE(pb.pickerOpen());
  CHECK(rig.ed->keyPressed(undoKey));
  rig.settle();
  CHECK_FALSE(rig.preset().a.blocks[0].bypass);
  // opening another overlay closes the picker
  click(pb.addButton(1));
  REQUIRE(pb.pickerOpen());
  rig.ed->setCabPageOpen(true);
  CHECK_FALSE(pb.pickerOpen());
}
