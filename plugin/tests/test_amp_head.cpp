// v0.2 Task D: the amp controls on the amp heads of the rig page (rig::AmpHead), the capture tag on capture blocks, the
// GAIN read-out, the "no amp" / "body path off" states and Cmd / Ctrl + Z. Screenshots go to SAWBLADE_SCREENSHOT_DIR (default
// build/screenshots): rig_amp_defaults / _moved / _ladder / _bodyoff.png. Needs a display (the editor tests run under xvfb-run).
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "ExportPanel.h"
#include "about/AboutBox.h"
#include "pedals/AdvancedDrawer.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "rig/AmpHead.h"
#include "rig/RigEditorPanel.h"
#include "rig/SlotStrip.h"
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
template <class T>
std::vector<T*> findAll(juce::Component& root) {
  std::vector<T*> v;
  collect<T>(root, v);
  return v;
}

juce::MouseEvent ev(juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos) {
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, &c, &c, now, downPos, now, 1, true);
}
// A mouse drag of `dy` pixels (negative = up) on a knob: 250 px = the full range.
void drag(skin::FilmstripKnob& k, float dy) {
  const juce::Point<float> start(15.0f, 15.0f), end(15.0f, 15.0f + dy);
  k.mouseDown(ev(k, start, start));
  k.mouseDrag(ev(k, end, start));
  k.mouseUp(ev(k, end, start));
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

struct Dir {
  fs::path dir;
  Dir() {
    static int n = 0;
    dir = fs::temp_directory_path() / ("sawblade_amphead_" + std::to_string(std::random_device{}()) + "_" + std::to_string(n++));
    fs::create_directories(dir);
    fs::create_directories(dir / "cache" / "T1");
    setCaptureCacheRootOverride(dir / "cache");
  }
  ~Dir() {
    setCaptureCacheRootOverride(std::nullopt);
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

json nam(const std::string& id, bool ladder = false) {
  json model = {{"file", (kFx / "nam" / "linear_identity.nam").string()}};
  if (ladder) {
    model["source"] = {{"provider", "tone3000"}, {"id", "T1"}, {"modelId", "m1"}, {"title", "Marshall A"}};
    model["ladder"] = json::parse(R"([{"modelId":"m1","gain":2.0,"name":"Gain 2"},{"modelId":"m2","gain":5.0,"name":"Gain 5"},{"modelId":"m3","gain":8.0,"name":"Gain 8"}])");
  }
  return {{"id", id}, {"type", "nam"}, {"slot", "amp"}, {"model", model}};
}

// aAmp / bAmp: the path has an amp block; bOn: BLEND topology (path B enabled).
json rigJson(bool aAmp, bool bAmp, bool bOn, bool aLadder = false) {
  json a = {{"blocks", json::array()}}, b = {{"enabled", bOn}, {"blocks", json::array()}};
  if (aAmp) a["blocks"].push_back(nam("a1", aLadder));
  if (bAmp) b["blocks"].push_back(nam("b1"));
  return {{"schema", "sawblade.preset"}, {"version", 2}, {"name", "amp head"}, {"paths", {{"a", a}, {"b", b}}}, {"align", {{"mode", "off"}}},
          {"blend", bOn ? 0.5 : 0.0}, {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}};
}

struct Rig {
  SettingsEnv env{"{}", /*isolateHome=*/true};
  Dir dir;
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;

  Rig() {
    proc.prepareToPlay(48000.0, 512);
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  }
  ~Rig() { base.reset(); }
  void load(const json& j) {
    const fs::path f = dir.dir / "p.json";
    std::ofstream(f) << j.dump(2);
    REQUIRE(proc.loadPresetFile(f));
    REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
    REQUIRE(proc.status().error.empty());
    juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
    ed->refreshNow();
  }
  double param(int i) { return static_cast<double>(proc.parameters().getRawParameterValue(paramSpec(i).id)->load()); }
  void snap(const char* name) {
    ed->refreshNow();
    savePng(ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f), name);
  }
};

}  // namespace

TEST_CASE("amp head: six labelled knobs per head, bound to ampA_* / ampB_*, moved by mouse drags", "[ampd][editor]") {
  Rig rig;
  rig.load(rigJson(true, true, true));
  const char* const names[6] = {"GAIN", "BASS", "MID", "TREBLE", "PRESENCE", "LEVEL"};
  const char* const ids[2][6] = {{"ampA_gain", "ampA_bass", "ampA_mid", "ampA_treble", "ampA_presence", "ampA_level"},
                                 {"ampB_gain", "ampB_bass", "ampB_mid", "ampB_treble", "ampB_presence", "ampB_level"}};
  REQUIRE(findAll<rig::AmpHead>(*rig.ed).size() == 2);
  for (int path = 0; path < 2; ++path) {
    rig::AmpHead& head = rig.ed->ampHead(path);
    CHECK(head.knobsEnabled());
    for (int k = 0; k < kAmpKnobCount; ++k) {
      INFO(ids[path][k]);
      auto& knob = head.knob(k);
      CHECK(knob.paramId() == ids[path][k]);
      CHECK(std::string(rig::AmpHead::caption(k)) == names[k]);
      CHECK(knob.isVisible());
      CHECK(knob.getWidth() <= 34);  // the filmstrip is drawn smaller than the pedal knobs, never redrawn
      CHECK(knob.getWidth() > 0);
      const double before = rig.param(ampParam(path, k));
      CHECK(before == Catch::Approx(5.0));
      drag(knob, -50.0f);  // 50 px up = +20 % of the range = +2
      juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
      CHECK(rig.param(ampParam(path, k)) == Catch::Approx(7.0).margin(0.05));
      CHECK(knob.getValue() == Catch::Approx(7.0).margin(0.05));
      drag(knob, 100.0f);  // 100 px down = -4
      CHECK(rig.param(ampParam(path, k)) == Catch::Approx(3.0).margin(0.05));
    }
  }
  // The other path's knobs never moved with them (ids are per path).
  rig.proc.parameters().getParameter("ampA_gain")->setValueNotifyingHost(1.0f);
  CHECK(rig.param(ampParam(1, kAmpGain)) == Catch::Approx(3.0).margin(0.05));
  CHECK(rig.ed->ampHead(0).knob(kAmpGain).getValue() == Catch::Approx(10.0));
}

TEST_CASE("amp head: no amp in the path, and the body path off: knobs disabled with the reason", "[ampd][editor]") {
  Rig rig;
  rig.load(rigJson(false, false, false));  // no blocks at all, BLEND off
  CHECK(rig.ed->ampHead(0).readout() == "NO AMP IN THIS PATH");
  CHECK(rig.ed->ampHead(0).readout() == rig::AmpHead::noAmpText());
  CHECK_FALSE(rig.ed->ampHead(0).knobsEnabled());
  CHECK_FALSE(rig.ed->ampHead(0).knob(kAmpGain).isEnabled());
  CHECK(rig.ed->ampHead(1).readout() == juce::String::fromUTF8("BODY PATH OFF \xE2\x80\x94 turn up BLEND to add one"));
  CHECK(rig.ed->ampHead(1).readout() == rig::AmpHead::bodyOffText());
  for (int k = 0; k < kAmpKnobCount; ++k) CHECK_FALSE(rig.ed->ampHead(1).knob(k).isEnabled());

  rig.load(rigJson(true, false, true));  // BLEND on, path B has no amp
  CHECK(rig.ed->ampHead(0).knobsEnabled());
  CHECK(rig.ed->ampHead(0).readout() == "GAIN 5.0");
  CHECK_FALSE(rig.ed->ampHead(1).knobsEnabled());
  CHECK(rig.ed->ampHead(1).readout() == rig::AmpHead::bodyMissingText());  // B on, no amp, no fill running: says so, not "NO AMP"

  rig.load(rigJson(true, true, false));  // path B has an amp but BLEND is off: the body head is off, and "add one" would be false
  CHECK(rig.ed->ampHead(0).knobsEnabled());
  CHECK(rig.ed->ampHead(1).readout() == rig::AmpHead::bodyOffWithBlocksText());
  CHECK(rig.ed->ampHead(1).readout() == juce::String::fromUTF8("BODY PATH OFF \xE2\x80\x94 turn up BLEND"));
  CHECK(rig.ed->ampHead(1).readout() != rig::AmpHead::bodyOffText());
  CHECK_FALSE(rig.ed->ampHead(1).knobsEnabled());

  rig.load(rigJson(true, true, true));
  CHECK(rig.ed->ampHead(1).knobsEnabled());
  CHECK(rig.ed->ampHead(1).readout() == "GAIN 5.0");
}

TEST_CASE("amp head: the GAIN read-out in all three states", "[ampd][editor]") {
  using LI = SawbladeProcessor::LadderInfo;
  const juce::String dot = juce::String::fromUTF8(" \xC2\xB7 ");
  LI none;
  CHECK(rig::AmpHead::gainReadout(7.0, none) == "GAIN 7.0");
  CHECK(rig::AmpHead::gainReadout(5.04, none) == "GAIN 5.0");
  LI lad;
  lad.has = true;
  lad.rungCount = 3;
  lad.activeIndex = 1;
  lad.targetIndex = 1;
  lad.activeName = "Gain 6";
  lad.targetName = "Gain 6";
  CHECK(rig::AmpHead::gainReadout(7.0, lad) == "GAIN 7.0" + dot + "capture: Gain 6");
  LI pend = lad;
  pend.pending = true;
  pend.targetIndex = 2;
  pend.targetName = "Gain 8";
  CHECK(rig::AmpHead::gainReadout(7.0, pend) == "GAIN 7.0" + dot + "drive only (fetching Gain 8)");
  pend.targetName.clear();
  pend.targetModelId = "m3";
  CHECK(rig::AmpHead::gainReadout(7.0, pend) == "GAIN 7.0" + dot + "drive only (fetching m3)");

  // Through the head, with a fake ladderInfo.
  Rig rig;
  rig.load(rigJson(true, true, true));
  Preset p = rig.proc.editBasePreset();
  rig.ed->ampHead(0).refresh(p, lad);
  CHECK(rig.ed->ampHead(0).readout() == "GAIN 5.0" + dot + "capture: Gain 6");
  rig.ed->ampHead(0).refresh(p, pend);
  CHECK(rig.ed->ampHead(0).readout() == "GAIN 5.0" + dot + "drive only (fetching m3)");
  rig.ed->ampHead(0).refresh(p, none);
  CHECK(rig.ed->ampHead(0).readout() == "GAIN 5.0");
  drag(rig.ed->ampHead(0).knob(kAmpGain), -50.0f);
  rig.ed->ampHead(0).refresh(p, lad);
  CHECK(rig.ed->ampHead(0).readout() == "GAIN 7.0" + dot + "capture: Gain 6");
}

TEST_CASE("amp head: a real gain ladder shows its rung, and drive only while the rung is pending", "[ampd][editor]") {
  Rig rig;
  fs::copy_file(kFx / "nam" / "linear_identity.nam", rig.dir.dir / "cache" / "T1" / "m2.nam");  // m3 is not cached
  rig.load(rigJson(true, true, true, /*aLadder=*/true));
  const juce::String dot = juce::String::fromUTF8(" \xC2\xB7 ");
  CHECK(rig.ed->ampHead(0).readout() == "GAIN 5.0" + dot + "capture: Gain 2");
  CHECK(rig.ed->ampHead(1).readout() == "GAIN 5.0");  // path B has no ladder
  // GAIN to the top: the audio thread asks for rung m3, which is not loaded: drive only. (It needs the audio to run.)
  rig.proc.parameters().getParameter("ampA_gain")->setValueNotifyingHost(1.0f);
  juce::AudioBuffer<float> buf(2, 512);
  juce::MidiBuffer midi;
  for (int i = 0; i < 8; ++i) {
    buf.clear();
    rig.proc.processBlock(buf, midi);
  }
  rig.ed->refreshNow();
  CHECK(rig.ed->ampHead(0).readout() == "GAIN 10.0" + dot + "drive only (fetching Gain 8)");
  rig.snap("rig_amp_ladder.png");
}

TEST_CASE("amp head: capture blocks carry the CAPTURE tag, modeled pedals do not", "[ampd][editor]") {
  Rig rig;
  json j = rigJson(true, true, true);
  const json ts = {{"id", "a2"}, {"type", "pedal.ts"}, {"slot", "boost"}, {"modelVersion", 1}, {"params", {{"drive", 0}, {"tone", 5}, {"level", 8}}}};
  j["paths"]["a"]["blocks"].insert(j["paths"]["a"]["blocks"].begin(), ts);
  rig.load(j);
  rig.ed->setRigEditorOpen(true);
  rig.ed->rigEditor().refresh();
  CHECK(rig::SlotStrip::captureTag() == juce::String::fromUTF8("CAPTURE \xC2\xB7 FIXED TONE"));
  auto strips = findAll<rig::SlotStrip>(*rig.ed);
  REQUIRE(strips.size() >= 2);
  int tagged = 0, untagged = 0;
  for (auto* s : strips)
    for (int i = 0; i < s->numCards(); ++i) {
      auto* labels = &s->card(i);
      const auto texts = findAll<juce::Label>(*labels);
      bool hasTag = false;
      for (auto* l : texts) hasTag = hasTag || l->getText() == rig::SlotStrip::captureTag();
      CHECK(hasTag == s->cardShowsCaptureTag(i));
      (hasTag ? tagged : untagged)++;
    }
  CHECK(tagged == 2);    // the A and B amp captures
  CHECK(untagged == 1);  // the modeled TS pedal
}

TEST_CASE("amp head: Cmd / Ctrl + Z undoes the BLEND fill", "[ampd][editor]") {
  Rig rig;
  rig.load(rigJson(true, false, false));
  const juce::KeyPress undoKey('z', juce::ModifierKeys::commandModifier, 0);
  CHECK_FALSE(rig.ed->keyPressed(undoKey));  // nothing to undo
  const Preset pre = rig.proc.currentPreset();
  rig.ed->rigController().setTopology(rig::Topology::Blend);
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  CHECK(rig.proc.currentPreset().b.enabled);
  CHECK_FALSE(rig.ed->keyPressed(juce::KeyPress('z', juce::ModifierKeys::shiftModifier, 0)));  // not the undo chord
  CHECK(rig.ed->rigController().canUndo());
  CHECK(rig.ed->keyPressed(undoKey));
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  CHECK(rig.proc.currentPreset() == pre);
  CHECK_FALSE(rig.ed->keyPressed(undoKey));
}

TEST_CASE("amp head: Cmd / Ctrl + Z does not bubble into an undo from a focused text field or an open overlay", "[ampd][editor]") {
  Rig rig;
  rig.load(rigJson(true, false, false));
  const Preset pre = rig.proc.currentPreset();
  rig.ed->rigController().setTopology(rig::Topology::Blend);
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  const Preset filled = rig.proc.currentPreset();
  REQUIRE(rig.ed->rigController().canUndo());
  const juce::KeyPress undoKey('z', juce::ModifierKeys::commandModifier, 0);

  // The real path is: a key press goes to the focused component and bubbles up through its parents to the editor (ComponentPeer does
  // exactly this walk). A headless X server gives no window and so no focus: the walk is done here, and the focus is probed through
  // the editor's test hook.
  const auto bubble = [&](juce::Component& from) {
    for (juce::Component* c = &from; c != nullptr; c = c->getParentComponent())
      if (c->keyPressed(undoKey)) return true;
    return false;
  };

  // 1. A read-only text editor has the focus: it does not take Cmd+Z (it is read-only), so the key bubbles up: not an undo.
  juce::TextEditor field;
  field.setReadOnly(true);
  field.setText("read-only", false);
  field.setBounds(10, 10, 100, 24);
  rig.ed->addAndMakeVisible(field);
  rig.ed->setFocusProbeForTests([&] { return static_cast<juce::Component*>(&field); });
  CHECK_FALSE(field.keyPressed(undoKey));  // the field itself passes it on
  CHECK_FALSE(bubble(field));
  CHECK(rig.proc.currentPreset() == filled);
  CHECK(rig.ed->rigController().canUndo());
  rig.ed->setFocusProbeForTests([] { return static_cast<juce::Component*>(nullptr); });
  rig.ed->removeChildComponent(&field);

  // 2. An overlay is open: the editor does not undo underneath it, however the key arrives. Every overlay is checked on its own:
  // start from a verified "nothing open" state, open exactly that overlay, check the direct and the bubbled chord, close it again.
  const auto allClosed = [&] {
    return !rig.ed->settingsOpen() && !rig.ed->aboutOpen() && !rig.ed->browserOpen() && !rig.ed->matchScreenOpen() && !rig.ed->exportPanelOpen() &&
           !rig.ed->micPageOpen() && !rig.ed->playAlongOpen() && !rig.ed->captureBrowserOpen() && !rig.ed->advancedDrawerOpen() && !rig.ed->rigEditorOpen();
  };
  const auto closeAll = [&] {
    rig.ed->closeAllOverlaysForTests();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);  // the capture browser is released on the next loop turn
    REQUIRE(allClosed());
  };
  const auto check = [&](const char* name, const std::function<void()>& open, const std::function<bool()>& isOpen) {
    INFO(name);
    closeAll();
    open();
    REQUIRE(isOpen());
    CHECK_FALSE(rig.ed->keyPressed(undoKey));
    CHECK_FALSE(bubble(*rig.ed));
    CHECK(rig.proc.currentPreset() == filled);
    CHECK(rig.ed->rigController().canUndo());
    closeAll();
  };
  check("settings", [&] { rig.ed->setSettingsOpen(true); }, [&] { return rig.ed->settingsOpen(); });
  check("about", [&] {
          rig.ed->setSettingsOpen(true);
          for (auto* b : findAll<juce::Button>(*rig.ed))
            if (b->getTitle() == "About Sawblade...") {
              const auto c = b->getLocalBounds().toFloat().getCentre();
              juce::Component& comp = *b;
              comp.mouseDown(ev(comp, c, c));  // a mouse click, as the About test in test_editor.cpp does
              comp.mouseUp(ev(comp, c, c));
            }
          rig.ed->setSettingsOpen(false);  // the about box alone
        },
        [&] { return rig.ed->aboutOpen() && !rig.ed->settingsOpen(); });
  check("preset browser", [&] { rig.ed->setBrowserOpen(true); }, [&] { return rig.ed->browserOpen(); });
  check("capture browser", [&] { rig.ed->openCaptureBrowserForTests(); }, [&] { return rig.ed->captureBrowserOpen(); });
  check("advanced drawer", [&] { rig.ed->setAdvancedDrawerOpen(true); }, [&] { return rig.ed->advancedDrawerOpen(); });
  check("export panel", [&] { rig.ed->openExportPanel(); }, [&] { return rig.ed->exportPanelOpen(); });
  check("play-along panel", [&] { rig.ed->setPlayAlongOpen(true); }, [&] { return rig.ed->playAlongOpen(); });
  check("match screen", [&] { rig.ed->openMatchScreen(); }, [&] { return rig.ed->matchScreenOpen(); });
  check("mic page", [&] { rig.ed->setMicPageOpen(true); }, [&] { return rig.ed->micPageOpen(); });
  closeAll();

  // 3. The RIG editor is deliberately not an overlay for this purpose: with it open the chord undoes the fill (that is where path B is edited).
  rig.ed->setRigEditorOpen(true);
  REQUIRE(rig.ed->rigEditorOpen());
  CHECK(rig.ed->keyPressed(undoKey));
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  CHECK(rig.proc.currentPreset() == pre);
  CHECK_FALSE(rig.ed->rigController().canUndo());
  rig.ed->setRigEditorOpen(false);
  rig.ed->rigController().setTopology(rig::Topology::Blend);  // fill again for step 4
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  REQUIRE(rig.ed->rigController().canUndo());

  // 4. Nothing open and no text focus: it does undo.
  rig.ed->setRigEditorOpen(false);
  rig.ed->setMicPageOpen(false);
  rig.ed->setBrowserOpen(false);
  rig.ed->setSettingsOpen(false);
  rig.ed->setPlayAlongOpen(false);
  rig.ed->exportPanel().setVisible(false);
  rig.ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  CHECK(bubble(*rig.ed));
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  CHECK(rig.proc.currentPreset() == pre);
}

