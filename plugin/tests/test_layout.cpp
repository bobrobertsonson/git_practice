// v0.4 Task D (docs/specs/v0_4-pedals.md): the main page is the SAW head and the BODY head side by side with a pedalboard under each, a cab
// chip where the paths meet, and the cab has its own page. Mouse-driven where the UI is: clicks are synthesized mouse events on the real
// components. Needs a display (the editor tests run under xvfb-run). The screenshot test writes main_single.png, main_blend.png and
// cab_page.png when the environment variable SAWBLADE_SCREENSHOT_DIR is set (and only renders them otherwise).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "browser/CaptureBrowser.h"
#include "mic/MicPage.h"
#include "pedals/AdvancedDrawer.h"
#include "pedals/PedalFace.h"
#include "rig/AmpHead.h"
#include "rig/CabControls.h"
#include "rig/CabScreen.h"
#include "rig/Pedalboard.h"
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

juce::MouseEvent mouseAt(juce::Component& c, juce::Point<float> pos, int clicks = 1) {
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, &c, &c, now, pos, now, clicks, true);
}
// A real click: mouse down + up in the middle of the component (Button's mouse handlers are protected; Component's are public).
void click(juce::Component& c) {
  const auto centre = c.getLocalBounds().toFloat().getCentre();
  c.mouseDown(mouseAt(c, centre));
  c.mouseUp(mouseAt(c, centre));
}
void doubleClick(juce::Component& c) {
  const auto centre = c.getLocalBounds().toFloat().getCentre();
  c.mouseDoubleClick(mouseAt(c, centre, 2));
}

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }
bool pumpUntil(const std::function<bool()>& pred, int timeoutMs = 20000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) return false;
    pump(10);
  }
  return true;
}

juce::Button* buttonTitled(juce::Component& root, const juce::String& title) {
  for (auto* b : all<juce::Button>(root))
    if (b->getTitle() == title) return b;
  return nullptr;
}
bool anyLabelEquals(juce::Component& root, const juce::String& text) {
  for (auto* l : all<juce::Label>(root))
    if (l->isVisible() && l->getText() == text) return true;
  return false;
}

