// v0.1.2 (docs/specs/v0_1_2-live_controls.md): the controls a player touches must change the sound.
//
// Everything runs at the level the user acts: a preset is loaded through PresetLoadFlow (what the preset browser calls),
// the processor renders tests/fixtures/di_riff.wav in 512-sample blocks at 44.1 kHz (Logic's usual block, forcing the
// 44.1 -> 48 kHz resampling path), and controls are moved through the editor's own components with mouse events
// (pedal-face knobs, the BLEND knob, EQ-graph nodes in the rig editor). Capture-backed presets load because the capture
// cache holds the repo's identity-NAM / impulse-IR stand-ins under the TONE3000 ids (never a real capture).
//
// Metric: relDiff(a, b) = RMS(a - b) / max(RMS(a), RMS(b)) over a 1 s window of the rendered DI (after a 0.3 s warm-up).
// "Differs" means relDiff > kMinRelDiff (0.05, about -26 dB: far above numerical noise, which is < 1e-4, and well below
// what any audible control change produces).
// Needs a display (the editor tests run under xvfb-run).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "pedals/CircuitFaces.h"
#include "pedals/PedalFace.h"
#include "presets/PresetLoadFlow.h"
#include "rig/EqGraph.h"
#include "rig/RigController.h"
#include "rig/RigEditorPanel.h"
#include "rig/RigModel.h"
#include "rig/RigWidgets.h"
#include "skin/FilmstripKnob.h"
#include "sawblade/preset.h"
#include "sawblade/wav_io.h"

using namespace sawblade;
using namespace sawblade::plugin;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kMinRelDiff = 0.05;
constexpr double kRate = 44100.0;
constexpr int kBlock = 512;

const fs::path kPresets = SAWBLADE_PRESETS_DIR;
const fs::path kFix = SAWBLADE_FIXTURES_DIR;
const fs::path kBuzzsaw = kPresets / "modeled" / "chainsaw" / "classic_buzzsaw.json";
const fs::path kBolt = kPresets / "matched" / "bolt_thrower_v1.json";

template <class T>
void collectAll(juce::Component& c, std::vector<T*>& out) {
  for (auto* child : c.getChildren()) {
    if (auto* t = dynamic_cast<T*>(child)) out.push_back(t);
    collectAll(*child, out);
  }
}
template <class T>
std::vector<T*> allOf(juce::Component& root) {
  std::vector<T*> v;
  collectAll<T>(root, v);
  return v;
}

juce::MouseEvent mouseAt(juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos) {
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, &c, &c, now, downPos, now, 1, true);
}

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

// Puts a stand-in for every TONE3000 capture a preset names into the capture cache (as `sawblade-t3k resolve` would).
void populateCache(const json& node, const fs::path& cache) {
  if (node.is_object()) {
    if (node.contains("file") && node["file"].is_string() && node.contains("source") && node["source"].is_object()) {
      const auto& s = node["source"];
      if (s.value("provider", "") == "tone3000" && s.contains("id") && s.contains("modelId")) {
        const bool ir = fs::path(node["file"].get<std::string>()).extension() == ".wav";
        const fs::path dest = cache / s["id"].get<std::string>() / (s["modelId"].get<std::string>() + (ir ? ".wav" : ".nam"));
        fs::create_directories(dest.parent_path());
        fs::copy_file(kFix / (ir ? "ir/impulse.wav" : "nam/linear_identity.nam"), dest, fs::copy_options::skip_existing);
      }
    }
    for (const auto& kv : node.items()) populateCache(kv.value(), cache);
  } else if (node.is_array()) {
    for (const auto& v : node) populateCache(v, cache);
  }
}

double rms(const std::vector<float>& v) {
  double s = 0.0;
  for (float x : v) s += static_cast<double>(x) * x;
  return v.empty() ? 0.0 : std::sqrt(s / static_cast<double>(v.size()));
}
double relDiff(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  std::vector<float> d(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i];
  return rms(d) / std::max({rms(a), rms(b), 1e-9});
}

struct World {
  std::unique_ptr<SettingsEnv> envOwner;  // null for a second instance sharing the first one's environment
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;
  std::vector<float> di;
  juce::AudioBuffer<float> buf{2, kBlock};
  juce::MidiBuffer midi;

  explicit World(bool ownEnv = true) {
    if (ownEnv) envOwner = std::make_unique<SettingsEnv>("{}", /*isolateHome=*/true);
    for (const auto* p : {&kBolt}) populateCache(json::parse(std::ifstream(*p)), captureCacheRoot());
    const AudioFile f = readWav(kFix / "di_riff.wav");
    REQUIRE(f.channels == 1);
    di = f.interleaved;
    proc.prepareToPlay(kRate, kBlock);
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  }
  ~World() { base.reset(); }

  // The preset browser's load path.
  void browserLoad(const fs::path& file, bool waitLevel = true) {
    bool finished = false;
    PresetLoadFlow::Callbacks cb;
    cb.onFinished = [&](const PresetLoadFlow::Outcome& o) {
      finished = true;
      INFO(o.message);
      REQUIRE(o.status == PresetLoadFlow::Outcome::Status::Loaded);
    };
    PresetLoadFlow flow(proc, cb, [](std::function<void()> fn) { fn(); });
    REQUIRE(flow.load(file, SubBank::Matched));
    REQUIRE(finished);
    REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
    REQUIRE(proc.status().error.empty());
    pump(80);  // attachments deliver the loaded parameters to the controls
    if (waitLevel) levelSettle();
  }
  // LEVEL MATCH (v0.3 Task B) measures the rig's trim on a worker thread, 400 ms after the last change: a render taken before it lands
  // differs from one taken after (a slow machine lets it land between two renders). Every settle waits for it, so the renders compare
  // a settled rig. The trim is a per-instance value: another instance's edit never starts or changes it (checked in the isolation test).
  void levelSettle() {
    REQUIRE(proc.waitForLevelWork(std::chrono::milliseconds(60000)));
    pump(30);
  }
  void loadInit() {
    proc.loadPreset(makeInitPreset());
    REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
    pump(80);
  }