TEST_CASE("amp head: screenshots of the rig page", "[ampd][editor]") {
  Rig rig;
  rig.load(rigJson(true, true, true));
  rig.snap("rig_amp_defaults.png");
  // Knobs moved: SAW head up, BODY head scooped.
  const float dys[6] = {-70.0f, 40.0f, -60.0f, -80.0f, 20.0f, -30.0f};
  for (int k = 0; k < kAmpKnobCount; ++k) {
    drag(rig.ed->ampHead(0).knob(k), dys[k]);
    drag(rig.ed->ampHead(1).knob(k), -dys[k]);
  }
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  rig.snap("rig_amp_moved.png");
  rig.load(rigJson(true, false, false));  // body path off
  rig.snap("rig_amp_bodyoff.png");  // B empty, BLEND off
  CHECK(rig.ed->ampHead(1).readout() == rig::AmpHead::bodyOffText());
  rig.load(rigJson(true, true, false));
  rig.snap("rig_amp_bodyoff_blocks.png");  // B has blocks, BLEND off
  CHECK(rig.ed->ampHead(1).readout() == rig::AmpHead::bodyOffWithBlocksText());
}

// --- v0.3 Task E: the STEPS tag on the amp head ---------------------------------------------------------------------------------------------

namespace {
// SAWBLADE_NO_NETWORK is "1" in every test; a test that runs the (fake) ladder tool turns it off for its own duration.
struct NetworkOn {
  std::optional<std::string> old;
  NetworkOn() {
    if (const char* c = std::getenv("SAWBLADE_NO_NETWORK")) old = c;
    ::setenv("SAWBLADE_NO_NETWORK", "0", 1);
  }
  ~NetworkOn() {
    if (old) ::setenv("SAWBLADE_NO_NETWORK", old->c_str(), 1);
    else ::unsetenv("SAWBLADE_NO_NETWORK");
  }
};

json fiveRungs() {
  json l = json::array();
  for (int i = 1; i <= 5; ++i) l.push_back({{"modelId", "m" + std::to_string(i)}, {"gain", 2.0 * i}, {"name", "Gain " + std::to_string(2 * i)}});
  return l;
}

// Path A's amp is a TONE3000 capture T1 / m1 (a rung of fiveRungs()), with or without its stored ladder.
json capturePreset(bool withLadder) {
  json j = rigJson(true, false, false);
  json& model = j["paths"]["a"]["blocks"][0]["model"];
  model["source"] = {{"provider", "tone3000"}, {"id", "T1"}, {"modelId", "m1"}, {"title", "Marshall A"}};
  if (withLadder) model["ladder"] = fiveRungs();
  return j;
}

// A stand-in `sawblade-t3k` that prints `doc` for every call; the processor's settings point at it.
void useLadderTool(Rig& rig, const std::string& doc) {
  const fs::path exe = rig.dir.dir / "fake-t3k";
  std::ofstream(exe) << "#!/bin/sh\necho '" << doc << "'\n";
  fs::permissions(exe, fs::perms::owner_all);
  std::ofstream(rig.env.dir / "settings.json") << json{{"version", 1}, {"firstRunCompleted", true}, {"t3kExecutable", exe.string()}}.dump();
  settings::Settings::resetSharedForTests();
}

// Runs the ladder fetch to completion: the tool, then the tick that applies its answer.
void fetchLadder(Rig& rig) {
  rig.proc.ladderTick();
  REQUIRE(rig.proc.waitForLadderWork());
  rig.proc.ladderTick();
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
  rig.ed->refreshNow();
}
}  // namespace