struct TempDir {
  fs::path dir;
  TempDir() {
    dir = fs::temp_directory_path() / ("sawblade_layout_tests_" + std::to_string(juce::Random::getSystemRandom().nextInt64() & 0xffffffff));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

// --- preset builders (identity fixtures; modeled pedals; nothing from TONE3000) -----------------------------------------------------
json namBlock(const std::string& id, const std::string& slot, const std::string& title = {}) {
  json model = {{"file", (kFx / "nam" / "linear_identity.nam").string()}};
  if (!title.empty()) model["source"] = {{"provider", "tone3000"}, {"id", "11"}, {"title", title}, {"creator", "fixture"}, {"license", "cc-by"}};
  return {{"id", id}, {"type", "nam"}, {"slot", slot}, {"model", model}};
}
json hmBlock(const std::string& id) {
  return {{"id", id}, {"type", "pedal.hm"}, {"slot", "pedal"}, {"modelVersion", 1}, {"params", {{"level", 6}, {"low", 6}, {"high", 5}, {"distortion", 7}}}};
}
json tsBlock(const std::string& id) {
  return {{"id", id}, {"type", "pedal.ts"}, {"slot", "boost"}, {"modelVersion", 1}, {"params", {{"drive", 2}, {"tone", 6}, {"level", 8}}}};
}
json eqBlock(const std::string& id) {
  return {{"id", id}, {"type", "eq"}, {"slot", "fx"}, {"bands", json::array({{{"type", "peak"}, {"freq", 1000}, {"gainDb", 3}, {"q", 1}}})}};
}
json irCapture(const std::string& title) {
  return {{"file", (kFx / "ir" / "impulse.wav").string()},
          {"source", {{"provider", "tone3000"}, {"id", "21"}, {"title", title}, {"creator", "fixture"}, {"license", "cc-by"}}}};
}
json sharedCab(const std::string& title = "V30 Mesa 4x12") { return {{"mode", "shared"}, {"ir", irCapture(title)}}; }
json perPathCab() { return {{"mode", "perPath"}, {"irA", irCapture("V30 Mesa 4x12 A")}, {"irB", irCapture("V30 Mesa 4x12 B")}}; }

json rigJson(const std::vector<json>& a, const std::vector<json>& b, bool bOn, const json& cab) {
  json ja = json::array(), jb = json::array();
  for (const json& blk : a) ja.push_back(blk);
  for (const json& blk : b) jb.push_back(blk);
  return {{"schema", "sawblade.preset"},
          {"version", 3},
          {"name", "layout test"},
          {"paths", {{"a", {{"role", "saw"}, {"blocks", ja}}}, {"b", {{"role", "body"}, {"enabled", bOn}, {"blocks", jb}}}}},
          {"align", {{"mode", "off"}}},
          {"blend", bOn ? 0.5 : 0.0},
          {"cab", cab}};
}

// A processor with its editor at 1280 x 800.
struct Rig {
  SettingsEnv env{kSettingsExist, /*isolateHome=*/true};
  TempDir tmp;
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;
  int files = 0;

  Rig() {
    proc.prepareToPlay(48000.0, 512);
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  }
  ~Rig() { base.reset(); }

  void wait() {
    REQUIRE(proc.waitForLoader(kLoad));
    REQUIRE(proc.status().error.empty());
    pump(60);
    ed->refreshNow();
  }
  void load(const fs::path& file) {
    REQUIRE(proc.loadPresetFile(file));
    wait();
  }
  void load(const json& j) {
    const fs::path f = tmp.dir / ("p" + std::to_string(files++) + ".json");
    std::ofstream(f) << j.dump(2);
    load(f);
  }
  juce::Rectangle<int> area(juce::Component& c) { return ed->getLocalArea(&c, c.getLocalBounds()); }
  juce::Rectangle<int> boardArea(int path) {
    auto& pb = ed->pedalboard();
    return ed->getLocalArea(&pb, rig::Pedalboard::boardBounds(path));
  }
  skin::RigView& rigView() { return *all<skin::RigView>(*ed).at(0); }
};

// The rectangles of the main page, in editor (= design) coordinates.
struct Geometry {
  juce::Rectangle<int> head0, head1, board0, board1, chip, caption0, caption1, tile00;
};
Geometry geometryOf(Rig& r) {
  Geometry g;
  g.head0 = r.area(r.ed->ampHead(0));
  g.head1 = r.area(r.ed->ampHead(1));
  g.board0 = r.boardArea(0);
  g.board1 = r.boardArea(1);
  g.chip = r.area(r.ed->cabChip());
  g.caption0 = r.ed->getLocalArea(&r.ed->pedalboard(), skin::RigLayout::caption(0));
  g.caption1 = r.ed->getLocalArea(&r.ed->pedalboard(), skin::RigLayout::caption(1));
  if (auto* t = r.ed->pedalboard().tile(0, 0)) g.tile00 = r.area(*t);
  return g;
}

const std::vector<json> kSawPath = {hmBlock("a1"), namBlock("a2", "amp")};
const std::vector<json> kBodyPath = {tsBlock("b1"), namBlock("b2", "amp")};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("layout: the main page at 1280 x 800 has two aligned heads, a board under each, a cab chip, and no overlaps (single and blend)", "[editor][layout]") {
  Rig rig;
  const juce::Rectangle<int> rigArea(0, 58, 940, 742), inspector(940, 58, 340, 742);
  Geometry geo[2];
  for (int blend = 0; blend < 2; ++blend) {
    INFO((blend ? "blend preset" : "single-path preset"));
    rig.load(rigJson(kSawPath, blend ? kBodyPath : std::vector<json>{}, blend != 0, sharedCab()));
    auto& pb = rig.ed->pedalboard();
    CHECK(rig.ed->getWidth() == 1280);
    CHECK(rig.ed->getHeight() == 800);

    // both columns are present: both heads and both boards exist and are visible (B may be off, it is never gone)
    CHECK(rig.ed->ampHead(0).isVisible());
    CHECK(rig.ed->ampHead(1).isVisible());
    CHECK(pb.tileCount(0) == 1);
    CHECK(pb.tileCount(1) == (blend ? 1 : 0));
    CHECK(pb.pathOff(1) == (blend == 0));
    CHECK(pb.addButton(0).isVisible());

    Geometry& g = geo[blend] = geometryOf(rig);
    // heads: same y, same size, SAW left of BODY, 330 px wide (the v0.2 amp overlays stay valid)
    CHECK(g.head0.getY() == g.head1.getY());
    CHECK(g.head0.getWidth() == g.head1.getWidth());
    CHECK(g.head0.getHeight() == g.head1.getHeight());
    CHECK(g.head0.getWidth() == 330);
    CHECK(g.head0.getRight() < g.head1.getX());
    // the head art and its amp controls coincide
    const auto pieces = all<skin::RigPiece>(*rig.ed);
    REQUIRE(pieces.size() == 2);
    CHECK(rig.area(*pieces[0]) == g.head0);
    CHECK(rig.area(*pieces[1]) == g.head1);
    // boards: below their heads (40 px, the column caption between), same y, same size, under their own head
    CHECK(g.board0.getY() == g.head0.getBottom() + 40);
    CHECK(g.board1.getY() == g.board0.getY());
    CHECK(g.board0.getHeight() == g.board1.getHeight());
    CHECK(g.board0.getWidth() == g.board1.getWidth());
    CHECK(g.board0.getBottom() == 58 + 672);
    CHECK(g.board0.getX() <= g.head0.getX());
    CHECK(g.board0.getRight() >= g.head0.getRight());
    CHECK(g.board1.getX() <= g.head1.getX());
    CHECK(g.board1.getRight() >= g.head1.getRight());
    CHECK(g.caption0.getY() >= g.head0.getBottom());
    CHECK(g.caption0.getBottom() <= g.board0.getY());
    CHECK(g.caption1.getBottom() <= g.board1.getY());
    // the cab chip is centred at the bottom, under both boards
    CHECK(g.chip.getY() >= g.board0.getBottom());
    CHECK(g.chip.getWidth() == 320);
    CHECK(g.chip.getHeight() == 36);
    CHECK(g.chip.getCentreX() == 470);
    CHECK(g.chip.getBottom() <= 800);

    // nothing overlaps: heads, boards, chip, inspector; all of the first five inside the rig area
    const std::vector<std::pair<const char*, juce::Rectangle<int>>> parts = {{"head SAW", g.head0},   {"head BODY", g.head1}, {"board SAW", g.board0},
                                                                              {"board BODY", g.board1}, {"cab chip", g.chip},   {"inspector", inspector}};
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i < 5) {
        INFO(parts[i].first << " lies inside the rig area");
        CHECK(rigArea.contains(parts[i].second));
      }
      for (std::size_t j = i + 1; j < parts.size(); ++j) {
        INFO(parts[i].first << " vs " << parts[j].first);
        CHECK_FALSE(parts[i].second.intersects(parts[j].second));
      }
    }
    // every tile lies inside its own board
    for (int path = 0; path < 2; ++path)
      for (int i = 0; i < pb.tileCount(path); ++i) {
        INFO("tile " << path << "/" << i);
        CHECK((path == 0 ? g.board0 : g.board1).contains(rig.area(*pb.tile(path, i))));
      }
    // the static mockup pieces are gone: only the two heads are rig pieces; the cab is a chip
    CHECK(all<skin::CabChip>(*rig.ed).size() == 1);
  }