  void block(const float* in, float* out0) {
    std::copy(in, in + kBlock, buf.getWritePointer(0));
    std::copy(in, in + kBlock, buf.getWritePointer(1));
    proc.processBlock(buf, midi);
    std::copy(buf.getReadPointer(0), buf.getReadPointer(0) + kBlock, out0);
  }
  // Feeds the DI from its start (looped); returns the 1 s window after a 0.3 s warm-up. `between(blockIndex)` runs between blocks.
  std::vector<float> render(const std::function<void(int)>& between = {}) {
    const int warm = static_cast<int>(0.3 * kRate) / kBlock, meas = static_cast<int>(kRate) / kBlock;
    std::vector<float> out, in(static_cast<std::size_t>(kBlock)), o(static_cast<std::size_t>(kBlock));
    std::size_t pos = 0;
    for (int b = 0; b < warm + meas; ++b) {
      if (between) between(b);
      for (auto& x : in) {
        x = di[pos];
        pos = (pos + 1) % di.size();
      }
      block(in.data(), o.data());
      if (b >= warm) out.insert(out.end(), o.begin(), o.end());
    }
    for (float x : out) REQUIRE(std::isfinite(x));
    return out;
  }
  void settle() {
    REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
    pump(60);
    levelSettle();
  }
};

skin::FilmstripKnob& knobByParam(juce::Component& root, const juce::String& id) {
  auto knobs = allOf<skin::FilmstripKnob>(root);
  auto it = std::find_if(knobs.begin(), knobs.end(), [&](auto* k) { return k->paramId() == id; });
  REQUIRE(it != knobs.end());
  return **it;
}

// A drag of `dy` pixels (negative = up) on a knob, with real mouse events: 250 px = the full range.
void dragKnob(skin::FilmstripKnob& k, float dy) {
  const juce::Point<float> start(40.0f, 60.0f), end(40.0f, 60.0f + dy);
  k.mouseDown(mouseAt(k, start, start));
  k.mouseDrag(mouseAt(k, end, start));
  k.mouseUp(mouseAt(k, end, start));
}

// Drags a knob to the far end of its range away from where it is (so the change is as large as possible).
void moveKnobFar(skin::FilmstripKnob& k) { dragKnob(k, k.proportion() > 0.5 ? 300.0f : -300.0f); }

void dragNode(rig::EqGraph& g, int band, float dx, float dy) {
  const juce::Point<float> n = g.nodePosition(band), to(n.x + dx, n.y + dy);
  juce::Component& c = g;
  c.mouseDown(mouseAt(g, n, n));
  c.mouseDrag(mouseAt(g, to, n));
  c.mouseUp(mouseAt(g, to, n));
}

}  // namespace

TEST_CASE("live controls: each preset loads through the browser flow and no longer sounds like INIT", "[editor][live]") {
  World w;
  w.loadInit();
  const auto init = w.render();
  for (const auto& file : {kBuzzsaw, kBolt}) {
    INFO(file.string());
    w.browserLoad(file);
    CHECK(w.proc.getName().isNotEmpty());
    const auto loaded = w.render();
    const double d = relDiff(init, loaded);
    INFO("relDiff vs INIT = " << d);
    CHECK(d > kMinRelDiff);
    w.loadInit();
  }
}

TEST_CASE("live controls: classic_buzzsaw pedal knobs, moved on the pedal face, change the sound", "[editor][live]") {
  World w;
  w.browserLoad(kBuzzsaw);
  auto faces = allOf<PedalFace>(*w.ed);
  REQUIRE(faces.size() == 1);
  PedalFace& face = *faces[0];
  face.refresh();
  const auto slot = w.proc.circuitSlot();
  REQUIRE(slot.has_value());
  const CircuitFace& cf = circuitFace(slot->circuit);
  int tested = 0;
  for (int k = 0; k < 6; ++k) {
    if (cf.knobs[static_cast<std::size_t>(k)].param < 0) continue;
    // a fresh load per knob, so every knob starts from the preset and is judged alone
    w.browserLoad(kBuzzsaw);
    face.refresh();
    skin::FilmstripKnob* knob = face.knob(slot->circuit, k);
    REQUIRE(knob != nullptr);
    const std::string id = paramSpec(cf.knobs[static_cast<std::size_t>(k)].param).id;
    INFO("knob " << k << " (" << id << ")");
    const auto before = w.render();
    const double p0 = knob->proportion();
    moveKnobFar(*knob);
    CHECK(std::abs(knob->proportion() - p0) > 0.2);  // the drag moved the knob
    const double paramNow = static_cast<double>(w.proc.parameters().getRawParameterValue(id)->load());
    CHECK(paramNow == knob->getValue());  // ... and the host parameter followed
    const auto after = w.render();
    const double d = relDiff(before, after);
    INFO("relDiff = " << d);
    CHECK(d > kMinRelDiff);
    ++tested;
  }
  CHECK(tested >= 3);
}