TEST_CASE("amp head: the text of the STEPS tag", "[ampd][editor][steps]") {
  CHECK(rig::AmpHead::stepsText(5) == "STEPS 5");
  CHECK(rig::AmpHead::stepsText(2) == "STEPS 2");
  CHECK(rig::AmpHead::stepsText(0) == juce::String::fromUTF8("STEPS \xE2\x80\x94"));
  CHECK(rig::AmpHead::stepsText(-1).isEmpty());  // unknown: nothing
  CHECK(rig::AmpHead::stepsText(1).isEmpty());   // one rung is not a ladder
}

TEST_CASE("amp head: STEPS n for a capture with a ladder; nothing for a capture nobody has checked, or no capture source", "[ampd][editor][steps]") {
  Rig rig;
  rig.load(capturePreset(true));
  CHECK(rig.ed->ampHead(0).stepsTag() == "STEPS 5");
  CHECK(rig.ed->ampHead(1).stepsTag().isEmpty());  // no amp in path B: no tag, and the read-out says so
  rig.load(capturePreset(false));
  CHECK(rig.ed->ampHead(0).stepsTag().isEmpty());  // a capture, never checked: unknown
  rig.load(rigJson(true, false, false));            // an amp that is a local file (no TONE3000 source): never checked either
  CHECK(rig.ed->ampHead(0).stepsTag().isEmpty());
  rig.load(rigJson(true, true, true, /*aLadder=*/true));  // the 3-rung ladder of the older tests
  CHECK(rig.ed->ampHead(0).stepsTag() == "STEPS 3");
  CHECK(rig.ed->ampHead(1).stepsTag().isEmpty());
  rig.snap("rig_amp_steps.png");
}