  // The SAW column does not depend on path B: identical bounds in the single-path and the blend preset.
  CHECK(geo[0].head0 == geo[1].head0);
  CHECK(geo[0].board0 == geo[1].board0);
  CHECK(geo[0].caption0 == geo[1].caption0);
  CHECK(geo[0].tile00 == geo[1].tile00);
  CHECK(geo[0].head1 == geo[1].head1);  // and the BODY column keeps its place when B is off
  CHECK(geo[0].board1 == geo[1].board1);
}

TEST_CASE("layout: path B off shows BODY PATH OFF - turn up BLEND, a dimmed head with disabled controls and an empty board; SAW is unchanged", "[editor][layout]") {
  Rig rig;
  auto& pb = rig.ed->pedalboard();
  // B holds a block but is off: the board stays empty
  rig.load(rigJson(kSawPath, {tsBlock("b1")}, /*bOn=*/false, sharedCab()));
  CHECK(pb.pathOff(1));
  CHECK(pb.tileCount(1) == 0);
  CHECK(pb.offText(1) == rig::AmpHead::bodyOffWithBlocksText());
  CHECK(pb.offText(1).contains("BODY PATH OFF"));
  CHECK(pb.offText(1).contains("turn up BLEND"));
  CHECK(pb.offText(0).isEmpty());
  CHECK(pb.captionText(1).contains("OFF"));
  CHECK_FALSE(pb.addButton(1).isVisible());  // not a drop target / nothing to add to
  CHECK(rig.rigView().bodyOff());
  CHECK(rig.ed->ampHead(1).getAlpha() < 0.5f);
  CHECK(rig.ed->ampHead(1).getAlpha() > 0.2f);
  CHECK_FALSE(rig.ed->ampHead(1).knobsEnabled());
  for (int k = 0; k < kAmpKnobCount; ++k) CHECK_FALSE(rig.ed->ampHead(1).knob(k).isEnabled());
  CHECK(rig.ed->ampHead(1).readout().contains("BODY PATH OFF"));
  // SAW is as usual
  CHECK(rig.ed->ampHead(0).getAlpha() == 1.0f);
  CHECK(rig.ed->ampHead(0).knobsEnabled());
  CHECK(pb.tileCount(0) == 1);
  CHECK(pb.captionText(0).contains("SAW"));
  CHECK(pb.captionText(0).contains("1 PEDAL"));
  CHECK(pb.addButton(0).isVisible());

  // B on: the head is back, the board shows the pedals before B's amp, the text is gone
  rig.load(rigJson(kSawPath, kBodyPath, /*bOn=*/true, sharedCab()));
  CHECK_FALSE(pb.pathOff(1));
  CHECK(pb.tileCount(1) == 1);
  CHECK(pb.offText(1).isEmpty());
  CHECK_FALSE(rig.rigView().bodyOff());
  CHECK(rig.ed->ampHead(1).getAlpha() == 1.0f);
  CHECK(rig.ed->ampHead(1).knobsEnabled());
  CHECK(pb.addButton(1).isVisible());
}

TEST_CASE("layout: a board shows the pedals before the amp; the footswitch is a bypass and one undo step; a refresh keeps the tiles", "[editor][layout]") {
  Rig rig;
  auto& pb = rig.ed->pedalboard();
  // pedal, amp, EQ after the amp: one tile, and the note for the block after the amp
  rig.load(rigJson({hmBlock("a1"), namBlock("a2", "amp"), eqBlock("a3")}, {}, false, sharedCab()));
  REQUIRE(pb.tileCount(0) == 1);
  CHECK(pb.afterAmpText(0) == "+1 AFTER AMP (rig editor)");
  CHECK(pb.afterAmpText(1).isEmpty());
  rig::BoardTile* tile = pb.tile(0, 0);
  REQUIRE(tile != nullptr);
  CHECK(tile->blockId() == "a1");
  CHECK(tile->name() == "CHAINSAW");  // the circuit's generic descriptor
  CHECK(tile->isCircuit());
  CHECK_FALSE(tile->bypassed());
  CHECK(tile->led().isOn());
  CHECK(tile->footswitch().getToggleState());
  CHECK(pb.captionText(0).contains("1 PEDAL"));

  // a refresh with the same blocks never rebuilds the tiles
  rig.ed->refreshNow();
  rig.ed->refreshNow();
  CHECK(pb.tile(0, 0) == tile);

  // the footswitch: one RigController::edit, one undo step
  const Preset before = rig.proc.currentPreset();
  CHECK_FALSE(before.a.blocks[0].bypass);
  CHECK_FALSE(rig.ed->rigController().canUndo());
  click(tile->footswitch());
  REQUIRE(rig.proc.waitForLoader(kLoad));
  CHECK(rig.proc.currentPreset().a.blocks[0].bypass);
  CHECK(rig.proc.currentPreset().a.blocks.size() == 3);
  CHECK(rig.ed->rigController().canUndo());
  rig.ed->refreshNow();
  CHECK(pb.tile(0, 0) == tile);  // the same widget: nothing was rebuilt under the hand
  CHECK(tile->bypassed());
  CHECK_FALSE(tile->led().isOn());
  // the rig editor shows the same bypass (one source of truth)
  CHECK(rig.ed->rigController().view().a.blocks[0].bypass);

  REQUIRE(rig.ed->rigController().undo());
  REQUIRE(rig.proc.waitForLoader(kLoad));
  CHECK_FALSE(rig.proc.currentPreset().a.blocks[0].bypass);
  rig.ed->refreshNow();
  CHECK_FALSE(tile->bypassed());
  CHECK(tile->led().isOn());
  CHECK_FALSE(rig.ed->rigController().canUndo());  // exactly one step

  // changing the block list rebuilds the tiles
  rig.load(rigJson({hmBlock("a1"), tsBlock("a4"), namBlock("a2", "amp")}, {}, false, sharedCab()));
  CHECK(pb.tileCount(0) == 2);
  CHECK(pb.tile(0, 1)->name() == "GREEN OVERDRIVE");
  CHECK_FALSE(pb.tile(0, 1)->isCircuit());
  CHECK(pb.afterAmpText(0).isEmpty());
}