TEST_CASE("live controls: bolt_thrower_v1 BLEND, moved on the main panel, changes the sound", "[editor][live]") {
  World w;
  w.browserLoad(kBolt);
  auto& blend = knobByParam(*w.ed, "blend");
  REQUIRE(blend.isEnabled());
  // sweep 0 -> 1 in steps; every step must differ from the previous one by > kMinRelDiff / 2 and the ends by > kMinRelDiff
  blend.setValue(blend.proportionOfLengthToValue(0.0), juce::sendNotificationSync);
  const auto at0 = w.render();
  dragKnob(blend, -250.0f);  // full range up
  CHECK(blend.proportion() > 0.95);
  CHECK(static_cast<double>(w.proc.parameters().getRawParameterValue("blend")->load()) > 0.95);
  const auto at1 = w.render();
  const double d = relDiff(at0, at1);
  INFO("relDiff BLEND 0 -> 1 = " << d);
  CHECK(d > kMinRelDiff);
}

TEST_CASE("live controls: bolt_thrower_v1 EQ-graph node drag in the rig editor changes the sound", "[editor][live]") {
  World w;
  w.browserLoad(kBolt);
  w.ed->setRigEditorOpen(true);
  auto& panel = w.ed->rigEditor();
  panel.setTab(rig::RigEditorPanel::Tab::Eq);
  for (rig::EqTarget t : {rig::EqTarget::Post, rig::EqTarget::EqA, rig::EqTarget::EqB}) {
    INFO("EQ target " << static_cast<int>(t));
    w.browserLoad(kBolt);
    panel.setEqTarget(t);
    panel.refresh();
    rig::EqGraph& g = panel.eqGraph();
    int node = -1;
    for (int i = 0; i < g.numBands(); ++i)
      if (g.band(i).type == EqType::Peak) {
        node = i;
        break;
      }
    REQUIRE(node >= 0);
    const auto before = w.render();
    const double g0 = g.band(node).gainDb;
    dragNode(g, node, 0.0f, g.nodePosition(node).y > g.getHeight() / 2 ? -80.0f : 80.0f);  // +-9 dB-ish
    w.settle();
    panel.refresh();
    CHECK(std::abs(g.band(node).gainDb - g0) > 3.0);
    const auto after = w.render();
    const double d = relDiff(before, after);
    INFO("relDiff = " << d);
    CHECK(d > kMinRelDiff);
  }
}

TEST_CASE("live controls: Logic-style, host parameter changes from another thread between blocks", "[editor][live]") {
  World w;
  w.browserLoad(kBolt);
  auto setFromThread = [&](const std::string& id, float norm) {
    std::thread t([&] { w.proc.parameters().getParameter(id)->setValueNotifyingHost(norm); });
    t.join();
  };
  setFromThread("blend", 0.0f);
  const auto at0 = w.render();
  // the change arrives mid-render, between two blocks
  const auto swept = w.render([&](int b) {
    if (b == 20) setFromThread("blend", 1.0f);
  });
  const auto at1 = w.render();
  INFO("relDiff 0 vs 1 = " << relDiff(at0, at1));
  CHECK(relDiff(at0, at1) > kMinRelDiff);
  // the window that contains the change differs from the pure-0 one, and ends like the pure-1 one
  CHECK(relDiff(at0, swept) > kMinRelDiff);
  // other host-visible controls (output gain, post-EQ slot) are honoured the same way
  setFromThread("outputGain", 0.2f);
  const auto quieter = w.render();
  CHECK(relDiff(at1, quieter) > kMinRelDiff);
  setFromThread("postEq1", 0.9f);
  const auto eqd = w.render();
  CHECK(relDiff(quieter, eqd) > kMinRelDiff);

  // buzzsaw: pedal parameters from another thread
  w.browserLoad(kBuzzsaw);
  const auto slot = w.proc.circuitSlot();
  REQUIRE(slot.has_value());
  const CircuitFace& cf = circuitFace(slot->circuit);
  int pk = -1;
  for (int k = 0; k < 6 && pk < 0; ++k)
    if (cf.knobs[static_cast<std::size_t>(k)].param >= 0) pk = cf.knobs[static_cast<std::size_t>(k)].param;
  REQUIRE(pk >= 0);
  const auto a = w.render();
  const auto* prm = w.proc.parameters().getParameter(paramSpec(pk).id);
  setFromThread(paramSpec(pk).id, prm->getValue() > 0.5f ? 0.1f : 0.9f);
  const auto b = w.render();
  INFO("pedal param " << paramSpec(pk).id << " relDiff = " << relDiff(a, b));
  CHECK(relDiff(a, b) > kMinRelDiff);
}

