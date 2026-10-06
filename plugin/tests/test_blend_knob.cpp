// v0.3 Task C: the BLEND knob turns a single-path rig into a blend by itself, and the body amp actually appears.
//
// Everything is driven the way the user acts: mouse events on the main panel's BLEND knob and on the body head's knobs, Cmd+Z on the
// editor. The TONE3000 tool is a fake `sawblade-t3k` (a shell script steered by files in a temp dir): `suggest-body` answers null (an
// empty pool), `fetch` waits while `hold` exists (so the "downloading" state can be looked at), then answers like the real tool or fails
// the way the real one does (exit 4 = not logged in; --json {"error","code"} for network / not_found). It copies a fixture model into
// the capture cache; nothing here is a real capture. Needs a display (the editor tests run under xvfb-run).
#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <thread>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "rig/AmpHead.h"
#include "rig/RigController.h"
#include "rig/RigModel.h"
#include "sawblade/auto_trim.h"
#include "skin/FilmstripKnob.h"

using namespace sawblade;
using namespace sawblade::plugin;
using nlohmann::json;
using sawblade::plugin::rig::FillReason;
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
// A mouse drag of `dy` pixels (negative = up) on a knob: 250 px = the full range.
void drag(skin::FilmstripKnob& k, float dy) {
  const juce::Point<float> start(15.0f, 15.0f), end(15.0f, 15.0f + dy);
  k.mouseDown(ev(k, start, start));
  k.mouseDrag(ev(k, end, start));
  k.mouseUp(ev(k, end, start));
}

