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
  CHECK(picker.tabCount() == 2);  // MODELED and (Task B) CAPTURES
  CHECK(picker.tabButton(0).getButtonText() == "MODELED");
  CHECK(picker.tabButton(1).getButtonText() == "CAPTURES");
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
    const juce::PopupMenu m = pb.menuFor(*pb.tile(0, 0));  // the iterator keeps a pointer to its menu: not a temporary
    juce::PopupMenu::MenuItemIterator it(m);
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
  checkOneUndoStep(rig, before, after);  // ends redone: the pedal is bypassed again
  CHECK(pb.tile(0, 0)->bypassed());
  CHECK_FALSE(pb.tile(0, 0)->led().isOn());
  // the board follows the undo and the redo, one by one
  REQUIRE(rig.ctl().undo());
  rig.settle();
  CHECK_FALSE(pb.tile(0, 0)->bypassed());
  CHECK(pb.tile(0, 0)->led().isOn());
  REQUIRE(rig.ctl().redo());
  rig.settle();
  CHECK(pb.tile(0, 0)->bypassed());
  CHECK_FALSE(pb.tile(0, 0)->led().isOn());

  // a footswitch press is not a drag
  click(pb.tile(0, 0)->footswitch());
  CHECK_FALSE(pb.dragInfo().pressed);
  rig.settle();
}

TEST_CASE("pedalboard: a refresh during a drag keeps the dragged tile", "[editor][pedalboard]") {
  Rig rig;
  rig.load(standard());
  auto& pb = rig.board();
  pb.setMouseDownProbe([] { return true; });  // the synthesized mouse is "down" for the whole drag
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

  // dropped on its own slot: nothing to do, and the board then follows the rig. The drop is resolved by block id against the rig as it
  // is now (not by the stale index "end of the board", which would have moved a1 behind a2).
  g.moveTo(rig.before(0, 0));
  g.release(rig.before(0, 0));
  rig.settle();
  CHECK(pb.tileCount(0) == 2);
  CHECK(ids(rig.preset().a) == Ids{"a1", "a2", "a4"});
  CHECK_FALSE(rig.ctl().canUndo());
  CHECK_FALSE(pb.dragInfo().pressed);
}