// Two instances of the plugin in one process (two Logic tracks): editing one must never touch the other. Guards against shared
// static / singleton state (settings, preset library, loader, caches). "Unchanged" = relDiff < kSameRelDiff against the
// instance's own earlier render of the same input (numerical noise is ~1e-7; 1e-3 is still 60 dB below kMinRelDiff).
TEST_CASE("live controls: two instances in one process do not affect each other", "[editor][live][isolation]") {
  constexpr double kSameRelDiff = 1e-3;
  World a;                 // bolt_thrower_v1: BLEND + EQ
  World b(/*ownEnv=*/false);  // classic_buzzsaw: pedal knobs
  a.browserLoad(kBolt);
  b.browserLoad(kBuzzsaw);
  const auto a0 = a.render();
  const auto b0 = b.render();
  const auto aState = a.proc.engineParamState();
  const auto bState = b.proc.engineParamState();
  REQUIRE(aState.valid);
  REQUIRE(bState.valid);
  // the control: untouched instances reproduce their own render
  CHECK(relDiff(a0, a.render()) < kSameRelDiff);
  CHECK(relDiff(b0, b.render()) < kSameRelDiff);

  // through A's editor: BLEND sweep and an EQ node drag
  auto& blend = knobByParam(*a.ed, "blend");
  blend.setValue(blend.proportionOfLengthToValue(0.0), juce::sendNotificationSync);
  dragKnob(blend, -250.0f);
  a.ed->setRigEditorOpen(true);
  a.ed->rigEditor().setTab(rig::RigEditorPanel::Tab::Eq);
  a.ed->rigEditor().setEqTarget(rig::EqTarget::Post);
  a.ed->rigEditor().refresh();
  rig::EqGraph& g = a.ed->rigEditor().eqGraph();
  dragNode(g, 0, 0.0f, g.nodePosition(0).y > g.getHeight() / 2 ? -80.0f : 80.0f);
  a.settle();
  b.settle();
  const auto a1 = a.render();
  CHECK(relDiff(a0, a1) > kMinRelDiff);  // A did change
  CHECK(relDiff(b0, b.render()) < kSameRelDiff);
  CHECK(b.proc.engineParamState().live == bState.live);
  CHECK(b.proc.currentPreset().name == "Classic Buzzsaw");

  // through B's editor: a pedal knob
  auto faces = allOf<PedalFace>(*b.ed);
  REQUIRE(faces.size() == 1);
  const auto slot = b.proc.circuitSlot();
  REQUIRE(slot.has_value());
  const CircuitFace& cf = circuitFace(slot->circuit);
  int k = 0;
  while (cf.knobs[static_cast<std::size_t>(k)].param < 0) ++k;
  skin::FilmstripKnob* knob = faces[0]->knob(slot->circuit, k);
  REQUIRE(knob != nullptr);
  const auto aStatus = a.proc.status();
  REQUIRE_FALSE(aStatus.levelPending);  // A is settled: its trim is measured
  moveKnobFar(*knob);
  b.settle();
  a.settle();
  {  // B's edit never changes A's trim, nor starts a measurement for A
    const auto now = a.proc.status();
    CHECK(now.trimDb == aStatus.trimDb);
    CHECK_FALSE(now.levelPending);
  }
  const auto b1 = b.render();
  CHECK(relDiff(b0, b1) > kMinRelDiff);  // B did change
  CHECK(relDiff(a1, a.render()) < kSameRelDiff);
  CHECK(a.proc.currentPreset().name == "UK Death Fuzz Blend (match v1)");
}

// Root cause of a macOS CI failure of the isolation test: an instance's OWN pending level-match measurement (Task B) landed between two
// renders of the same instance, so its output changed with no other instance involved. This is that mechanism, made deterministic with the
// debounce: a render taken while the measurement is pending differs from one taken after it lands (which is why settle() now waits for it).
TEST_CASE("live controls: an instance's own pending level measurement changes its render when it lands", "[editor][live][levelmatch]") {
  World a;
  a.proc.setLevelDebounceMs(60000);  // the measurement of the loaded rig waits (a slow machine)
  a.browserLoad(kBolt, /*waitLevel=*/false);
  pump(600);
  REQUIRE(a.proc.status().levelPending);
  const double trimBefore = a.proc.status().trimDb;
  const auto before = a.render();
  a.proc.setLevelDebounceMs(0);  // the debounce elapses
  REQUIRE(a.proc.waitForLevelWork(std::chrono::milliseconds(60000)));
  CHECK_FALSE(a.proc.status().levelPending);
  const auto after = a.render();
  INFO("trim " << trimBefore << " -> " << a.proc.status().trimDb << ", relDiff " << relDiff(before, after));
  CHECK(a.proc.status().trimDb != trimBefore);
  CHECK(relDiff(before, after) > kMinRelDiff);  // no other instance involved
}

// ---------------------------------------------------------------------------------------------------------------------
// v0.3 Task A: the rig editor's preset knobs (PresetKnob) drag like the main-page knobs. Mouse-driven, in many small steps
// with the editor's refresh tick running in between (what the 16 Hz editor timer does while a hand is on the knob).
namespace {

// Visible up to (not including) `root`: the test editor is not on the desktop, so the top levels do not count.
bool visibleBelow(const juce::Component& c, const juce::Component& root) {
  for (const juce::Component* p = &c; p != nullptr && p != &root; p = p->getParentComponent())
    if (!p->isVisible()) return false;
  return true;
}

std::vector<rig::PresetKnob*> shownPresetKnobs(juce::Component& root) {
  std::vector<rig::PresetKnob*> v;
  for (auto* k : allOf<rig::PresetKnob>(root))
    if (visibleBelow(*k, root)) v.push_back(k);
  return v;
}

using Reader = std::function<double(const Preset&)>;

// What each shown PresetKnob edits, in the order the page creates them.
std::vector<Reader> readersFor(rig::RigEditorPanel::Tab tab, const Preset& p) {
  using Tab = rig::RigEditorPanel::Tab;
  std::vector<Reader> r;
  if (tab == Tab::Gate) {
    for (rig::GateField f : {rig::GateField::Hysteresis, rig::GateField::Attack, rig::GateField::Hold, rig::GateField::Release, rig::GateField::Range,
                             rig::GateField::Ratio, rig::GateField::KeyHpf})
      r.push_back([f](const Preset& q) { return rig::gateField(q.gate, f); });
  } else if (tab == Tab::Comp) {
    for (rig::CompField f : {rig::CompField::Threshold, rig::CompField::Ratio, rig::CompField::Knee, rig::CompField::Attack, rig::CompField::Release,
                             rig::CompField::Makeup})
      r.push_back([f](const Preset& q) { return rig::compField(q.busComp, f); });
  } else if (tab == Tab::Chain) {
    for (int path = 0; path < 2; ++path)
      for (const Block& b : (path == 0 ? p.a : p.b).blocks)
        if (dynamic_cast<const NamBlockParams*>(b.params.get()) != nullptr)
          r.push_back([path, id = b.id](const Preset& q) {
            for (const Block& c : (path == 0 ? q.a : q.b).blocks)
              if (c.id == id) return static_cast<const NamBlockParams*>(c.params.get())->inputGainDb;
            return -999.0;
          });
  }
  return r;
}

juce::MouseEvent mouseAtMods(juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, juce::ModifierKeys mods, int clicks = 1) {
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, downPos, now, clicks, true);
}