struct Dirs {
  fs::path dir;
  Dirs() {
    static int n = 0;
    dir = fs::temp_directory_path() / ("sawblade_blendknob_" + std::to_string(std::random_device{}()) + "_" + std::to_string(n++));
    fs::create_directories(dir / "cache");
    setCaptureCacheRootOverride(dir / "cache");  // the pool is empty: nothing is cached
  }
  ~Dirs() {
    setCaptureCacheRootOverride(std::nullopt);
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

struct NetworkAllowed {  // the editor tests run with SAWBLADE_NO_NETWORK=1; these ones run the fake tool for real
  std::optional<std::string> old;
  NetworkAllowed() {
    if (const char* c = std::getenv("SAWBLADE_NO_NETWORK")) old = c;
    ::setenv("SAWBLADE_NO_NETWORK", "0", 1);
  }
  ~NetworkAllowed() {
    if (old) ::setenv("SAWBLADE_NO_NETWORK", old->c_str(), 1);
    else ::unsetenv("SAWBLADE_NO_NETWORK");
  }
};

// A path A amp titled "Marshall A"; path B empty and off; BLEND 0 (full SAW). `inputGainDb` makes the level-match trim non-zero.
json sawOnlyJson(double inputGainDb = 0.0) {
  json a = {{"blocks", json::array({{{"id", "a1"}, {"type", "nam"}, {"slot", "amp"},
                                     {"model", {{"file", (kFx / "nam" / "linear_identity.nam").string()},
                                                {"source", {{"provider", "tone3000"}, {"id", "T0"}, {"modelId", "m0"}, {"title", "Marshall A"}}}}}}})}};
  json b = {{"enabled", false}, {"blocks", json::array()}};
  return {{"schema", "sawblade.preset"}, {"version", 3}, {"name", "saw only"}, {"input", {{"gainDb", inputGainDb}}},
          {"paths", {{"a", a}, {"b", b}}}, {"align", {{"mode", "off"}}}, {"blend", 0.0},
          {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}};
}

struct Rig {
  SettingsEnv env{"{}", /*isolateHome=*/true};
  NetworkAllowed net;
  Dirs dirs;
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;
  fs::path exe, log, mode, hold;

  explicit Rig(const std::string& initialMode = "ok", bool toolPresent = true) {
    exe = dirs.dir / "fake-t3k";
    log = dirs.dir / "calls.log";
    mode = dirs.dir / "mode";
    hold = dirs.dir / "hold";
    setMode(initialMode);
    if (toolPresent) writeTool();
    // the shared settings file names the tool (T3kTool reads it on every run)
    std::ofstream(env.dir / "settings.json") << json{{"t3kExecutable", exe.string()}}.dump();
    proc.setLevelDebounceMs(0);
    proc.prepareToPlay(48000.0, 512);
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  }
  ~Rig() {
    release();  // a fetch waiting on `hold` ends
    base.reset();
  }
  void writeTool() {
    std::ofstream(exe) << "#!/bin/sh\necho \"$@\" >> '" << log.string() << "'\n"
                       << "cmd=$1\n"
                       << "if [ \"$cmd\" = suggest-body ]; then echo null; exit 0; fi\n"
                       << "if [ \"$cmd\" = fetch ]; then\n"
                       << "  while [ -f '" << hold.string() << "' ]; do sleep 0.05; done\n"
                       << "  m=$(cat '" << mode.string() << "')\n"
                       << "  if [ \"$m\" = auth ]; then echo '{\"error\":\"not logged in to TONE3000\",\"code\":\"auth\"}'; exit 4; fi\n"
                       << "  if [ \"$m\" = network ]; then echo '{\"error\":\"network failure: ConnectError\",\"code\":\"network\"}'; exit 1; fi\n"
                       << "  if [ \"$m\" = notfound ]; then echo '{\"error\":\"tone 88689 has no usable models\",\"code\":\"not_found\"}'; exit 1; fi\n"
                       << "  tone=$2; model=5001; cache=''; shift 2\n"
                       << "  while [ $# -gt 0 ]; do case \"$1\" in --model) model=$2; shift 2;; --cache-dir) cache=$2; shift 2;; *) shift;; esac; done\n"
                       << "  mkdir -p \"$cache/$tone\"; cp '" << (kFx / "nam" / "linear_identity.nam").string() << "' \"$cache/$tone/$model.nam\"\n"
                       << "  sha=$(sha256sum \"$cache/$tone/$model.nam\" | cut -d' ' -f1)\n"
                       << "  printf '{\"tone_id\":\"%s\",\"model_id\":\"%s\",\"path\":\"%s/%s/%s.nam\",\"sha256\":\"%s\",\"kind\":\"nam\",\"gear\":\"amp\","
                          "\"source\":{\"provider\":\"tone3000\",\"id\":\"%s\",\"modelId\":\"%s\",\"title\":\"EVH 5150iii Ivory\",\"creator\":\"c\",\"license\":\"cc-by\",\"url\":\"u\"}}\\n' "
                          "\"$tone\" \"$model\" \"$cache\" \"$tone\" \"$model\" \"$sha\" \"$tone\" \"$model\"\n"
                       << "fi\n";
    ::chmod(exe.c_str(), 0755);
  }
  void setMode(const std::string& m) const { std::ofstream(mode) << m << "\n"; }
  void holdFetches() const { std::ofstream(hold) << "x"; }
  void release() const {
    std::error_code ec;
    fs::remove(hold, ec);
  }
  // The fill's own runs (the ladder lookup of path A's tone is a different tool call, started by the processor).
  int fillCalls() const {
    int n = 0;
    std::ifstream in(log);
    for (std::string l; std::getline(in, l);)
      if (l.rfind("suggest-body", 0) == 0) ++n;
    return n;
  }
  int fetchCalls() const {
    int n = 0;
    std::ifstream in(log);
    for (std::string l; std::getline(in, l);)
      if (l.rfind("fetch", 0) == 0) ++n;
    return n;
  }

  void load(const json& j) {
    const fs::path f = dirs.dir / "p.json";
    std::ofstream(f) << j.dump(2);
    REQUIRE(proc.loadPresetFile(f));
    REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
    REQUIRE(proc.status().error.empty());
    pump(60);
  }
  void pump(int ms) {
    juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
    ed->refreshNow();
  }
  bool pumpUntil(const std::function<bool()>& pred, int timeoutMs = 30000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
      pump(20);
      if (pred()) return true;
    }
    return false;
  }
  void settle() {
    REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
    pump(40);
  }
  skin::FilmstripKnob& blendKnob() {
    std::vector<skin::FilmstripKnob*> all;
    collect(*ed, all);
    for (auto* k : all)
      if (k->paramId() == "blend") return *k;  // the main panel's knob comes before the rig editor's
    FAIL("no BLEND knob");
    return *all.front();
  }
  juce::String bodyHead() { return ed->ampHead(1).readout(); }
  static const NamBlockParams* ampOf(const PathPreset& p) {
    const int i = ampIndex(p);
    return i < 0 ? nullptr : dynamic_cast<const NamBlockParams*>(p.blocks[static_cast<std::size_t>(i)].params.get());
  }
};

}  // namespace