TEST_CASE("layout: the pedal face lies over the circuit tile and the drawer opens beside it (SAW and BODY columns)", "[editor][layout]") {
  Rig rig;
  auto& pb = rig.ed->pedalboard();
  auto faces = all<PedalFace>(*rig.ed);
  auto drawers = all<AdvancedDrawer>(*rig.ed);
  REQUIRE(faces.size() == 1);
  REQUIRE(drawers.size() == 1);
  PedalFace& face = *faces[0];
  AdvancedDrawer& drawer = *drawers[0];

  // circuit pedal on the SAW board
  rig.load(rigJson(kSawPath, {}, false, sharedCab()));
  face.refresh();
  REQUIRE(face.isVisible());
  rig::BoardTile* tile = pb.tileForBlock(0, 0);
  REQUIRE(tile != nullptr);
  CHECK(face.getBounds() == pb.tileBounds(*tile) + pb.getPosition());
  doubleClick(*tile);  // the drawer belongs to the tile that carries the face
  CHECK(drawer.isOpen());
  drawer.finishAnimation();
  CHECK(drawer.getX() == face.getRight() + AdvancedDrawer::kGap);
  CHECK(drawer.getWidth() >= AdvancedDrawer::kMinOpenWidth);
  drawer.setOpen(false, false);

  // the only circuit pedal is on the BODY board: the face follows it, the drawer opens to its left
  rig.load(rigJson({namBlock("a1", "amp")}, {hmBlock("b1"), namBlock("b2", "amp")}, true, sharedCab()));
  face.refresh();
  REQUIRE(face.isVisible());
  tile = pb.tileForBlock(1, 0);
  REQUIRE(tile != nullptr);
  CHECK(face.getBounds() == pb.tileBounds(*tile) + pb.getPosition());
  CHECK(face.getX() >= 478);  // in the BODY column
  doubleClick(*tile);
  CHECK(drawer.isOpen());
  drawer.finishAnimation();
  CHECK(drawer.getRight() + AdvancedDrawer::kGap == face.getX());
  CHECK(drawer.getX() >= 0);
  CHECK(drawer.getWidth() >= AdvancedDrawer::kMinOpenWidth);
  drawer.setOpen(false, false);

  // a circuit block after the amp has no tile: no face
  rig.load(rigJson({namBlock("a1", "amp"), hmBlock("a2")}, {}, false, sharedCab()));
  face.refresh();
  CHECK_FALSE(face.isVisible());
  CHECK(pb.tileCount(0) == 0);
  CHECK(pb.afterAmpText(0) == "+1 AFTER AMP (rig editor)");
}

TEST_CASE("layout: selecting a pedal tile shows SELECTED - SAW / BODY PEDAL and its name; BROWSE CAPTURES targets that block", "[editor][layout][browser]") {
  Rig rig;
  auto& pb = rig.ed->pedalboard();
  const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");
  // two capture pedals and an amp in SAW, a modeled boost and an amp in BODY
  rig.load(rigJson({namBlock("a1", "pedal", "Pedal One"), namBlock("a2", "pedal", "Pedal Two"), namBlock("a3", "amp")}, kBodyPath, true, sharedCab()));
  REQUIRE(pb.tileCount(0) == 2);
  REQUIRE(pb.tileCount(1) == 1);
  CHECK(rig.ed->selectedPiece() == skin::Piece::SawPedal);  // the default selection
  CHECK(rig.ed->selectedBlockId().empty());
  CHECK(anyLabelEquals(*rig.ed, "SELECTED" + dot + "SAW PEDAL"));
  CHECK(anyLabelEquals(*rig.ed, "PEDAL ONE"));  // the first tile of the path

  auto openBrowserAndTargets = [&](std::vector<SlotTarget>& targets, std::string* why = nullptr) {
    auto* browse = buttonTitled(*rig.ed, "BROWSE CAPTURES");
    REQUIRE(browse != nullptr);
    browse->triggerClick();  // asynchronous
    REQUIRE(pumpUntil([&] { return rig.ed->captureBrowserOpen(); }));
    auto browsers = all<CaptureBrowser>(*rig.ed);
    REQUIRE(browsers.size() == 1);
    targets = browsers[0]->controller().targets(why);
    rig.ed->closeAllOverlaysForTests();
    REQUIRE(pumpUntil([&] { return all<CaptureBrowser>(*rig.ed).empty(); }));
  };

  std::vector<SlotTarget> targets;
  openBrowserAndTargets(targets);  // nothing picked: the slot's usual block (the first pedal)
  REQUIRE(targets.size() == 1);
  CHECK(targets[0].path == 'a');
  CHECK(targets[0].blockIndex == 0);

  // pick the second pedal tile: the inspector follows, and BROWSE CAPTURES targets that very block
  click(*pb.tile(0, 1));
  CHECK(rig.ed->selectedPiece() == skin::Piece::SawPedal);
  CHECK(rig.ed->selectedBlockId() == "a2");
  CHECK(anyLabelEquals(*rig.ed, "SELECTED" + dot + "SAW PEDAL"));
  CHECK(anyLabelEquals(*rig.ed, "PEDAL TWO"));
  CHECK(pb.tile(0, 1)->selected());
  CHECK_FALSE(pb.tile(0, 0)->selected());
  openBrowserAndTargets(targets);
  REQUIRE(targets.size() == 1);
  CHECK(targets[0].path == 'a');
  CHECK(targets[0].blockIndex == 1);

  // a BODY tile: "BODY PEDAL" and the modeled name; a modeled circuit has no capture to replace
  click(*pb.tile(1, 0));
  CHECK(rig.ed->selectedPiece() == skin::Piece::BodyPedal);
  CHECK(rig.ed->selectedBlockId() == "b1");
  CHECK(anyLabelEquals(*rig.ed, "SELECTED" + dot + "BODY PEDAL"));
  CHECK(anyLabelEquals(*rig.ed, "GREEN OVERDRIVE"));
  CHECK_FALSE(pb.tile(0, 1)->selected());
  std::string why;
  openBrowserAndTargets(targets, &why);
  CHECK(targets.empty());
  CHECK(why.find("modeled circuit") != std::string::npos);

  // an amp head: back to the amp slot
  auto heads = all<skin::RigPiece>(*rig.ed);
  REQUIRE(heads.size() == 2);
  heads[0]->mouseDown(mouseAt(*heads[0], {5.0f, 5.0f}));
  CHECK(rig.ed->selectedPiece() == skin::Piece::SawAmp);
  CHECK(anyLabelEquals(*rig.ed, "SELECTED" + dot + "SAW AMP"));
  CHECK_FALSE(pb.tile(1, 0)->selected());
  openBrowserAndTargets(targets);
  REQUIRE(targets.size() == 1);
  CHECK(targets[0].blockIndex == 2);

  // the selected block disappears from the board: the selection falls back to the first tile of the path, never a dangling tile
  click(*pb.tile(0, 1));
  REQUIRE(rig.ed->selectedBlockId() == "a2");
  rig.load(rigJson({namBlock("a1", "pedal", "Pedal One"), namBlock("a3", "amp")}, {}, false, sharedCab()));
  CHECK(pb.tileCount(0) == 1);
  CHECK(rig.ed->selectedPiece() == skin::Piece::SawPedal);
  CHECK(rig.ed->selectedBlockId().empty());
  CHECK(anyLabelEquals(*rig.ed, "SELECTED" + dot + "SAW PEDAL"));
  CHECK(anyLabelEquals(*rig.ed, "PEDAL ONE"));
  CHECK(pb.tile(0, 0)->selected());
}