TEST_CASE("amp head: STEPS appears when the ladder fetch completes, and survives an undo / redo", "[ampd][editor][steps]") {
  Rig rig;
  rig.load(capturePreset(false));
  REQUIRE(rig.ed->ampHead(0).stepsTag().isEmpty());  // unknown while nobody asked
  auto& ctl = rig.ed->rigController();
  ctl.edit([](Preset& p) { p.a.levelDb = -2.0; });   // a step whose snapshot (before it) has no ladder yet
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  const NetworkOn net;
  useLadderTool(rig, R"({"tone_id":"T1","size":"standard","rungs":[)" + [] {
    std::string r;
    for (int i = 1; i <= 5; ++i) r += std::string(i > 1 ? "," : "") + R"({"model_id":"m)" + std::to_string(i) + R"(","gain":)" + std::to_string(2 * i) + R"(.0,"name":"Gain )" + std::to_string(2 * i) + R"("})";
    return r;
  }() + "]}");
  rig.proc.ladderTick();  // starts the tool
  REQUIRE(rig.proc.waitForLadderWork());
  CHECK(rig.ed->ampHead(0).stepsTag().isEmpty());  // the answer is waiting for the tick: still nothing
  rig.proc.ladderTick();                           // applies it
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
  rig.ed->refreshNow();
  CHECK(rig.ed->ampHead(0).stepsTag() == "STEPS 5");
  // Undo to the snapshot from before the ladder, redo: the tag stays (the ladder was patched into the history).
  REQUIRE(ctl.undo());
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  rig.ed->refreshNow();
  CHECK(rig.proc.currentPreset().a.levelDb == Catch::Approx(0.0));
  CHECK(rig.ed->ampHead(0).stepsTag() == "STEPS 5");
  REQUIRE(ctl.redo());
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  rig.ed->refreshNow();
  CHECK(rig.ed->ampHead(0).stepsTag() == "STEPS 5");
}