// A press-and-drag on a knob driven one event at a time.
struct Hand {
  skin::FilmstripKnob& k;
  juce::Point<float> start{40.0f, 60.0f};
  explicit Hand(skin::FilmstripKnob& knob, juce::ModifierKeys mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier)) : k(knob), mods_(mods) {
    k.mouseDown(mouseAtMods(k, start, start, mods_));
  }
  void to(float dy) { k.mouseDrag(mouseAtMods(k, {start.x, start.y + dy}, start, mods_)); }
  void up(float dy) { k.mouseUp(mouseAtMods(k, {start.x, start.y + dy}, start, mods_)); }

 private:
  juce::ModifierKeys mods_;
};

struct RigKnobWorld : World {
  rig::RigEditorPanel* panel = nullptr;
  RigKnobWorld() {
    browserLoad(kBolt);
    ed->setRigEditorOpen(true);
    panel = &ed->rigEditor();
    // RATIO only works in EXPANDER mode (otherwise the whole knob is disabled)
    panel->controller().edit([](Preset& p) { rig::setGateMode(p, GateMode::Expander); });
    settle();
  }
  void tick() {
    panel->refresh();
    pump(5);
  }
  // The model as the editor shows it (applied edits and the ones still waiting for their debounce) ...
  double model(const Reader& r) { return r(panel->controller().view()); }
  // ... and as it has been handed to the processor (pending debounced edits not included).
  double applied(const Reader& r) { return r(proc.editBasePreset()); }
};

constexpr double kTol = 1e-6;

}  // namespace

TEST_CASE("rig knobs: every PresetKnob sweeps min to max in a 250 px drag, a refresh tick never moves it, and the model ends equal to the knob",
          "[editor][live][rigknobs]") {
  using Tab = rig::RigEditorPanel::Tab;
  RigKnobWorld w;
  int tested = 0;
  for (Tab tab : {Tab::Chain, Tab::Gate, Tab::Comp}) {
    w.panel->setTab(tab);
    w.settle();
    w.panel->refresh();
    const auto readers = readersFor(tab, w.panel->controller().view());
    REQUIRE(shownPresetKnobs(*w.panel).size() == readers.size());
    if (tab == Tab::Chain) {  // an INPUT knob on both paths
      int namCount[2] = {0, 0};
      const Preset v = w.panel->controller().view();
      for (int path = 0; path < 2; ++path)
        for (const Block& b : (path == 0 ? v.a : v.b).blocks)
          if (dynamic_cast<const NamBlockParams*>(b.params.get()) != nullptr) ++namCount[path];
      CHECK(namCount[0] >= 1);
      CHECK(namCount[1] >= 1);
      CHECK(readers.size() == static_cast<std::size_t>(namCount[0] + namCount[1]));
    }
    for (std::size_t i = 0; i < readers.size(); ++i) {
      w.panel->setTab(tab);
      w.panel->refresh();
      auto knobs = shownPresetKnobs(*w.panel);
      REQUIRE(knobs.size() == readers.size());
      rig::PresetKnob& pk = *knobs[i];
      INFO("tab " << static_cast<int>(tab) << " knob " << i << " (" << pk.knob().getTitle() << ")");
      juce::Component::SafePointer<rig::PresetKnob> alive(&pk);
      skin::FilmstripKnob& k = pk.knob();
      REQUIRE(k.isEnabled());
      k.setValue(k.getMinimum(), juce::dontSendNotification);

      int gestures = 0;
      pk.onGestureBegin = [&] { ++gestures; };
      pk.onGestureEnd = [&] { ++gestures; };

      Hand hand(k);
      float dy = 0.0f;
      for (int step = 0; step < 25; ++step) {
        dy -= 10.0f;  // up = increase; 25 x 10 px = 250 px
        hand.to(dy);
        const double before = k.getValue();
        if (step % 2 == 1) w.tick();  // the editor's refresh while the button is down
        REQUIRE(alive != nullptr);    // the refresh did not rebuild the control under the hand
        CHECK(k.getValue() == before);  // ... and did not write the model back into it
        if (step == 12) {  // a pause mid-drag: the debounce fires and the rebuild lands; the knob still does not move
          w.panel->controller().flushTimerForTests();  // the debounce / throttle timer, fired explicitly (no wall-clock wait)
          w.proc.waitForLoader(std::chrono::milliseconds(60000));
          const double v = k.getValue();
          w.tick();
          REQUIRE(alive != nullptr);
          CHECK(k.getValue() == v);
        }
      }
      CHECK(k.proportion() > 0.999);
      hand.up(dy);
      w.settle();
      REQUIRE(alive != nullptr);
      CHECK(w.proc.waitForLoader(std::chrono::milliseconds(60000)));
      CHECK(std::abs(k.getValue() - k.getMaximum()) < kTol * (k.getMaximum() - k.getMinimum()));
      CHECK(std::abs(w.model(readers[i]) - k.getValue()) < kTol * (k.getMaximum() - k.getMinimum()));
      w.tick();
      CHECK(std::abs(k.getValue() - k.getMaximum()) < kTol * (k.getMaximum() - k.getMinimum()));  // still there after the next refresh
      CHECK(gestures == 2);  // one begin, one end
      ++tested;
    }
  }
  CHECK(tested >= 14);  // 7 gate + 6 comp + at least one INPUT
}