TEST_CASE("layout: the CAB button and the cab chip open the CAB page; it is an overlay of the mutually exclusive group", "[editor][layout][cab]") {
  Rig rig;
  rig.load(rigJson(kSawPath, kBodyPath, true, sharedCab()));
  auto* cabBtn = buttonTitled(*rig.ed, "Cab page");
  auto* rigBtn = buttonTitled(*rig.ed, "RIG");
  REQUIRE(cabBtn != nullptr);
  REQUIRE(rigBtn != nullptr);
  CHECK(cabBtn->getButtonText() == "CAB");
  CHECK(cabBtn->getTooltip().isNotEmpty());
  CHECK(rig.area(*cabBtn).getBottom() <= 58);  // on the top bar
  CHECK(rig.area(*cabBtn).getX() > rig.area(*rigBtn).getRight());  // next to RIG
  CHECK(rig.area(*cabBtn).getX() - rig.area(*rigBtn).getRight() <= 24);
  for (auto* b : all<juce::Button>(*rig.ed))  // nothing on the top bar overlaps the new button
    if (b != cabBtn && b->getParentComponent() == cabBtn->getParentComponent() && b->getY() < 58) {
      INFO(b->getTitle());
      CHECK_FALSE(b->getBounds().intersects(cabBtn->getBounds()));
    }

  CHECK_FALSE(rig.ed->cabPageOpen());
  CHECK_FALSE(rig.ed->cabScreen().isVisible());
  click(*cabBtn);
  CHECK(rig.ed->cabPageOpen());
  CHECK(cabBtn->getToggleState());
  CHECK(rig.area(rig.ed->cabScreen()) == juce::Rectangle<int>(0, 58, 1280, 742));  // below the top bar, over the rig and the inspector
  click(*cabBtn);
  CHECK_FALSE(rig.ed->cabPageOpen());
  CHECK_FALSE(cabBtn->getToggleState());

  // the cab chip
  auto& chip = rig.ed->cabChip();
  CHECK(chip.text().startsWith("CAB"));
  click(chip);
  CHECK(rig.ed->cabPageOpen());
  CHECK(cabBtn->getToggleState());
  // the BACK button closes it
  click(rig.ed->cabScreen().backButton());
  CHECK_FALSE(rig.ed->cabPageOpen());
  CHECK_FALSE(cabBtn->getToggleState());

  // mutually exclusive with the others; the buttons follow
  rig.ed->setRigEditorOpen(true);
  click(*cabBtn);
  CHECK(rig.ed->cabPageOpen());
  CHECK_FALSE(rig.ed->rigEditorOpen());
  CHECK_FALSE(rigBtn->getToggleState());
  click(*rigBtn);
  CHECK(rig.ed->rigEditorOpen());
  CHECK_FALSE(rig.ed->cabPageOpen());
  CHECK_FALSE(cabBtn->getToggleState());
  rig.ed->setRigEditorOpen(false);
  click(*cabBtn);
  rig.ed->setBrowserOpen(true);
  CHECK_FALSE(rig.ed->cabPageOpen());
  CHECK_FALSE(cabBtn->getToggleState());
  rig.ed->setBrowserOpen(false);
  click(*cabBtn);
  rig.ed->setMicPageOpen(true);
  CHECK_FALSE(rig.ed->cabPageOpen());
  rig.ed->setMicPageOpen(false);
  CHECK_FALSE(rig.ed->cabPageOpen());  // a mic page opened directly does not come back to the CAB page

  // UI state: never saved
  rig.ed->setCabPageOpen(true);
  juce::MemoryBlock state;
  rig.proc.getStateInformation(state);
  const juce::String text = juce::String::fromUTF8(static_cast<const char*>(state.getData()), static_cast<int>(state.getSize()));
  CHECK_FALSE(text.containsIgnoreCase("cabPage"));
  rig.ed->setCabPageOpen(false);
}

