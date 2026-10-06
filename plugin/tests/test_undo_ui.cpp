// v0.3 Task D at the level the user acts: knob drags (the main page's host-parameter knobs and the rig editor's preset knobs), Cmd / Ctrl
// + Z and + Shift, the overlay rule, and a closed / reopened editor. Needs a display (the editor tests run under xvfb-run).
// "Exact" means JSON-exact (toJson carries the level-match stamp; Preset::operator== ignores it, by design).
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "rig/RigController.h"
#include "rig/RigEditorPanel.h"
#include "rig/RigWidgets.h"
#include "skin/FilmstripKnob.h"

using namespace sawblade;
using namespace sawblade::plugin;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kFx = SAWBLADE_FIXTURES_DIR;

template <class T>
void collect(juce::Component& c, std::vector<T*>& out) {
  for (auto* child : c.getChildren()) {
    if (auto* t = dynamic_cast<T*>(child)) out.push_back(t);
    collect(*child, out);
  }
}

juce::MouseEvent ev(juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos) {
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, &c, &c, now, downPos, now, 1, true);
}
// A mouse drag in `moves` steps of `dy` pixels each (negative = up): 250 px = the full range.
void drag(skin::FilmstripKnob& k, float dy, int moves = 4) {
  const juce::Point<float> start(15.0f, 15.0f);
  k.mouseDown(ev(k, start, start));
  for (int i = 1; i <= moves; ++i) k.mouseDrag(ev(k, {15.0f, 15.0f + dy * static_cast<float>(i)}, start));
  k.mouseUp(ev(k, {15.0f, 15.0f + dy * static_cast<float>(moves)}, start));
}

std::string J(const Preset& p) { return toJson(p).dump(); }

json rigJson() {
  const auto nam = [&](const std::string& id) {
    return json{{"id", id}, {"type", "nam"}, {"slot", "amp"}, {"model", {{"file", (kFx / "nam" / "linear_identity.nam").string()}}}};
  };
  return {{"schema", "sawblade.preset"}, {"version", 3}, {"name", "undo ui"},
          {"paths", {{"a", {{"blocks", json::array({nam("a1")})}}}, {"b", {{"blocks", json::array({nam("b1")})}}}}},
          {"align", {{"mode", "off"}}}, {"blend", 0.5},
          {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}};
}

struct Rig {
  SettingsEnv env{R"({"levelMatch": false})", /*isolateHome=*/true};
  fs::path dir;
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;

  Rig() {
    static int n = 0;
    dir = fs::temp_directory_path() / ("sawblade_undoui_" + std::to_string(std::random_device{}()) + "_" + std::to_string(n++));
    fs::create_directories(dir);
    proc.prepareToPlay(48000.0, 512);
    open();
    const fs::path f = dir / "p.json";
    std::ofstream(f) << rigJson().dump(2);
    REQUIRE(proc.loadPresetFile(f));
    settle();
  }
  ~Rig() {
    base.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void open() {
    base.reset(proc.createEditor());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  }
  void close() {
    base.reset();
    ed = nullptr;
  }
  void settle() {
    REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
    juce::MessageManager::getInstance()->runDispatchLoopUntil(40);
    if (ed) ed->refreshNow();
  }
  std::string now() const { return J(proc.currentPreset()); }
  skin::FilmstripKnob& knob(const std::string& paramId) {
    std::vector<skin::FilmstripKnob*> all;
    collect(*ed, all);
    for (auto* k : all)
      if (k->paramId() == juce::String(paramId)) return *k;
    FAIL("no knob " << paramId);
    return *all.front();
  }
  double param(int i) { return static_cast<double>(proc.parameters().getRawParameterValue(paramSpec(i).id)->load()); }
  bool key(juce::juce_wchar c, bool shift) {
    return ed->keyPressed(juce::KeyPress(c, juce::ModifierKeys::commandModifier | (shift ? juce::ModifierKeys::shiftModifier : 0), 0));
  }
};

}  // namespace