TEST_CASE("rig knobs: while a structural knob is dragged the model follows on a debounce, and mouse-up applies the final value once",
          "[editor][live][rigknobs]") {
  RigKnobWorld w;
  w.panel->setTab(rig::RigEditorPanel::Tab::Comp);
  w.panel->refresh();
  auto knobs = shownPresetKnobs(*w.panel);
  REQUIRE(knobs.size() == 6);
  rig::PresetKnob& pk = *knobs[5];  // MAKEUP -24..48
  const Reader makeup = [](const Preset& p) { return rig::compField(p.busComp, rig::CompField::Makeup); };
  skin::FilmstripKnob& k = pk.knob();
  const double m0 = w.applied(makeup);
  Hand hand(k);
  hand.to(-50.0f);
  CHECK(w.applied(makeup) == m0);  // nothing handed over yet: the debounce is running
  w.panel->controller().flushTimerForTests();  // the debounce / throttle timer, fired explicitly (no wall-clock wait)
  w.settle();
  const double mid = w.applied(makeup);
  CHECK(mid != m0);  // applied during the drag, on the debounce
  CHECK(std::abs(mid - k.getValue()) < kTol * 72.0);
  hand.to(-120.0f);
  hand.up(-120.0f);
  CHECK_FALSE(w.panel->controller().hasPending());  // mouse-up flushed it
  CHECK(std::abs(w.applied(makeup) - k.getValue()) < kTol * 72.0);  // already handed to the processor, no further wait
  w.settle();
  CHECK(std::abs(w.model(makeup) - k.getValue()) < kTol * 72.0);
  CHECK(k.getValue() > mid);
}

TEST_CASE("rig knobs: a PresetKnob drags like a main-page knob (shift = fine, double-click resets, wheel works)", "[editor][live][rigknobs]") {
  RigKnobWorld w;
  w.panel->setTab(rig::RigEditorPanel::Tab::Gate);
  w.panel->refresh();
  auto knobs = shownPresetKnobs(*w.panel);
  REQUIRE(knobs.size() == 7);
  skin::FilmstripKnob& k = knobs[4]->knob();  // RANGE -120..0 dB, linear
  k.setValue(-60.0, juce::dontSendNotification);
  {  // shift: a full 250 px moves a tenth of the range
    Hand hand(k, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier));
    hand.to(-250.0f);
    hand.up(-250.0f);
    CHECK(k.getValue() == Catch::Approx(-60.0 + 12.0).margin(0.01));
  }
  w.settle();
  k.mouseDoubleClick(mouseAtMods(k, {40.0f, 60.0f}, {40.0f, 60.0f}, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 2));
  CHECK(k.getValue() == -90.0);  // the knob's default
  w.panel->controller().flushTimerForTests();  // the debounce / throttle timer, fired explicitly (no wall-clock wait)
  w.settle();
  CHECK(w.model([](const Preset& p) { return rig::gateField(p.gate, rig::GateField::Range); }) == -90.0);
  const double before = k.getValue();
  juce::MouseWheelDetails wd{};
  wd.deltaY = 0.2f;
  wd.isReversed = false;
  wd.isSmooth = false;
  wd.isInertial = false;
  k.mouseWheelMove(mouseAtMods(k, {40.0f, 60.0f}, {40.0f, 60.0f}, juce::ModifierKeys()), wd);
  CHECK(k.getValue() > before);
  w.panel->controller().flushTimerForTests();  // the debounce / throttle timer, fired explicitly (no wall-clock wait)
  w.settle();
  CHECK(std::abs(w.model([](const Preset& p) { return rig::gateField(p.gate, rig::GateField::Range); }) - k.getValue()) < 1e-6);
}