TEST_CASE("blend knob: 0 -> 0.5 with the mouse on a SAW-only preset enables blend and fills B; the head says downloading, then binds the amp",
          "[blendknob][editor]") {
  Rig rig;
  rig.load(sawOnlyJson());
  rig.holdFetches();
  REQUIRE_FALSE(rig.proc.currentPreset().b.enabled);
  CHECK(rig.ed->ampHead(1).readout() == rig::AmpHead::bodyOffText());
  auto& blend = rig.blendKnob();
  REQUIRE(blend.isEnabled());  // enabled on a single-path rig: this is how a blend starts
  CHECK(blend.getValue() == 0.0);

  drag(blend, -125.0f);  // half the range
  CHECK(blend.getValue() == Catch::Approx(0.5).margin(0.02));
  rig.settle();
  const Preset p = rig.proc.currentPreset();
  CHECK(p.b.enabled);
  REQUIRE(p.b.blocks.size() >= 1);
  CHECK(p.b.blocks[0].type == "pedal.ts");  // the boost is there at once ...
  CHECK(p.blend == Catch::Approx(0.5).margin(0.02));
  CHECK(blend.getValue() == Catch::Approx(0.5).margin(0.02));  // the rebuild did not move the knob
  CHECK(p.levelMatch.mode == LevelMatchMode::Auto);            // the same fill as the rig section's switch
  CHECK(rig.ed->rigController().canUndo());

  // ... and the amp is on its way: the head says what it is waiting for (never a silent boost-only path).
  REQUIRE(rig.pumpUntil([&] { return rig.bodyHead().startsWith("BODY AMP DOWNLOADING"); }));
  CHECK(rig.bodyHead() == rig::AmpHead::bodyDownloadingText(rig::kFallbackBodyTitle));
  CHECK(rig.bodyHead().contains(juce::String::fromUTF8("\xE2\x80\xA6")));
  CHECK(Rig::ampOf(rig.proc.currentPreset().b) == nullptr);
  CHECK_FALSE(rig.ed->ampHead(1).knobsEnabled());

  // The amp arrives: the head reads its GAIN and the knobs are bound to it.
  rig.release();
  REQUIRE(rig.pumpUntil([&] { return rig.bodyHead().startsWith("GAIN"); }));
  rig.settle();
  const Preset q = rig.proc.currentPreset();
  REQUIRE(Rig::ampOf(q.b) != nullptr);
  CHECK(Rig::ampOf(q.b)->model.source->id == rig::kFallbackBodyTone);  // the shortlist amp
  CHECK(rig.ed->ampHead(1).knobsEnabled());
  for (int k = 0; k < kAmpKnobCount; ++k) CHECK(rig.ed->ampHead(1).knob(k).isEnabled());
  CHECK(q.b.blocks[0].type == "pedal.ts");  // the boost stayed in front
  CHECK(rig.fetchCalls() == 1);
  CHECK(rig.proc.status().error.empty());
  auto& gain = rig.ed->ampHead(1).knob(kAmpGain);
  const double g0 = gain.getValue();
  drag(gain, -50.0f);  // +2
  CHECK(gain.getValue() == Catch::Approx(g0 + 2.0).margin(0.05));
  CHECK(rig.proc.currentPreset().b.ampControls.gain == Catch::Approx(g0 + 2.0).margin(0.05));
  CHECK(static_cast<double>(rig.proc.parameters().getRawParameterValue("ampB_gain")->load()) == Catch::Approx(g0 + 2.0).margin(0.05));
}