TEST_CASE("undo ui: a host-parameter knob drag is one step; Cmd+Z restores it, Cmd+Shift+Z redoes it", "[undoui][editor]") {
  Rig r;
  const std::string before = r.now();
  const double v0 = r.param(kOutputGain);
  drag(r.knob(paramSpec(kOutputGain).id), -40.0f, 6);
  r.settle();
  const double v1 = r.param(kOutputGain);
  REQUIRE(v1 != Catch::Approx(v0));
  CHECK(r.proc.undoSteps() == 1);  // six mouse moves, one step
  const std::string after = r.now();
  REQUIRE(after != before);
  CHECK(r.key('z', false));
  r.settle();
  CHECK(r.now() == before);
  CHECK(r.param(kOutputGain) == Catch::Approx(v0).margin(1e-6));
  CHECK_FALSE(r.key('z', false));  // nothing more to undo
  CHECK(r.key('z', true));         // Cmd+Shift+Z
  r.settle();
  CHECK(r.now() == after);
  CHECK(r.param(kOutputGain) == Catch::Approx(v1).margin(1e-6));
  CHECK_FALSE(r.key('Z', true));  // nothing to redo (upper-case key code, as a Shift chord may deliver)
}

TEST_CASE("undo ui: the main page BLEND and level knobs are undoable per user gesture; host automation of them is not", "[undoui][editor]") {
  Rig r;
  r.proc.historyClear();
  // Host automation: moves the knobs, records nothing.
  for (int i = 0; i < 5; ++i) {
    auto* p = r.proc.parameters().getParameter(paramSpec(kBlend).id);
    p->setValueNotifyingHost(0.1f * static_cast<float>(i));
  }
  CHECK(r.proc.undoSteps() == 0);
  // The user drags BLEND (the topology is already a blend: no fill) and the input gain.
  const std::string s0 = r.now();
  drag(r.knob(paramSpec(kBlend).id), 30.0f);
  r.settle();
  CHECK(r.proc.undoSteps() == 1);
  const std::string s1 = r.now();
  drag(r.knob(paramSpec(kInputGain).id), -30.0f);
  r.settle();
  CHECK(r.proc.undoSteps() == 2);
  CHECK(r.key('z', false));
  r.settle();
  CHECK(r.now() == s1);
  CHECK(r.key('z', false));
  r.settle();
  CHECK(r.now() == s0);
}

TEST_CASE("undo ui: a rig editor knob drag is one step", "[undoui][editor]") {
  Rig r;
  r.ed->setRigEditorOpen(true);
  r.settle();
  std::vector<rig::PresetKnob*> knobs;
  collect(*r.ed, knobs);
  REQUIRE_FALSE(knobs.empty());
  r.proc.historyClear();
  const std::string before = r.now();
  bool moved = false;
  for (auto* k : knobs) {
    if (!k->isShowing() && !k->isVisible()) continue;
    drag(k->knob(), -50.0f, 5);
    r.settle();
    if (r.now() != before) {
      moved = true;
      break;
    }
  }
  REQUIRE(moved);
  CHECK(r.proc.undoSteps() == 1);
  CHECK(r.key('z', false));  // the rig editor is not an overlay for the undo chord
  r.settle();
  CHECK(r.now() == before);
  CHECK(r.key('z', true));
  r.settle();
  CHECK(r.now() != before);
}

TEST_CASE("undo ui: Cmd+Z and Cmd+Shift+Z do nothing under an overlay or in a text field", "[undoui][editor]") {
  Rig r;
  r.ed->rigController().edit([](Preset& p) { p.a.levelDb = -3.0; });
  r.settle();
  const std::string edited = r.now();
  REQUIRE(r.ed->rigController().canUndo());
  r.ed->setSettingsOpen(true);
  CHECK_FALSE(r.key('z', false));
  CHECK_FALSE(r.key('z', true));
  r.ed->closeAllOverlaysForTests();
  juce::TextEditor field;
  field.setReadOnly(true);
  field.setBounds(10, 10, 100, 24);
  r.ed->addAndMakeVisible(field);
  r.ed->setFocusProbeForTests([&] { return static_cast<juce::Component*>(&field); });
  CHECK_FALSE(r.key('z', false));
  r.ed->setFocusProbeForTests([] { return static_cast<juce::Component*>(nullptr); });
  r.ed->removeChildComponent(&field);
  CHECK(r.now() == edited);
  CHECK(r.proc.undoSteps() == 1);
  // Plain Z, or Shift+Z without Cmd, is not the chord.
  CHECK_FALSE(r.ed->keyPressed(juce::KeyPress('z', 0, 0)));
  CHECK_FALSE(r.ed->keyPressed(juce::KeyPress('z', juce::ModifierKeys::shiftModifier, 0)));
  // Redo under an overlay is refused too, after a real undo.
  CHECK(r.key('z', false));
  r.settle();
  r.ed->setSettingsOpen(true);
  CHECK_FALSE(r.key('z', true));
  CHECK(r.proc.redoSteps() == 1);
  r.ed->closeAllOverlaysForTests();
  juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  CHECK(r.key('z', true));
  r.settle();
  CHECK(r.now() == edited);
}