TEST_CASE("rig knobs: a continuous drag applies on a throttle (not only when the hand pauses), mouse-up leaves nothing pending",
          "[editor][live][rigknobs]") {
  // Deterministic: the throttle timer is fired through a test hook (one "150 ms interval" every 5 hand events), never waited for.
  RigKnobWorld w;
  w.panel->setTab(rig::RigEditorPanel::Tab::Comp);
  w.panel->refresh();
  auto knobs = shownPresetKnobs(*w.panel);
  REQUIRE(knobs.size() == 6);
  skin::FilmstripKnob& k = knobs[5]->knob();  // MAKEUP
  const Reader makeup = [](const Preset& p) { return rig::compField(p.busComp, rig::CompField::Makeup); };
  const double m0 = w.applied(makeup);
  auto& ctl = w.panel->controller();
  const int starts0 = ctl.timerStartsForTests();
  Hand hand(k);
  float dy = 0.0f;
  std::vector<double> seen{m0};
  bool changedBeforeUp = false;
  int fires = 0;
  for (int ev = 1; ev <= 40; ++ev) {
    dy -= 3.0f;
    hand.to(dy);
    if (ev <= 4) CHECK(w.applied(makeup) == m0);  // nothing handed over before the first interval is over
    if (ev % 5 == 0) {
      CHECK(ctl.flushTimerForTests());  // an interval ends: the latest value is applied (the timer was running)
      ++fires;
      const double a = w.applied(makeup);
      if (a != seen.back()) seen.push_back(a);
      if (a != m0) changedBeforeUp = true;
      w.proc.waitForLoader(std::chrono::milliseconds(60000));
    }
  }
  CHECK(fires == 8);
  CHECK(changedBeforeUp);                       // applied while the hand was still down: the model changed before mouse-up
  CHECK(seen.size() == static_cast<std::size_t>(fires) + 1);  // every interval applied a newer value ...
  CHECK(ctl.timerStartsForTests() - starts0 == fires);        // ... and the timer was started once per interval, never restarted per event
  hand.up(dy);
  CHECK_FALSE(w.panel->controller().hasPending());
  w.settle();
  CHECK(std::abs(w.model(makeup) - k.getValue()) < kTol * 72.0);
  CHECK(std::abs(w.applied(makeup) - k.getValue()) < kTol * 72.0);
}

TEST_CASE("rig knobs: KEY HPF shows the model's value (OFF / 40 Hz) the moment the hand lets go", "[editor][live][rigknobs]") {
  RigKnobWorld w;
  w.panel->setTab(rig::RigEditorPanel::Tab::Gate);
  w.panel->refresh();
  auto knobs = shownPresetKnobs(*w.panel);
  REQUIRE(knobs.size() == 7);
  skin::FilmstripKnob& k = knobs[6]->knob();  // KEY HPF 0..400 Hz
  const Reader hpf = [](const Preset& p) { return rig::gateField(p.gate, rig::GateField::KeyHpf); };
  struct Case {
    double releaseHz, modelHz;
  };
  for (const Case c : {Case{25.0, 40.0}, Case{10.0, 0.0}}) {
    INFO("released at " << c.releaseHz << " Hz");
    k.setValue(150.0, juce::dontSendNotification);
    Hand hand(k);
    const float down = static_cast<float>((150.0 - c.releaseHz) / 400.0 * 250.0);  // pixels down from 150 Hz
    hand.to(down / 2);
    hand.to(down);
    CHECK(k.getValue() == Catch::Approx(c.releaseHz).margin(0.01));
    hand.up(down);
    CHECK(k.getValue() == c.modelHz);  // before any refresh tick
    w.settle();
    CHECK(w.model(hpf) == c.modelHz);
    w.tick();
    CHECK(k.getValue() == c.modelHz);
  }
}

TEST_CASE("rig knobs: destroying a PresetKnob mid-drag closes its gesture", "[editor][live][rigknobs]") {
  RigKnobWorld w;
  int begin = 0, end = 0;
  auto pk = std::make_unique<rig::PresetKnob>(w.panel->controller(), "X", skin::FilmstripKnob::Kind::Pedal, juce::Colours::red,
                                              skin::FilmstripKnob::Range{0.0, 10.0, 5.0, 1, "u"}, [](Preset&, double) {});
  pk->onGestureBegin = [&] { ++begin; };
  pk->onGestureEnd = [&] { ++end; };
  Hand hand(pk->knob());
  hand.to(-20.0f);
  CHECK(begin == 1);
  CHECK(end == 0);
  pk.reset();
  CHECK(begin == 1);
  CHECK(end == 1);
  w.settle();
}

TEST_CASE("rig knobs: the host-parameter knobs (BLEND, levels, gate THRESHOLD) sweep min to max in 250 px with refresh ticks in between",
          "[editor][live][rigknobs]") {
  RigKnobWorld w;
  w.panel->setTab(rig::RigEditorPanel::Tab::Blend);
  for (const char* id : {"blend", "levelA", "levelB"}) {
    INFO(id);
    w.panel->refresh();
    skin::FilmstripKnob& k = knobByParam(*w.panel, id);
    REQUIRE(k.isEnabled());
    k.setValue(k.getMinimum(), juce::sendNotificationSync);
    Hand hand(k);
    float dy = 0.0f;
    for (int i = 0; i < 25; ++i) {
      dy -= 10.0f;
      hand.to(dy);
      const double before = k.getValue();
      if (i % 2 == 1) w.tick();
      CHECK(k.getValue() == before);
    }
    hand.up(dy);
    CHECK(k.proportion() > 0.999);
    CHECK(static_cast<double>(w.proc.parameters().getRawParameterValue(id)->load()) == Catch::Approx(k.getMaximum()).margin(1e-3));
  }
  w.panel->setTab(rig::RigEditorPanel::Tab::Gate);
  w.panel->refresh();
  {
    skin::FilmstripKnob& k = knobByParam(*w.panel, "gateThreshold");
    k.setValue(k.getMinimum(), juce::sendNotificationSync);
    Hand hand(k);
    float dy = 0.0f;
    for (int i = 0; i < 25; ++i) {
      dy -= 10.0f;
      hand.to(dy);
      const double before = k.getValue();
      if (i % 2 == 1) w.tick();
      CHECK(k.getValue() == before);
    }
    hand.up(dy);
    CHECK(k.proportion() > 0.999);
  }
}