TEST_CASE("blend knob: BLEND moved while the amp downloads does not drop the amp", "[blendknob][editor]") {
  Rig rig;
  rig.load(sawOnlyJson());
  rig.holdFetches();
  drag(rig.blendKnob(), -125.0f);
  rig.settle();
  REQUIRE(rig.pumpUntil([&] { return rig.bodyHead().startsWith("BODY AMP DOWNLOADING"); }));
  drag(rig.blendKnob(), -50.0f);  // the player keeps turning it
  drag(rig.blendKnob(), 25.0f);
  const double b = rig.blendKnob().getValue();
  rig.release();
  REQUIRE(rig.pumpUntil([&] { return rig.bodyHead().startsWith("GAIN"); }));
  rig.settle();
  CHECK(Rig::ampOf(rig.proc.currentPreset().b) != nullptr);
  CHECK(rig.proc.currentPreset().blend == Catch::Approx(b).margin(0.02));  // the amp arrival did not move BLEND either
}

TEST_CASE("blend knob: a failing tool shows why and the one action; touching BLEND again retries", "[blendknob][editor]") {
  struct Case {
    const char* mode;
    FillReason reason;
  };
  for (const Case& c : {Case{"auth", FillReason::NotLoggedIn}, Case{"network", FillReason::Network}, Case{"notfound", FillReason::NoCapture}}) {
    CAPTURE(c.mode);
    Rig rig(c.mode);
    rig.load(sawOnlyJson());
    drag(rig.blendKnob(), -125.0f);
    rig.settle();
    REQUIRE(rig.pumpUntil([&] { return rig.bodyHead() == rig::AmpHead::bodyFailedText(c.reason); }));
    CHECK(rig.bodyHead() != rig::AmpHead::noAmpText());  // never the silent "NO AMP"
    CHECK(rig.proc.currentPreset().b.enabled);
    CHECK(Rig::ampOf(rig.proc.currentPreset().b) == nullptr);  // boost only, and the head says so
    CHECK_FALSE(rig.ed->ampHead(1).knobsEnabled());
    // The fix (a login, a connection): touching BLEND again retries the fill.
    rig.setMode("ok");
    drag(rig.blendKnob(), 10.0f);
    rig.settle();
    REQUIRE(rig.pumpUntil([&] { return rig.bodyHead().startsWith("GAIN"); }));
    rig.settle();
    CHECK(Rig::ampOf(rig.proc.currentPreset().b) != nullptr);
  }
}

TEST_CASE("blend knob: the failure texts name a reason and an action", "[blendknob][editor]") {
  using R = FillReason;
  for (R r : {R::NotLoggedIn, R::Network, R::NoCapture, R::NoTool, R::License, R::NetworkOff, R::Other}) {
    const juce::String t = rig::AmpHead::bodyFailedText(r);
    INFO(t);
    CHECK(t.contains(juce::String::fromUTF8(" \xE2\x80\x94 ")));  // "<reason> - <action>"
    CHECK(t.length() <= 60);                                      // fits the head's read-out pill
  }
  CHECK(rig::AmpHead::bodyFailedText(R::NotLoggedIn).contains("Settings"));
  CHECK(rig::AmpHead::bodyFailedText(R::NoCapture).contains("BROWSE CAPTURES"));
}

TEST_CASE("blend knob: no tool at all is shown too (not a silent boost-only path)", "[blendknob][editor]") {
  Rig rig("ok", /*toolPresent=*/false);
  rig.load(sawOnlyJson());
  drag(rig.blendKnob(), -125.0f);
  rig.settle();
  REQUIRE(rig.pumpUntil([&] { return rig.bodyHead() == rig::AmpHead::bodyFailedText(FillReason::NoTool); }));
  CHECK(rig.proc.currentPreset().b.enabled);
}

TEST_CASE("blend knob: host automation of BLEND never changes the topology", "[blendknob][editor]") {
  Rig rig;
  rig.load(sawOnlyJson());
  auto* prm = rig.proc.parameters().getParameter("blend");
  REQUIRE(prm != nullptr);
  prm->setValueNotifyingHost(prm->convertTo0to1(0.6f));  // automation on the message thread
  rig.pump(100);
  std::thread([&] { prm->setValueNotifyingHost(prm->convertTo0to1(0.8f)); }).join();  // and from another thread (the audio thread's role)
  rig.pump(300);
  rig.settle();
  const Preset p = rig.proc.currentPreset();
  CHECK_FALSE(p.b.enabled);
  CHECK(p.b.blocks.empty());
  CHECK(rig.fillCalls() == 0);
  CHECK_FALSE(rig.ed->rigController().canUndo());
  CHECK(rig.bodyHead() == rig::AmpHead::bodyOffText());
  // A state restore carrying a BLEND value is no gesture either.
  juce::MemoryBlock state;
  rig.proc.getStateInformation(state);
  rig.proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
  rig.settle();
  CHECK_FALSE(rig.proc.currentPreset().b.enabled);
  CHECK(rig.fillCalls() == 0);
}