TEST_CASE("layout: the cab chip names the cab and says LIVE or STUDIO", "[editor][layout][cab]") {
  Rig rig;
  const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");
  const juce::String bullet = juce::String::fromUTF8("\xe2\x97\x8f ");
  auto& chip = rig.ed->cabChip();

  rig.load(rigJson(kSawPath, kBodyPath, true, sharedCab("V30 Mesa 4x12")));
  CHECK(chip.mode() == skin::CabChip::Mode::Live);
  CHECK(chip.text() == "CAB" + dot + "V30 Mesa 4x12" + dot + bullet + "LIVE");

  rig.load(rigJson(kSawPath, kBodyPath, true, perPathCab()));  // per-path cabs: the studio blend
  CHECK(chip.mode() == skin::CabChip::Mode::Studio);
  CHECK(chip.text().contains("V30 Mesa 4x12 A + V30 Mesa 4x12 B"));
  CHECK(chip.text().endsWith(bullet + "STUDIO"));

  json off = sharedCab();
  off["enabled"] = false;
  rig.load(rigJson(kSawPath, kBodyPath, true, off));
  CHECK(chip.mode() == skin::CabChip::Mode::Off);
  CHECK(chip.text() == "CAB OFF");

  // the chip is the same LIVE / STUDIO rule as the top bar's mode chip
  rig.load(rigJson(kSawPath, kBodyPath, true, perPathCab()));
  juce::String modeText;
  for (auto* l : all<juce::Label>(*rig.ed))
    if (l->getTitle() == "Blend mode") modeText = l->getText();
  CHECK(modeText.contains("STUDIO"));
  CHECK(chip.text().contains("STUDIO"));
}

TEST_CASE("layout: everything the old cab area did is reachable by mouse on the CAB page", "[editor][layout][cab]") {
  Rig rig;
  rig.load(rigJson(kSawPath, kBodyPath, true, sharedCab()));
  auto* cabBtn = buttonTitled(*rig.ed, "Cab page");
  REQUIRE(cabBtn != nullptr);
  click(*cabBtn);
  REQUIRE(rig.ed->cabPageOpen());
  rig::CabScreen& page = rig.ed->cabScreen();
  rig::CabControls& cab = page.controls();

  // shared: one IR card with the cab's IR, CHOOSE... and BROWSE IR; the LIVE notice; MIC POSITIONS
  CHECK(cab.cardVisible(rig::CabSlot::Shared));
  CHECK_FALSE(cab.cardVisible(rig::CabSlot::A));
  CHECK_FALSE(cab.cardVisible(rig::CabSlot::B));
  CHECK(cab.cardTitle(rig::CabSlot::Shared) == "V30 Mesa 4x12");
  CHECK(cab.chooseButton(rig::CabSlot::Shared).isVisible());
  CHECK(cab.browseButton(rig::CabSlot::Shared).isVisible());
  CHECK(cab.micButton().isVisible());
  CHECK(cab.noticeText().startsWith("LIVE-COMPATIBLE"));
  CHECK(cab.cabOnButton().getToggleState());
  const std::vector<juce::Button*> buttons = {&cab.chooseButton(rig::CabSlot::Shared), &cab.browseButton(rig::CabSlot::Shared), &cab.micButton(),
                                              &cab.cabOnButton(),                       &cab.mode.button(0),                       &cab.mode.button(1)};
  for (juce::Button* b : buttons) {
    INFO(b->getTitle());
    CHECK(b->getTitle().isNotEmpty());
    CHECK(b->getTooltip().isNotEmpty());
  }

  // CAB ON
  click(cab.cabOnButton());
  REQUIRE(rig.proc.waitForLoader(kLoad));
  CHECK_FALSE(rig.proc.currentPreset().cab.enabled);
  pump(30);
  rig.ed->refreshNow();
  CHECK_FALSE(cab.cabOnButton().getToggleState());
  CHECK(rig.ed->cabChip().mode() == skin::CabChip::Mode::Off);  // the chip on the main page follows
  click(cab.cabOnButton());
  REQUIRE(rig.proc.waitForLoader(kLoad));
  CHECK(rig.proc.currentPreset().cab.enabled);

  // mode PER PATH: two IR cards, the STUDIO notice
  click(cab.mode.button(1));
  REQUIRE(rig.proc.waitForLoader(kLoad));
  CHECK(rig.proc.currentPreset().cab.mode == CabMode::PerPath);
  pump(30);
  rig.ed->refreshNow();
  CHECK_FALSE(cab.cardVisible(rig::CabSlot::Shared));
  CHECK(cab.cardVisible(rig::CabSlot::A));
  CHECK(cab.cardVisible(rig::CabSlot::B));
  CHECK(cab.noticeText().startsWith("STUDIO BLEND"));
  CHECK(cab.browseButton(rig::CabSlot::A).isVisible());
  CHECK(cab.browseButton(rig::CabSlot::B).isVisible());
  CHECK(rig.ed->cabChip().mode() == skin::CabChip::Mode::Studio);

  // BROWSE IR (per path: either card): the capture browser for the cab, with both IR targets
  click(cab.browseButton(rig::CabSlot::B));
  REQUIRE(rig.ed->captureBrowserOpen());
  CHECK_FALSE(rig.ed->cabPageOpen());  // the browser covers the whole editor
  {
    auto browsers = all<CaptureBrowser>(*rig.ed);
    REQUIRE(browsers.size() == 1);
    CHECK(browsers[0]->controller().slot() == Slot::Cab);
    const auto targets = browsers[0]->controller().targets();
    REQUIRE(targets.size() == 2);
    CHECK(targets[0].kind == SlotTarget::Kind::CabA);
    CHECK(targets[1].kind == SlotTarget::Kind::CabB);
  }
  rig.ed->closeAllOverlaysForTests();
  REQUIRE(pumpUntil([&] { return all<CaptureBrowser>(*rig.ed).empty(); }));

  // back to SHARED, then BROWSE IR from the shared card: one target
  click(*cabBtn);
  REQUIRE(rig.ed->cabPageOpen());
  click(cab.mode.button(0));
  REQUIRE(rig.proc.waitForLoader(kLoad));
  CHECK(rig.proc.currentPreset().cab.mode == CabMode::Shared);
  pump(30);
  rig.ed->refreshNow();
  CHECK(cab.cardVisible(rig::CabSlot::Shared));
  click(cab.browseButton(rig::CabSlot::Shared));
  REQUIRE(rig.ed->captureBrowserOpen());
  {
    auto browsers = all<CaptureBrowser>(*rig.ed);
    REQUIRE(browsers.size() == 1);
    CHECK(browsers[0]->controller().slot() == Slot::Cab);
    CHECK(browsers[0]->controller().targets().size() == 1);
  }
  rig.ed->closeAllOverlaysForTests();
  REQUIRE(pumpUntil([&] { return all<CaptureBrowser>(*rig.ed).empty(); }));

  // MIC POSITIONS: the mic page; its "< RIG" comes back to the CAB page
  click(*cabBtn);
  REQUIRE(rig.ed->cabPageOpen());
  CHECK_FALSE(rig.ed->micPageOpen());
  click(cab.micButton());
  REQUIRE(rig.ed->micPageOpen());
  CHECK_FALSE(rig.ed->cabPageOpen());
  CHECK_FALSE(cabBtn->getToggleState());
  auto* back = rig.ed->micPage().buttonTitled(juce::String::fromUTF8("\xe2\x80\xb9 RIG"));
  REQUIRE(back != nullptr);
  click(*back);
  CHECK_FALSE(rig.ed->micPageOpen());
  CHECK(rig.ed->cabPageOpen());
  CHECK(cabBtn->getToggleState());
  // a second round trip works the same
  click(cab.micButton());
  REQUIRE(rig.ed->micPageOpen());
  click(*rig.ed->micPage().buttonTitled(juce::String::fromUTF8("\xe2\x80\xb9 RIG")));
  CHECK(rig.ed->cabPageOpen());

  // double-clicking the cab is gone from the main page: no rig piece but the two heads, and the chip is the entry point
  click(*cabBtn);
  CHECK_FALSE(rig.ed->cabPageOpen());
  CHECK(all<skin::RigPiece>(*rig.ed).size() == 2);
}