TEST_CASE("undo ui: the history survives closing and reopening the editor", "[undoui][editor]") {
  Rig r;
  const std::string before = r.now();
  r.ed->rigController().edit([](Preset& p) { p.a.blocks[0].bypass = true; });
  r.settle();
  const std::string after = r.now();
  drag(r.knob(paramSpec(kOutputGain).id), -40.0f);
  r.settle();
  REQUIRE(r.proc.undoSteps() == 2);
  r.close();
  CHECK(r.proc.undoSteps() == 2);  // processor-owned
  r.open();
  r.settle();
  CHECK(r.ed->rigController().canUndo());
  CHECK(r.key('z', false));
  r.settle();
  CHECK(r.now() == after);
  CHECK(r.key('z', false));
  r.settle();
  CHECK(r.now() == before);
  r.close();  // undone steps stay redoable across a close as well
  r.open();
  r.settle();
  CHECK(r.proc.redoSteps() == 2);
  CHECK(r.key('z', true));
  r.settle();
  CHECK(r.now() == after);
}

TEST_CASE("undo ui: closing the editor in the middle of a drag leaves one step, not a stuck gesture", "[undoui][editor]") {
  Rig r;
  auto& k = r.knob(paramSpec(kOutputGain).id);
  const juce::Point<float> start(15.0f, 15.0f);
  k.mouseDown(ev(k, start, start));
  k.mouseDrag(ev(k, {15.0f, -20.0f}, start));
  CHECK(r.proc.historyInGesture());
  r.close();
  CHECK_FALSE(r.proc.historyInGesture());
  CHECK(r.proc.undoSteps() == 1);
  CHECK(r.proc.undo());
}

namespace {
void wheel(skin::FilmstripKnob& k, float dy) {
  juce::MouseWheelDetails wd{};
  wd.deltaY = dy;
  // JUCE drops a wheel event whose eventTime equals the previous one: every notch gets a later time.
  static juce::int64 tick = juce::Time::currentTimeMillis();
  const auto now = juce::Time(++tick);
  const juce::Point<float> p(15.0f, 15.0f);
  k.mouseWheelMove(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &k, &k, now, p, now, 1, false), wd);
}
}  // namespace

TEST_CASE("undo ui: a burst of wheel notches is one step and one rebuild; notches apart are one step each", "[undoui][editor]") {
  Rig r;
  r.ed->setRigEditorOpen(true);
  r.settle();
  std::vector<rig::PresetKnob*> knobs;
  collect(*r.ed, knobs);
  // A debounced knob a wheel notch moves (a live knob applies every notch at once and has no pending edit).
  skin::FilmstripKnob* k = nullptr;
  for (auto* pk : knobs) {
    if (!pk->isVisible()) continue;
    const std::string before = r.now();
    wheel(pk->knob(), 0.2f);
    const bool debounced = r.ed->rigController().hasPending();
    r.ed->rigController().flushTimerForTests();
    r.settle();
    if (debounced && r.now() != before) {
      k = &pk->knob();
      break;
    }
  }
  REQUIRE(k != nullptr);
  auto& ctl = r.ed->rigController();
  r.proc.historyClear();
  const std::string before = r.now();
  const auto builds0 = r.proc.engineBuilds();
  for (int i = 0; i < 5; ++i) wheel(*k, 0.2f);  // five notches inside the debounce window
  CHECK(r.proc.undoSteps() == 0);                // nothing flushed per notch
  REQUIRE(ctl.flushTimerForTests());             // the timer fires once
  r.settle();
  CHECK(r.proc.undoSteps() == 1);
  CHECK(r.proc.engineBuilds() == builds0 + 1);   // one load
  CHECK(r.now() != before);
  CHECK(r.key('z', false));
  r.settle();
  CHECK(r.now() == before);
  // Notches apart (the timer fired between them): one step each.
  r.proc.historyClear();
  for (int i = 0; i < 3; ++i) {
    wheel(*k, 0.2f);
    REQUIRE(ctl.flushTimerForTests());
    r.settle();
  }
  CHECK(r.proc.undoSteps() == 3);
}