TEST_CASE("amp head: STEPS - when the capture was checked and has no ladder; nothing when the tool fails", "[ampd][editor][steps]") {
  {
    Rig rig;
    rig.load(capturePreset(false));
    const NetworkOn net;
    useLadderTool(rig, R"({"tone_id":"T1","size":"standard","rungs":null})");
    CHECK(rig.ed->ampHead(0).stepsTag().isEmpty());
    fetchLadder(rig);
    CHECK(rig.proc.ladderSteps("T1") == 0);
    CHECK(rig.ed->ampHead(0).stepsTag() == juce::String::fromUTF8("STEPS \xE2\x80\x94"));
    CHECK(rig.ed->ampHead(0).readout() == "GAIN 5.0");  // the read-out is unchanged
    // Undo / redo keep it (the state is the session's knowledge of the tone, not part of a snapshot).
    rig.ed->rigController().edit([](Preset& p) { p.b.levelDb = -1.0; });
    REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
    REQUIRE(rig.ed->rigController().undo());
    REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
    rig.ed->refreshNow();
    CHECK(rig.ed->ampHead(0).stepsTag() == juce::String::fromUTF8("STEPS \xE2\x80\x94"));
    rig.snap("rig_amp_steps_none.png");
  }
  {
    Rig rig;  // a tool that fails: unknown, never "none"
    rig.load(capturePreset(false));
    const NetworkOn net;
    useLadderTool(rig, "nonsense");
    fetchLadder(rig);
    CHECK(rig.proc.ladderSteps("T1") == -1);
    CHECK(rig.ed->ampHead(0).stepsTag().isEmpty());
  }
}