TEST_CASE("blend knob: back to full SAW keeps path B; one undo restores the SAW-only preset", "[blendknob][editor]") {
  Rig rig;
  rig.load(sawOnlyJson());
  drag(rig.blendKnob(), -125.0f);
  rig.settle();
  REQUIRE(rig.pumpUntil([&] { return rig.bodyHead().startsWith("GAIN"); }));
  rig.settle();
  const Preset filled = rig.proc.currentPreset();
  REQUIRE(filled.b.blocks.size() == 2);

  drag(rig.blendKnob(), 300.0f);  // all the way down: full SAW
  rig.settle();
  CHECK(rig.blendKnob().getValue() == 0.0);
  const Preset saw = rig.proc.currentPreset();
  CHECK(saw.b.enabled);                             // path B is not deleted ...
  CHECK(saw.b.blocks.size() == filled.b.blocks.size());
  CHECK(saw.b.blocks[1].type == "nam");
  CHECK(rig.ed->ampHead(1).knobsEnabled());         // ... and its head stays live
  drag(rig.blendKnob(), -100.0f);                   // up again: nothing is filled twice
  rig.settle();
  CHECK(rig.proc.currentPreset().b.blocks.size() == filled.b.blocks.size());
  CHECK(rig.fetchCalls() == 1);

  // The fill is one undo step: from a fresh knob move, Cmd+Z restores the SAW-only preset exactly.
  Rig again;
  again.load(sawOnlyJson());
  const Preset before = again.proc.currentPreset();
  drag(again.blendKnob(), -125.0f);
  again.settle();
  REQUIRE(again.pumpUntil([&] { return again.bodyHead().startsWith("GAIN"); }));
  again.settle();
  REQUIRE(again.ed->rigController().canUndo());
  CHECK(again.ed->keyPressed(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)));
  again.settle();
  CHECK(again.proc.currentPreset() == before);
  CHECK(again.blendKnob().getValue() == 0.0);  // the knob is back on full SAW
  CHECK_FALSE(again.ed->rigController().canUndo());
}

TEST_CASE("blend knob: the new topology is level matched in the background, from the old trim, with no dip to 0", "[blendknob][editor][levelmatch]") {
  Rig rig;
  rig.load(sawOnlyJson(6.0));
  REQUIRE(rig.proc.waitForLevelWork());
  const double t0 = rig.proc.status().trimDb;
  REQUIRE(std::fabs(t0) > 1.0);
  const auto jobs0 = rig.proc.levelWorker().trimJobsRun();
  rig.holdFetches();
  std::vector<std::pair<bool, double>> seen;  // (pending, trim) at every look
  const auto look = [&] {
    const auto st = rig.proc.status();
    seen.emplace_back(st.levelPending, st.trimDb);
  };
  drag(rig.blendKnob(), -125.0f);
  look();
  rig.settle();
  look();
  REQUIRE(rig.pumpUntil([&] {
    look();
    return rig.bodyHead().startsWith("BODY AMP DOWNLOADING") && !rig.proc.status().levelPending;
  }));
  rig.release();  // the amp arrives: a second topology, measured again
  REQUIRE(rig.pumpUntil([&] {
    look();
    return rig.bodyHead().startsWith("GAIN");
  }));
  rig.settle();
  REQUIRE(rig.proc.waitForLevelWork());
  look();
  for (const auto& [pending, trim] : seen)
    if (pending) CHECK(trim == t0);  // until a measurement lands the old trim holds: never 0
  CHECK(rig.proc.levelWorker().trimJobsRun() > jobs0);  // measured on the level worker, not on the audio thread
  const Preset now = rig.proc.currentPreset();
  CHECK(autoTrimFresh(now));                            // the trim in the preset is the new topology's
  CHECK(rig.proc.status().trimDb == Catch::Approx(now.autoTrim.db).margin(1e-9));
  CHECK_FALSE(rig.proc.status().levelFailed);
}