TEST_CASE("layout: Cmd / Ctrl + Z does not undo under the CAB page", "[editor][layout][cab]") {
  Rig rig;
  rig.load(rigJson(kSawPath, {}, false, sharedCab()));
  auto& pb = rig.ed->pedalboard();
  const juce::KeyPress undoKey('z', juce::ModifierKeys::commandModifier, 0);
  // an edit to undo: the bypass of the pedal
  click(pb.tile(0, 0)->footswitch());
  REQUIRE(rig.proc.waitForLoader(kLoad));
  REQUIRE(rig.proc.currentPreset().a.blocks[0].bypass);
  REQUIRE(rig.ed->rigController().canUndo());
  const Preset edited = rig.proc.currentPreset();

  rig.ed->setCabPageOpen(true);
  REQUIRE(rig.ed->cabPageOpen());
  CHECK_FALSE(rig.ed->keyPressed(undoKey));
  // the real path: the key bubbles from the focused child up through its parents to the editor
  bool handled = false;
  for (juce::Component* c = &rig.ed->cabScreen().backButton(); c != nullptr; c = c->getParentComponent())
    if (c->keyPressed(undoKey)) handled = true;
  CHECK_FALSE(handled);
  CHECK(rig.proc.currentPreset() == edited);
  CHECK(rig.ed->rigController().canUndo());

  rig.ed->setCabPageOpen(false);
  CHECK(rig.ed->keyPressed(undoKey));  // nothing open: it undoes
  REQUIRE(rig.proc.waitForLoader(kLoad));
  CHECK_FALSE(rig.proc.currentPreset().a.blocks[0].bypass);
}

TEST_CASE("layout: BROWSE CAPTURES follows the tile the inspector names, also with the default selection (a modeled first tile + a capture later)", "[editor][layout][browser]") {
  Rig rig;
  auto& pb = rig.ed->pedalboard();
  // first tile modeled, second a capture, then the amp
  rig.load(rigJson({hmBlock("a1"), namBlock("a2", "pedal", "Pedal Two"), namBlock("a3", "amp")}, {}, false, sharedCab()));
  REQUIRE(pb.tileCount(0) == 2);
  const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");
  CHECK(rig.ed->selectedBlockId().empty());  // the default selection ...
  CHECK(anyLabelEquals(*rig.ed, "CHAINSAW"));  // ... names the first tile
  auto* browse = buttonTitled(*rig.ed, "BROWSE CAPTURES");
  REQUIRE(browse != nullptr);
  browse->triggerClick();
  REQUIRE(pumpUntil([&] { return rig.ed->captureBrowserOpen(); }));
  {
    auto browsers = all<CaptureBrowser>(*rig.ed);
    REQUIRE(browsers.size() == 1);
    std::string why;
    // not the capture further down the board: the tile shown is a modeled circuit and has nothing to replace
    CHECK(browsers[0]->controller().targets(&why).empty());
    CHECK(why.find("modeled circuit") != std::string::npos);
  }
  rig.ed->closeAllOverlaysForTests();
  REQUIRE(pumpUntil([&] { return all<CaptureBrowser>(*rig.ed).empty(); }));
  // picking the capture tile targets it
  click(*pb.tile(0, 1));
  browse->triggerClick();
  REQUIRE(pumpUntil([&] { return rig.ed->captureBrowserOpen(); }));
  {
    auto browsers = all<CaptureBrowser>(*rig.ed);
    REQUIRE(browsers.size() == 1);
    const auto targets = browsers[0]->controller().targets();
    REQUIRE(targets.size() == 1);
    CHECK(targets[0].blockIndex == 1);
  }
  rig.ed->closeAllOverlaysForTests();
  REQUIRE(pumpUntil([&] { return all<CaptureBrowser>(*rig.ed).empty(); }));
  (void)dot;
}

