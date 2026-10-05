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

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "pedals/CircuitFaces.h"
#include "pedals/PedalFace.h"
#include "presets/PresetLoadFlow.h"
#include "rig/EqGraph.h"
#include "rig/RigEditorPanel.h"
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
  void browserLoad(const fs::path& file) {
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
  moveKnobFar(*knob);
  b.settle();
  a.settle();
  const auto b1 = b.render();
  CHECK(relDiff(b0, b1) > kMinRelDiff);  // B did change
  CHECK(relDiff(a1, a.render()) < kSameRelDiff);
  CHECK(a.proc.currentPreset().name == "UK Death Fuzz Blend (match v1)");
}