TEST_CASE("pedalboard: a drop is resolved by block id when the rig changed during the drag", "[editor][pedalboard]") {
  // SAW: a1 chainsaw, a2 overdrive (bypassed), a3 EQ, a4 amp.
  const auto changeMidDrag = [](Rig& rig, auto&& edit) {
    Preset p = rig.preset();
    edit(p);
    rig.proc.loadPreset(p);
    REQUIRE(rig.proc.waitForLoader(kLoad));
    pump(30);
    rig.ed->refreshNow();
  };
  SECTION("a real move still lands by id") {
    Rig rig;
    rig.load(standard());
    auto& pb = rig.board();
    pb.setMouseDownProbe([] { return true; });
    Gesture g(rig, *pb.tile(0, 0));
    g.moveTo(rig.before(0, 2));  // between a2 and the EQ a3
    REQUIRE(pb.dragInfo().active);
    changeMidDrag(rig, [](Preset& p) { p.a.blocks.erase(p.a.blocks.begin() + 1); });  // a2 is removed: a1 a3 a4
    g.moveTo(rig.before(0, 2));  // the stale board is still shown: before the EQ
    g.release(rig.before(0, 2));
    rig.settle();
    CHECK(ids(rig.preset().a) == Ids{"a1", "a3", "a4"});  // before a3 is where a1 already is: nothing moved
    CHECK_FALSE(rig.ctl().canUndo());

    // and a move that does change the order is resolved against the new preset, not the old indices
    rig.load(standard());
    Gesture h(rig, *pb.tile(0, 0));
    h.moveTo(rig.afterLast(0));  // after the EQ a3
    REQUIRE(pb.dragInfo().active);
    changeMidDrag(rig, [](Preset& p) { p.a.blocks.erase(p.a.blocks.begin() + 1); });  // a2 removed: a1 a3 a4
    h.release(rig.afterLast(0));
    rig.settle();
    CHECK(ids(rig.preset().a) == Ids{"a3", "a1", "a4"});
    CHECK(rig.ctl().canUndo());
  }
  SECTION("the target's neighbours are gone: the drop is cancelled, no undo step") {
    Rig rig;
    rig.load(standard());
    auto& pb = rig.board();
    pb.setMouseDownProbe([] { return true; });
    Gesture g(rig, *pb.tile(0, 0));
    g.moveTo(rig.afterLast(0));  // after the EQ a3, the last tile
    REQUIRE(pb.dragInfo().active);
    changeMidDrag(rig, [](Preset& p) { p.a.blocks.erase(p.a.blocks.begin() + 2); });  // the EQ is removed
    g.release(rig.afterLast(0));
    rig.settle();
    CHECK(ids(rig.preset().a) == Ids{"a1", "a2", "a4"});
    CHECK_FALSE(rig.ctl().canUndo());
    CHECK(pb.tileCount(0) == 2);  // refreshed
  }
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

TEST_CASE("pedalboard: a drag whose mouse-up was lost is abandoned by the next refresh (ghost gone, tile back, the board follows the rig again)", "[editor][pedalboard]") {
  Rig rig;
  rig.load(standard());
  auto& pb = rig.board();
  bool down = true;
  pb.setMouseDownProbe([&down] { return down; });
  rig::BoardTile* t = pb.tile(0, 0);
  Gesture g(rig, *t);
  g.moveTo(rig.afterLast(0));
  REQUIRE(pb.dragInfo().active);
  CHECK(t->getAlpha() < 1.0f);
  const Preset before = rig.preset();

  // the rig changes under the drag, then the mouse capture is lost: no mouse-up ever arrives
  Preset other = before;
  other.a.blocks.erase(other.a.blocks.begin() + 2);  // the EQ
  rig.proc.loadPreset(other);
  REQUIRE(rig.proc.waitForLoader(kLoad));
  pump(30);
  rig.ed->refreshNow();
  CHECK(pb.dragInfo().active);  // the button is still down: the drag is kept (and the old board with it)
  CHECK(pb.tileCount(0) == 3);

  down = false;
  rig.ed->refreshNow();
  const auto d = pb.dragInfo();
  CHECK_FALSE(d.active);
  CHECK_FALSE(d.pressed);
  CHECK(d.tile == nullptr);
  CHECK(pb.tileCount(0) == 2);  // the board follows the rig again
  CHECK(pb.tile(0, 0)->getAlpha() == 1.0f);
  CHECK(ids(rig.preset().a) == Ids{"a1", "a2", "a4"});  // and nothing was dropped
  CHECK_FALSE(rig.ctl().canUndo());

  // a stale press (tile pressed, never released) does not leave a dimmed tile behind either: the next press starts clean
  down = true;
  Gesture first(rig, *pb.tile(0, 0));
  first.moveTo(rig.afterLast(0));
  REQUIRE(pb.dragInfo().active);
  rig::BoardTile* second = pb.tile(0, 1);
  Gesture again(rig, *second);  // a new press while the old drag never ended
  CHECK(pb.tile(0, 0)->getAlpha() == 1.0f);
  CHECK_FALSE(pb.dragInfo().active);
  CHECK(pb.dragInfo().tile == second);
  again.release(rig.before(0, 1));
  rig.settle();
  CHECK_FALSE(pb.dragInfo().pressed);
}

TEST_CASE("pedalboard: a path with a block after the amp keeps the amp and that block through reorder, cross-path insert and remove", "[editor][pedalboard]") {
  Rig rig;
  // SAW: chainsaw a1, overdrive a2, amp a3, EQ a4 (after the amp: not a tile).  BODY: overdrive b1, amp b2, EQ b3 (after its amp).
  rig.load(rigJson({hmBlock("a1"), tsBlock("a2", 2), namAmp("a3"), eqBlock("a4")}, {tsBlock("b1", 4), namAmp("b2"), eqBlock("b3")}, true));
  auto& pb = rig.board();
  REQUIRE(pb.tileCount(0) == 2);
  REQUIRE(pb.tileCount(1) == 1);
  CHECK(pb.afterAmpText(0) == "+1 AFTER AMP (rig editor)");
  CHECK(pb.afterAmpText(1) == "+1 AFTER AMP (rig editor)");
  const Preset before = rig.preset();
  const auto sameTail = [&](const Preset& p) {  // the amps and the blocks after them are exactly as before
    return p.a.blocks[p.a.blocks.size() - 2] == before.a.blocks[2] && p.a.blocks.back() == before.a.blocks[3] &&
           p.b.blocks[p.b.blocks.size() - 2] == before.b.blocks[1] && p.b.blocks.back() == before.b.blocks[2];
  };

  // reorder within SAW
  {
    Gesture g(rig, *pb.tile(0, 0));
    g.moveTo(rig.afterLast(0));
    g.release(rig.afterLast(0));
    rig.settle();
    const Preset p = rig.preset();
    CHECK(ids(p.a) == Ids{"a2", "a1", "a3", "a4"});
    CHECK(sameTail(p));
    CHECK(ids(p.b) == ids(before.b));
    CHECK(pb.afterAmpText(0) == "+1 AFTER AMP (rig editor)");
  }
  // SAW -> BODY: in before BODY's amp, the amp and its trailing EQ stay put
  {
    Gesture g(rig, *pb.tile(0, 0));  // a2
    g.moveTo(rig.afterLast(1));
    g.release(rig.afterLast(1));
    rig.settle();
    const Preset p = rig.preset();
    CHECK(ids(p.a) == Ids{"a1", "a3", "a4"});
    REQUIRE(p.b.blocks.size() == 4);
    CHECK(p.b.blocks[0].id == "b1");
    CHECK(p.b.blocks[2].id == "b2");  // BODY's amp
    CHECK(p.b.blocks[3].id == "b3");  // and its EQ after it
    CHECK(p.b.blocks[1].type == "pedal.ts");  // the moved pedal sits before the amp
    CHECK(p.a.blocks[1] == before.a.blocks[2]);
    CHECK(p.a.blocks[2] == before.a.blocks[3]);
  }
  // remove: a tile goes, the amp and the block after it stay
  {
    Gesture g(rig, *pb.tile(1, 0));
    const juce::Point<float> off(470.0f, 100.0f);
    g.moveTo(off);
    g.release(off);
    rig.settle();
    const Preset p = rig.preset();
    CHECK(p.b.blocks.size() == 3);
    CHECK(p.b.blocks[p.b.blocks.size() - 2] == before.b.blocks[1]);
    CHECK(p.b.blocks.back() == before.b.blocks[2]);
  }
}

TEST_CASE("pedalboard: dropping on the right half of a tile puts the pedal after it (a1 onto a2 -> a2, a1, a3)", "[editor][pedalboard]") {
  Rig rig;
  rig.load(rigJson({hmBlock("a1"), tsBlock("a2", 2), eqBlock("a3"), namAmp("a4")}, {tsBlock("b1"), namAmp("b2")}, true));
  auto& pb = rig.board();
  REQUIRE(pb.tileCount(0) == 3);
  const Preset before = rig.preset();
  const auto b2 = pb.tileBounds(*pb.tile(0, 1));
  const juce::Point<float> rightHalf(static_cast<float>(b2.getCentreX() + 6), static_cast<float>(b2.getCentreY()));
  Gesture g(rig, *pb.tile(0, 0));
  g.moveTo(rightHalf);
  CHECK(pb.dragInfo().insertIndex == 1);
  g.release(rightHalf);
  rig.settle();
  const Preset after = rig.preset();
  CHECK(ids(after.a) == Ids{"a2", "a1", "a3", "a4"});
  checkOneUndoStep(rig, before, after);

  // and the left half of the same tile puts it before: a3 onto the left half of a2 -> a1, a3, a2
  const auto c2 = pb.tileBounds(*pb.tile(0, 1));  // a1 now
  const juce::Point<float> leftHalf(static_cast<float>(c2.getX() + 6), static_cast<float>(c2.getCentreY()));
  Gesture h(rig, *pb.tile(0, 2));  // a3
  h.moveTo(leftHalf);
  CHECK(pb.dragInfo().insertIndex == 1);
  h.release(leftHalf);
  rig.settle();
  CHECK(ids(rig.preset().a) == Ids{"a2", "a3", "a1", "a4"});
}

// ---------------------------------------------------------------------------------------------------------------------------------
// v0.8 I2: the "UNCAL" mark of a capture whose metadata has no input / output level, and the main-view notice
TEST_CASE("pedalboard: with calibrated input levels on, a capture without level metadata carries the UNCAL mark; off, none does", "[editor][pedalboard][devicecal]") {
  Rig rig;
  const auto capture = [](const std::string& id, const char* file) {
    return json{{"id", id}, {"type", "nam"}, {"slot", "pedal"}, {"model", {{"file", (kFx / "nam" / file).string()}}}};
  };
  // a1 has no levels (the linear identity fixture); a2 has both (cal_pedal_a: in 6 dBu, out 10 dBu).
  const json rigJ = rigJson({capture("a1", "linear_identity.nam"), capture("a2", "cal_pedal_a.nam"), namAmp("a3")}, {namAmp("b1")}, true);
  rig.load(rigJ);  // calibration off
  auto& pb = rig.board();
  REQUIRE(pb.tileCount(0) == 2);
  CHECK_FALSE(pb.tile(0, 0)->uncalibrated());
  CHECK_FALSE(pb.tile(0, 1)->uncalibrated());
  CHECK_FALSE(rig.ed->ampHead(0).uncalibrated());
  CHECK_FALSE(labelContains(*rig.ed, "Interface not calibrated"));

  settings::Settings::shared().setCalibratedInputLevels(true);
  rig.proc.calibrationTick();  // what the 10 Hz timer does: rebuild with calibration
  rig.settle();
  CHECK(rig.proc.status().calibrationOn);
  CHECK(pb.tile(0, 0)->uncalibrated());
  CHECK_FALSE(pb.tile(0, 1)->uncalibrated());
  CHECK(rig.ed->ampHead(0).uncalibrated());  // the SAW amp (linear identity fixture) has no levels either
  // No device record: the non-blocking notice is in the main view.
  rig.ed->refreshNow();
  CHECK(labelContains(*rig.ed, "Interface not calibrated: assuming +12 dBu"));

  // A device record: the notice goes (the mark stays: it is about the capture's own metadata).
  settings::Settings::shared().setDeviceCalibration(settings::recordFromPreset(*settings::findDevicePreset("scarlett-4i4-3g"), "2026-10-08"));
  rig.proc.calibrationTick();
  rig.settle();
  rig.ed->refreshNow();
  CHECK_FALSE(labelContains(*rig.ed, "Interface not calibrated"));
  CHECK(pb.tile(0, 0)->uncalibrated());

  settings::Settings::shared().setCalibratedInputLevels(false);
  settings::Settings::shared().setDeviceCalibration(std::nullopt);
  rig.proc.calibrationTick();
  rig.settle();
  CHECK_FALSE(pb.tile(0, 0)->uncalibrated());
  CHECK_FALSE(rig.ed->ampHead(0).uncalibrated());
}

// v0.8 I2: swapping a pedal that feeds an amp, with calibrated input levels on, stores make-up 0 (the old one is wiped); adding one stores none.
TEST_CASE("pedalboard: with calibrated input levels on, a pedal that feeds the amp gets make-up 0 on a swap and none on an add", "[editor][pedalboard][devicecal][levelmatch]") {
  Rig rig;
  const auto capture = [](const std::string& id, const char* file, double makeup) {
    json b = {{"id", id}, {"type", "nam"}, {"slot", "pedal"}, {"model", {{"file", (kFx / "nam" / file).string()}}}};
    if (makeup != 0.0) b["makeupDb"] = makeup;
    return b;
  };
  settings::Settings::shared().setCalibratedInputLevels(true);
  rig.load(rigJson({capture("a1", "cal_pedal_a.nam", 5.0), namAmp("a2")}, {namAmp("b1")}, true));
  rig.proc.calibrationTick();
  rig.settle();
  const auto makeupOf = [&](int path, std::size_t i) {
    const Preset p = rig.preset();
    return static_cast<const NamBlockParams&>(*(path == 0 ? p.a : p.b).blocks[i].params).makeupDb;
  };
  REQUIRE(makeupOf(0, 0) == 5.0);
  Capture next;
  next.file = (kFx / "nam" / "linear_05_025.nam").string();
  next.resolvedPath = next.file;
  REQUIRE(rig.board().swapPedalCapture(0, "a1", next));
  const auto end = std::chrono::steady_clock::now() + kLoad;
  while (makeupOf(0, 0) == 5.0 && std::chrono::steady_clock::now() < end) pump(40);
  rig.settle();
  CHECK(makeupOf(0, 0) == 0.0);
  // Adding a pedal in front of the amp: nothing is stored for it.
  Capture added;
  added.file = (kFx / "nam" / "linear_05_025.nam").string();
  added.resolvedPath = added.file;
  REQUIRE(rig.board().addCapturePedal(0, added));
  pump(1500);
  rig.settle();
  const Preset p = rig.preset();
  REQUIRE(p.a.blocks.size() == 3);
  for (std::size_t i = 0; i < 2; ++i) CHECK(makeupOf(0, i) == 0.0);
  settings::Settings::shared().setCalibratedInputLevels(false);
}