TEST_CASE("rig knobs: KEY HPF does not stick in its OFF / 40 Hz dead zone under the mouse wheel", "[editor][live][rigknobs]") {
  RigKnobWorld w;
  w.panel->setTab(rig::RigEditorPanel::Tab::Gate);
  w.panel->refresh();
  auto knobs = shownPresetKnobs(*w.panel);
  REQUIRE(knobs.size() == 7);
  skin::FilmstripKnob& k = knobs[6]->knob();
  const Reader hpf = [](const Preset& p) { return rig::gateField(p.gate, rig::GateField::KeyHpf); };
  auto wheel = [&](float dy) {
    juce::MouseWheelDetails wd{};
    wd.deltaY = dy;
    k.mouseWheelMove(mouseAtMods(k, {40.0f, 60.0f}, {40.0f, 60.0f}, juce::ModifierKeys()), wd);
    w.panel->controller().flushTimerForTests();  // the debounce / throttle timer, fired explicitly (no wall-clock wait)
    w.settle();
  };
  for (float notch : {0.1f, 0.2f}) {
    INFO("notch " << notch);
    // (a) from OFF, up leaves OFF and keeps going up
    w.panel->controller().edit([](Preset& p) { rig::setGateField(p, rig::GateField::KeyHpf, 0.0); });
    w.settle();
    w.panel->refresh();
    REQUIRE(k.getValue() == 0.0);
    wheel(notch);
    CHECK(k.getValue() == 40.0);
    CHECK(w.model(hpf) == 40.0);
    wheel(notch);
    const double second = k.getValue();
    CHECK(second > 40.0);
    CHECK(w.model(hpf) == second);
    wheel(notch);
    CHECK(k.getValue() > second);
    // (b) from 40 Hz, down goes to OFF
    w.panel->controller().edit([](Preset& p) { rig::setGateField(p, rig::GateField::KeyHpf, 40.0); });
    w.settle();
    w.panel->refresh();
    REQUIRE(k.getValue() == 40.0);
    wheel(-notch);
    CHECK(k.getValue() == 0.0);
    CHECK(w.model(hpf) == 0.0);
  }
}

TEST_CASE("rig knobs: a real double-click sequence (down x2, double-click, up) resets to the default and leaves nothing pending",
          "[editor][live][rigknobs]") {
  RigKnobWorld w;
  w.panel->setTab(rig::RigEditorPanel::Tab::Gate);
  w.panel->refresh();
  auto knobs = shownPresetKnobs(*w.panel);
  REQUIRE(knobs.size() == 7);
  rig::PresetKnob& pk = *knobs[4];  // RANGE -120..0 dB, default -90
  skin::FilmstripKnob& k = pk.knob();
  const Reader range = [](const Preset& p) { return rig::gateField(p.gate, rig::GateField::Range); };
  int begin = 0, end = 0;
  pk.onGestureBegin = [&] { ++begin; };
  pk.onGestureEnd = [&] { ++end; };
  {  // move it off the default first
    Hand hand(k);
    hand.to(-40.0f);
    hand.up(-40.0f);
    w.settle();
    REQUIRE(k.getValue() != -90.0);
    REQUIRE(w.model(range) == k.getValue());
  }
  begin = end = 0;
  const juce::Point<float> p(40.0f, 60.0f);
  const juce::ModifierKeys left(juce::ModifierKeys::leftButtonModifier);
  k.mouseDown(mouseAtMods(k, p, p, left, 1));
  k.mouseUp(mouseAtMods(k, p, p, left, 1));
  k.mouseDown(mouseAtMods(k, p, p, left, 2));
  k.mouseDoubleClick(mouseAtMods(k, p, p, left, 2));
  k.mouseUp(mouseAtMods(k, p, p, left, 2));
  CHECK(k.getValue() == -90.0);
  CHECK_FALSE(k.mouseHeld());
  CHECK_FALSE(w.panel->controller().hasPending());  // the mouse-up flushed the reset
  CHECK(w.model(range) == -90.0);
  w.settle();
  CHECK(w.model(range) == -90.0);
  CHECK(w.applied(range) == -90.0);
  CHECK(begin == end);  // the gesture hooks balance
  CHECK(begin >= 1);
}

TEST_CASE("rig knobs: mouseHeld() is false after a mouse-up and after a wheel event", "[editor][live][rigknobs]") {
  RigKnobWorld w;
  w.panel->setTab(rig::RigEditorPanel::Tab::Gate);
  w.panel->refresh();
  auto knobs = shownPresetKnobs(*w.panel);
  REQUIRE(knobs.size() == 7);
  skin::FilmstripKnob& k = knobs[4]->knob();
  CHECK_FALSE(k.mouseHeld());
  {
    Hand hand(k);
    CHECK(k.mouseHeld());
    hand.to(-20.0f);
    CHECK(k.mouseHeld());
    hand.up(-20.0f);
    CHECK_FALSE(k.mouseHeld());
  }
  w.settle();
  juce::MouseWheelDetails wd{};
  wd.deltaY = 0.2f;
  k.mouseWheelMove(mouseAtMods(k, {40.0f, 60.0f}, {40.0f, 60.0f}, juce::ModifierKeys()), wd);
  CHECK_FALSE(k.mouseHeld());
  w.panel->controller().flushTimerForTests();  // the debounce / throttle timer, fired explicitly (no wall-clock wait)
  w.settle();
  CHECK_FALSE(k.mouseHeld());
}