TEST_CASE("layout: the top bar keeps every button and chip apart, and the latency chip is not squeezed", "[editor][layout]") {
  Rig rig;
  rig.load(rigJson(kSawPath, kBodyPath, true, sharedCab()));
  auto* cabBtn = buttonTitled(*rig.ed, "Cab page");
  REQUIRE(cabBtn != nullptr);
  juce::Component* bar = cabBtn->getParentComponent();
  std::vector<juce::Component*> parts;
  juce::Label* lat = nullptr;
  for (auto* c : bar->getChildren()) {
    if (!c->isVisible() || c->getBottom() > 58) continue;
    if (dynamic_cast<juce::Button*>(c) != nullptr) parts.push_back(c);
    if (auto* l = dynamic_cast<juce::Label*>(c)) {
      if (l->getTitle() == "Latency") lat = l;
      if (l->getTitle() == "Latency" || l->getTitle() == "Blend mode") parts.push_back(c);
    }
  }
  REQUIRE(lat != nullptr);
  CHECK(lat->getWidth() >= 130);
  CHECK(parts.size() >= 12);
  for (std::size_t i = 0; i < parts.size(); ++i)
    for (std::size_t j = i + 1; j < parts.size(); ++j) {
      INFO(parts[i]->getTitle() << " vs " << parts[j]->getTitle());
      CHECK_FALSE(parts[i]->getBounds().intersects(parts[j]->getBounds()));
    }
}

TEST_CASE("layout: the mic page only returns to the CAB page when it was opened from it", "[editor][layout][cab]") {
  Rig rig;
  rig.load(rigJson(kSawPath, kBodyPath, true, sharedCab()));
  rig.ed->setCabPageOpen(true);
  click(rig.ed->cabScreen().controls().micButton());
  REQUIRE(rig.ed->micPageOpen());
  rig.ed->setMicPageOpen(false);  // closed by other means than its back button
  CHECK_FALSE(rig.ed->cabPageOpen());
  rig.ed->setMicPageOpen(true);   // opened again directly (not from the CAB page)
  auto* back = rig.ed->micPage().buttonTitled(juce::String::fromUTF8("\xe2\x80\xb9 RIG"));
  REQUIRE(back != nullptr);
  click(*back);
  CHECK_FALSE(rig.ed->micPageOpen());
  CHECK_FALSE(rig.ed->cabPageOpen());  // a stale "came from the CAB page" must not bring it back
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Screenshots for the report: main_single.png, main_blend.png, cab_page.png in $SAWBLADE_SCREENSHOT_DIR (docs/reports/v0_4/). Without the
// variable the pages are still rendered and checked, nothing is written.
TEST_CASE("layout: screenshots of the main page (single, blend) and the cab page", "[editor][layout][screenshots]") {
  Rig rig;
  const char* envDir = std::getenv("SAWBLADE_SCREENSHOT_DIR");
  const bool write = envDir != nullptr && *envDir != '\0';

  auto snapshot = [&](const char* name) {
    rig.ed->refreshNow();
    pump(30);
    rig.ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
    const juce::Image img = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 1.0f);
    REQUIRE(img.getWidth() == 1280);
    REQUIRE(img.getHeight() == 800);
    // not blank: the luminance of a sample of the pixels varies
    double sum = 0.0, sum2 = 0.0;
    int n = 0;
    for (int y = 0; y < img.getHeight(); y += 4)
      for (int x = 0; x < img.getWidth(); x += 4) {
        const juce::Colour c = img.getPixelAt(x, y);
        const double l = 0.299 * c.getFloatRed() + 0.587 * c.getFloatGreen() + 0.114 * c.getFloatBlue();
        sum += l;
        sum2 += l * l;
        ++n;
      }
    const double mean = sum / n;
    INFO(name);
    CHECK(std::sqrt(std::max(0.0, sum2 / n - mean * mean)) > 0.03);
    if (!write) return;
    const juce::File dir(envDir);
    REQUIRE(dir.createDirectory().wasOk());
    const juce::File f = dir.getChildFile(name);
    f.deleteFile();
    juce::FileOutputStream out(f);
    REQUIRE(out.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(img, out));
  };

  // single path: SAW with a chainsaw circuit, a boost and an amp; BODY off
  rig.load(rigJson({hmBlock("a1"), tsBlock("a2"), namBlock("a3", "amp")}, {}, false, sharedCab("V30 Mesa 4x12")));
  snapshot("main_single.png");
  // blend: both columns
  rig.load(rigJson({hmBlock("a1"), namBlock("a2", "amp")}, {tsBlock("b1"), namBlock("b2", "amp")}, true, sharedCab("V30 Mesa 4x12")));
  snapshot("main_blend.png");
  // the cab page with per-path cabs (two IR cards, the STUDIO notice)
  rig.load(rigJson({hmBlock("a1"), namBlock("a2", "amp")}, {tsBlock("b1"), namBlock("b2", "amp")}, true, perPathCab()));
  rig.ed->setCabPageOpen(true);
  snapshot("cab_page.png");
  rig.ed->setCabPageOpen(false);
}
