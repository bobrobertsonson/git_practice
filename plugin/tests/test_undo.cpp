// v0.3 Task D, processor + controller level: undo / redo of every rig edit.
//
// "Exact" means JSON-exact: Preset::operator== ignores the level-match stamp (AutoTrimStamp::operator== is always true, by design), so
// every restore here is compared as toJson() text, which carries the stamp and every parameter value.
// Level match is off in the exactness tests (a measured stamp that lands between two looks is not what they are about) and on in the
// trim test.
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <functional>

#include "PluginProcessor.h"
#include "PresetAudition.h"
#include "SettingsEnv.h"
#include "rig/RigController.h"
#include "rig/RigModel.h"
#include "LadderFetch.h"
#include "sawblade/auto_trim.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::plugin;
using namespace sawblade::plugin::rig;
using nlohmann::json;
namespace fs = std::filesystem;

#include "processor_harness.h"

namespace {

std::string J(const Preset& p) { return toJson(p).dump(); }

struct Undo {
  TempDir tmp;
  SettingsEnv env;
  Host h;
  fs::path presetFile;
  explicit Undo(const char* settings = R"({"levelMatch": false})") : env(settings), h(48000.0, 512) {
    h.p.setLevelDebounceMs(0);
    presetFile = writeIdentityPreset(tmp.dir, "base", 0);
    h.load(presetFile);
  }
  std::string now() const { return J(h.p.currentPreset()); }
  void settle() { REQUIRE(h.p.waitForLoader()); }
};

Block eqBlk(const std::string& id) { return makeBlock("eq", id, "fx", kFixtures, flatEqFields()); }

struct Kind {
  const char* name;
  std::function<void(Undo&, RigController&)> setup;  // runs before the baseline (may be empty); its own steps are not the subject
  std::function<void(Undo&, RigController&)> act;    // the edit: must be exactly one step
};

}  // namespace

TEST_CASE("undo: every kind of rig edit is one step and undoes / redoes to the exact preset", "[undo][rig]") {
  const std::vector<Kind> kinds = {
      {"add a block", {}, [](Undo&, RigController& c) { c.edit([](Preset& p) { addBlock(p.a, 1, eqBlk("a9")); }); }},
      {"remove a block", {}, [](Undo&, RigController& c) { c.edit([](Preset& p) { removeBlock(p.b, 0); }); }},
      {"reorder blocks", [](Undo&, RigController& c) { c.edit([](Preset& p) { addBlock(p.a, 1, eqBlk("a9")); }); },
       [](Undo&, RigController& c) { c.edit([](Preset& p) { moveBlock(p.a, 0, 1); }); }},
      {"bypass a block", {}, [](Undo&, RigController& c) { c.edit([](Preset& p) { setBypass(p.a, 0, true); }); }},
      {"swap a capture with its make-up", {},
       [](Undo& u, RigController&) {
         Preset p = u.h.p.editBasePreset();
         p.a.blocks[0] = makeBlock("nam", "a1", "amp", kFixtures,
                                   {{"model", {{"file", (kFixtures / "nam" / "linear_05_025.nam").string()}}}, {"makeupDb", 2.5}});
         u.h.p.loadPresetUndoable(std::move(p), SawbladeProcessor::HistoryKind::Edit, /*keepMonitor=*/false);  // what the capture browser does
       }},
      {"a block's parameters (debounced edit)", [](Undo&, RigController& c) { c.edit([](Preset& p) { addBlock(p.a, 1, eqBlk("a9")); }); },
       [](Undo&, RigController& c) {
         c.editDebounced([](Preset& p) { setBlockInputGainDb(p.a, 0, 4.0); });
         c.flushPending();
       }},
      {"an EQ band (live)", {}, [](Undo&, RigController& c) { c.eqLive(EqTarget::Post, 0, 2500.0, 3.0, 2.0); }},
      {"an EQ band added", {}, [](Undo&, RigController& c) { c.edit([](Preset& p) { addBand(p, EqTarget::Post, EqBand{}); }); }},
      {"cab on / off", {}, [](Undo&, RigController& c) { c.edit([](Preset& p) { setCabEnabled(p, false); }); }},
      {"cab mode", {}, [](Undo&, RigController& c) { c.edit([](Preset& p) { setCabMode(p, CabMode::PerPath); }); }},
      {"gate", {}, [](Undo&, RigController& c) { c.edit([](Preset& p) { setGateEnabled(p, true); }); }},
      {"bus comp", {}, [](Undo&, RigController& c) { c.edit([](Preset& p) { setCompEnabled(p, true); }); }},
      {"topology (blend fill)", [](Undo&, RigController& c) { c.edit([](Preset& p) { p.b.blocks.clear(); p.b.enabled = false; p.blend = 0.0; }); },
       [](Undo&, RigController& c) { c.setTopology(Topology::Blend); }},
      {"topology (single)", {}, [](Undo&, RigController& c) { c.setTopology(Topology::Single); }},
  };
  for (const Kind& k : kinds) {
    INFO(k.name);
    Undo u;
    RigController ctl(u.h.p);
    if (k.setup) {
      k.setup(u, ctl);
      u.settle();
    }
    u.h.p.historyClear();
    const std::string before = u.now();
    k.act(u, ctl);
    u.settle();
    const std::string after = u.now();
    REQUIRE(after != before);
    REQUIRE(u.h.p.undoSteps() == 1);
    REQUIRE_FALSE(ctl.canRedo());
    for (int round = 0; round < 2; ++round) {  // twice: a redone step undoes again
      REQUIRE(ctl.undo());
      u.settle();
      CHECK(u.now() == before);
      CHECK(ctl.canRedo());
      CHECK_FALSE(ctl.canUndo());
      REQUIRE(ctl.redo());
      u.settle();
      CHECK(u.now() == after);
      CHECK(ctl.canUndo());
      CHECK_FALSE(ctl.canRedo());
    }
  }
}

TEST_CASE("undo: a preset load is a step of its own and never clears the history; undo goes back to the exact preset", "[undo][rig]") {
  Undo u;
  RigController ctl(u.h.p);
  ctl.edit([](Preset& p) { setBypass(p.a, 0, true); });
  u.settle();
  const std::string beforeLoad = u.now();
  const fs::path other = writeIdentityPreset(u.tmp.dir, "other", 0);
  { std::ifstream in(other); json j = json::parse(in); j["blend"] = 0.25; j["name"] = "other"; std::ofstream(other) << j.dump(2); }
  REQUIRE(u.h.p.loadPresetFile(other, nullptr, /*undoable=*/true));
  u.settle();
  const std::string loaded = u.now();
  REQUIRE(loaded != beforeLoad);
  CHECK(u.h.p.undoSteps() == 2);  // the edit before it is still there
  REQUIRE(ctl.undo());
  u.settle();
  CHECK(u.now() == beforeLoad);
  REQUIRE(ctl.redo());
  u.settle();
  CHECK(u.now() == loaded);
  REQUIRE(ctl.undo());
  REQUIRE(ctl.undo());  // and through the load to the edit before it
  u.settle();
  CHECK_FALSE(u.h.p.currentPreset().a.blocks[0].bypass);
  CHECK_FALSE(ctl.canUndo());
  // A load that is not a user's (A / B compare, the audition, a restore) adds no step.
  u.h.p.historyClear();
  u.h.p.loadPreset(u.h.p.currentPreset());
  u.settle();
  CHECK(u.h.p.undoSteps() == 0);
}

TEST_CASE("undo: apply-match (audition APPLY) is one step back to the preset before the audition", "[undo][rig]") {
  Undo u;
  RigController ctl(u.h.p);
  const std::string original = u.now();
  const fs::path cand = writeIdentityPreset(u.tmp.dir, "cand", 0);
  { std::ifstream in(cand); json j = json::parse(in); j["blend"] = 0.8; j["name"] = "cand"; std::ofstream(cand) << j.dump(2); }
  std::string err;
  REQUIRE(u.h.p.audition().audition(cand, &err));
  u.settle();
  CHECK(u.h.p.undoSteps() == 0);  // the audition and its A / B toggles are not steps
  REQUIRE(u.h.p.audition().toggleAB());
  REQUIRE(u.h.p.audition().toggleAB());
  u.settle();
  CHECK(u.h.p.undoSteps() == 0);
  REQUIRE(u.h.p.audition().apply());
  u.settle();
  const std::string applied = u.now();
  REQUIRE(applied != original);
  CHECK(u.h.p.undoSteps() == 1);
  REQUIRE(ctl.undo());
  u.settle();
  CHECK(u.now() == original);
  REQUIRE(ctl.redo());
  u.settle();
  CHECK(u.now() == applied);
}

TEST_CASE("undo: 65 edits keep the last 64; a new edit after an undo clears redo", "[undo][rig]") {
  Undo u;
  RigController ctl(u.h.p);
  std::vector<std::string> states;  // states[i] = the preset after edit i+1
  for (int i = 1; i <= 65; ++i) {
    ctl.edit([i](Preset& p) { p.a.levelDb = 0.1 * i; });
    u.settle();
    states.push_back(u.now());
  }
  CHECK(u.h.p.undoSteps() == 64);
  int undone = 0;
  while (ctl.undo()) ++undone;
  u.settle();
  CHECK(undone == 64);
  CHECK(u.now() == states[0]);  // the 65th edit dropped the oldest step: edit 1 can no longer be undone
  CHECK(u.h.p.redoSteps() == 64);
  REQUIRE(ctl.redo());
  u.settle();
  CHECK(u.now() == states[1]);
  // A new edit after an undo ends the redo branch.
  ctl.edit([](Preset& p) { p.a.levelDb = -7.0; });
  u.settle();
  CHECK_FALSE(ctl.canRedo());
  CHECK_FALSE(ctl.redo());
  CHECK(u.h.p.currentPreset().a.levelDb == Catch::Approx(-7.0));
}

TEST_CASE("undo: host automation records nothing and an undo never rewinds it; a user parameter gesture is one step", "[undo][rig]") {
  Undo u;
  RigController ctl(u.h.p);
  for (int i = 0; i < 8; ++i) u.h.setParam(kOutputGain, -1.0 * i);  // the host: no gesture
  CHECK(u.h.p.undoSteps() == 0);
  CHECK_FALSE(ctl.canUndo());
  // A user's drag (begin gesture, many values, end gesture) on another parameter: ONE step.
  auto* prm = u.h.p.parameters().getParameter(paramSpec(kInputGain).id);
  const double inBefore = u.h.param(kInputGain);
  REQUIRE(juce::MessageManager::existsAndIsCurrentThread());
  prm->beginChangeGesture();
  for (int i = 1; i <= 10; ++i) prm->setValueNotifyingHost(prm->convertTo0to1(static_cast<float>(inBefore + 0.5 * i)));
  prm->endChangeGesture();
  CHECK(u.h.p.undoSteps() == 1);
  const double inAfter = u.h.param(kInputGain);
  REQUIRE(inAfter != Catch::Approx(inBefore));
  u.h.setParam(kOutputGain, -9.0);  // the host automates OUTPUT again after the gesture
  REQUIRE(ctl.undo());
  u.settle();
  CHECK(u.h.param(kInputGain) == Catch::Approx(inBefore).margin(1e-6));
  CHECK(u.h.param(kOutputGain) == Catch::Approx(-9.0).margin(1e-6));  // not rewound
  REQUIRE(ctl.redo());
  u.settle();
  CHECK(u.h.param(kInputGain) == Catch::Approx(inAfter).margin(1e-6));
  CHECK(u.h.param(kOutputGain) == Catch::Approx(-9.0).margin(1e-6));
  // A gesture that changes nothing is no step.
  const std::size_t steps = u.h.p.undoSteps();
  prm->beginChangeGesture();
  prm->endChangeGesture();
  CHECK(u.h.p.undoSteps() == steps);
}

TEST_CASE("undo: a gesture holds many edits as one step, gestures nest, and an undo mid-gesture is refused", "[undo][rig]") {
  Undo u;
  RigController ctl(u.h.p);
  const std::string before = u.now();
  ctl.beginGesture();
  ctl.beginGesture();  // nested
  for (int i = 1; i <= 5; ++i) {
    ctl.edit([i](Preset& p) { p.a.levelDb = 0.5 * i; });
    ctl.eqLive(EqTarget::Post, 0, 1000.0 + 100.0 * i, 0.0, 1.0);
  }
  CHECK(u.h.p.undoSteps() == 0);
  CHECK_FALSE(ctl.undo());  // the drag's step is not complete
  ctl.endGesture();
  CHECK(u.h.p.undoSteps() == 0);
  ctl.endGesture();
  u.settle();
  CHECK(u.h.p.undoSteps() == 1);
  REQUIRE(ctl.undo());
  u.settle();
  CHECK(u.now() == before);
}

TEST_CASE("undo: the history is in the processor: it survives a controller (editor) being destroyed and rebuilt", "[undo][rig]") {
  Undo u;
  std::string before, after;
  {
    RigController ctl(u.h.p);
    before = u.now();
    ctl.edit([](Preset& p) { setBypass(p.a, 0, true); });
    u.settle();
    after = u.now();
  }
  CHECK(u.h.p.undoSteps() == 1);
  RigController again(u.h.p);
  REQUIRE(again.canUndo());
  REQUIRE(again.undo());
  u.settle();
  CHECK(u.now() == before);
  REQUIRE(again.redo());
  u.settle();
  CHECK(u.now() == after);
  // A state restore (a host session load) leaves the history alone and is no step.
  juce::MemoryBlock s;
  u.h.p.getStateInformation(s);
  u.h.p.setStateInformation(s.getData(), static_cast<int>(s.getSize()));
  u.settle();
  CHECK(u.h.p.undoSteps() == 1);
}

TEST_CASE("undo: the level-match trim never dips to 0 on undo / redo (edit steps and load steps)", "[undo][levelmatch]") {
  Undo u(R"({"levelMatch": true})");
  // A rig whose trim is clearly not 0: +6 dB of input gain.
  const fs::path loud = writeIdentityPreset(u.tmp.dir, "loud", 0);
  { std::ifstream in(loud); json j = json::parse(in); j["input"] = {{"gainDb", 6.0}}; std::ofstream(loud) << j.dump(2); }
  u.h.load(loud);
  u.h.p.levelTick();
  REQUIRE(u.h.p.waitForLevelWork());
  const double t0 = u.h.p.status().trimDb;
  REQUIRE(std::fabs(t0) > 1.0);
  RigController ctl(u.h.p);
  u.h.p.historyClear();

  std::vector<double> seen;
  const auto look = [&] { seen.push_back(u.h.p.status().trimDb); };
  const auto settleLooking = [&] {
    look();
    REQUIRE(u.h.p.waitForLoader());
    look();
    u.h.p.levelTick();
    look();
    REQUIRE(u.h.p.waitForLevelWork());
    look();
  };

  // An edit step that changes the measured sound (path A's level), undone and redone.
  ctl.edit([](Preset& p) { p.a.levelDb = -6.0; });
  look();
  settleLooking();
  const double t1 = u.h.p.status().trimDb;
  REQUIRE(ctl.undo());
  look();
  settleLooking();
  CHECK(u.h.p.status().trimDb == Catch::Approx(t0).margin(1e-6));
  REQUIRE(ctl.redo());
  look();
  settleLooking();
  CHECK(u.h.p.status().trimDb == Catch::Approx(t1).margin(1e-6));
  REQUIRE(ctl.undo());
  settleLooking();

  // A load step (a different preset): undoing it must not drop the trim to 0 either.
  const fs::path other = writeIdentityPreset(u.tmp.dir, "other", 0);
  { std::ifstream in(other); json j = json::parse(in); j["input"] = {{"gainDb", -4.0}}; j["name"] = "other"; std::ofstream(other) << j.dump(2); }
  REQUIRE(u.h.p.loadPresetFile(other, nullptr, /*undoable=*/true));  // a fresh preset starts at its own trim (0 until measured): not what is checked
  REQUIRE(u.h.p.waitForLoader());
  u.h.p.levelTick();
  REQUIRE(u.h.p.waitForLevelWork());
  seen.clear();
  REQUIRE(u.h.p.undoSteps() >= 1);
  REQUIRE(ctl.undo());
  look();
  settleLooking();
  CHECK(u.h.p.status().trimDb == Catch::Approx(t0).margin(1e-6));
  REQUIRE(ctl.redo());
  look();
  settleLooking();
  for (double t : seen) CHECK(t != 0.0);
}

namespace {

json ladderRungs() {
  return json::parse(R"([{"modelId":"m1","gain":2.0,"name":"Gain 2"},{"modelId":"m2","gain":5.0,"name":"Gain 5"},{"modelId":"m3","gain":8.0,"name":"Gain 8"}])");
}

// A path A amp that is a TONE3000 capture (T1 / m1), optionally with its ladder and active rung.
json ampPreset(bool ladder, const char* gainStep = nullptr, const char* toneId = "T1") {
  json model = {{"file", (kFixtures / "nam" / "linear_identity.nam").string()},
                {"source", {{"provider", "tone3000"}, {"id", toneId}, {"modelId", "m1"}, {"title", "Marshall A"}}}};
  if (ladder) model["ladder"] = ladderRungs();
  json a = {{"blocks", json::array({{{"id", "a1"}, {"type", "nam"}, {"slot", "amp"}, {"model", model}}})}};
  if (gainStep) a["ampControls"] = {{"gainStep", gainStep}};
  return {{"schema", "sawblade.preset"}, {"version", 3}, {"name", "amp"}, {"paths", {{"a", a}, {"b", {{"enabled", false}, {"blocks", json::array()}}}}},
          {"align", {{"mode", "off"}}}, {"blend", 0.0}, {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}};
}

fs::path writeJsonFile(const fs::path& dir, const std::string& name, const json& j) {
  const fs::path f = dir / (name + ".json");
  std::ofstream(f) << j.dump(2);
  return f;
}

std::vector<LadderRung> rungsOf(const json& j) {
  std::vector<LadderRung> v;
  for (const auto& r : j) v.push_back({r["modelId"].get<std::string>(), r["gain"].get<double>(), r["name"].get<std::string>()});
  return v;
}

}  // namespace

TEST_CASE("undo: a wheel notch burst is one step; notches apart are one step each (no flush per notch)", "[undo][rig]") {
  // (see test_undo_ui.cpp for the knob-level version; here: the controller's debounce)
  Undo u;
  RigController ctl(u.h.p);
  const std::string before = u.now();
  for (int i = 1; i <= 5; ++i) ctl.editDebounced([i](Preset& p) { p.a.levelDb = 0.5 * i; });
  CHECK(u.h.p.undoSteps() == 0);  // nothing flushed yet
  REQUIRE(ctl.flushTimerForTests());
  u.settle();
  CHECK(u.h.p.undoSteps() == 1);
  REQUIRE(ctl.undo());
  u.settle();
  CHECK(u.now() == before);
}

TEST_CASE("undo: a masked restore keeps the gain-ladder rung that follows an automated GAIN", "[undo][rig][ladder]") {
  Undo u;
  u.h.load(writeJsonFile(u.tmp.dir, "amp", ampPreset(true, "m1")));
  RigController ctl(u.h.p);
  u.h.p.historyClear();
  ctl.edit([](Preset& p) { p.b.levelDb = -1.0; });  // an unrelated step (GAIN is not in its mask)
  u.settle();
  // The host automates GAIN to another rung; the ladder write-back records the rung (simulated: what ladderWriteBack does).
  u.h.setParam(ampParam(0, kAmpGain), 9.0);
  u.h.p.applyLiveEdit([](Preset& p) { p.a.ampControls.gainStep = "m3"; });
  REQUIRE(u.h.p.currentPreset().a.ampControls.gainStep == "m3");
  REQUIRE(ctl.undo());
  u.settle();
  const Preset p = u.h.p.currentPreset();
  CHECK(p.b.levelDb == Catch::Approx(0.0));
  CHECK(u.h.param(ampParam(0, kAmpGain)) == Catch::Approx(9.0).margin(1e-6));  // the automation stands ...
  CHECK(p.a.ampControls.gainStep == "m3");                                      // ... and the rung still agrees with it
  REQUIRE(ctl.redo());
  u.settle();
  CHECK(u.h.p.currentPreset().a.ampControls.gainStep == "m3");
}

TEST_CASE("undo: a ladder written back later is patched into the stored snapshots; undoing an earlier step keeps it", "[undo][rig][ladder]") {
  Undo u;
  u.h.load(writeJsonFile(u.tmp.dir, "amp", ampPreset(false)));
  RigController ctl(u.h.p);
  u.h.p.historyClear();
  ctl.edit([](Preset& p) { p.b.levelDb = -1.0; });  // step 1: its snapshot has no ladder
  u.settle();
  REQUIRE(u.h.p.currentPreset().a.blocks.size() == 1);
  // The ladder arrives (what ladderTick does: load it keeping the monitor state, patch the history, no step).
  const auto rungs = rungsOf(ladderRungs());
  Preset p = u.h.p.editBasePreset();
  REQUIRE(applyLadderToPreset(p, "T1", rungs));
  u.h.p.loadPreset(std::move(p), /*keepMonitor=*/true);
  u.h.p.patchHistory([&](Preset& snap) { applyLadderToPreset(snap, "T1", rungs); });
  u.settle();
  CHECK(u.h.p.undoSteps() == 1);
  const auto ladderOf = [](const Preset& pr) {
    const auto* n = dynamic_cast<const NamBlockParams*>(pr.a.blocks[0].params.get());
    return n ? n->model.ladder.size() : std::size_t{0};
  };
  REQUIRE(ladderOf(u.h.p.currentPreset()) == 3);
  REQUIRE(ctl.undo());
  u.settle();
  CHECK(u.h.p.currentPreset().b.levelDb == Catch::Approx(0.0));
  CHECK(ladderOf(u.h.p.currentPreset()) == 3);  // not taken away
  REQUIRE(ctl.redo());
  u.settle();
  CHECK(ladderOf(u.h.p.currentPreset()) == 3);
}

TEST_CASE("undo: a fetched ladder is patched only into snapshots whose amp is that capture", "[undo][rig][ladder]") {
  // A step whose snapshot holds ANOTHER tone's amp (same model id, so only the tone id differs): patching the first tone's ladder must not
  // give it one, and undoing to it shows no ladder.
  Undo u;
  u.h.load(writeJsonFile(u.tmp.dir, "amp", ampPreset(false, nullptr, "OTHER")));
  RigController ctl(u.h.p);
  u.h.p.historyClear();
  ctl.edit([](Preset& p) { p.b.levelDb = -1.0; });
  u.settle();
  REQUIRE(u.h.p.undoSteps() == 1);
  const auto rungs = rungsOf(ladderRungs());
  u.h.p.patchHistory([&](Preset& snap) { applyLadderToPreset(snap, "T1", rungs); });
  REQUIRE(ctl.undo());
  u.settle();
  const auto* n = dynamic_cast<const NamBlockParams*>(u.h.p.currentPreset().a.blocks[0].params.get());
  REQUIRE(n != nullptr);
  CHECK(n->model.source->id == "OTHER");
  CHECK(n->model.ladder.empty());
  // The same with the matching tone does give the snapshot the ladder (the rule is the tone id).
  Undo v;
  v.h.load(writeJsonFile(v.tmp.dir, "amp", ampPreset(false)));
  RigController ctl2(v.h.p);
  v.h.p.historyClear();
  ctl2.edit([](Preset& p) { p.b.levelDb = -1.0; });
  v.settle();
  v.h.p.patchHistory([&](Preset& snap) { applyLadderToPreset(snap, "T1", rungs); });
  REQUIRE(ctl2.undo());
  v.settle();
  const auto* m = dynamic_cast<const NamBlockParams*>(v.h.p.currentPreset().a.blocks[0].params.get());
  REQUIRE(m != nullptr);
  CHECK(m->model.ladder.size() == 3);
}

TEST_CASE("undo: a live edit while a load is in flight is one step measured on the preset the user sees", "[undo][rig]") {
  Undo u;
  RigController ctl(u.h.p);
  const std::string before = u.now();
  u.h.p.loadPreset(u.h.p.currentPreset(), /*keepMonitor=*/true);  // in flight (not awaited)
  ctl.live([](Preset& p) { p.postEq[0].freq = 2000.0; });
  CHECK(u.h.p.undoSteps() == 1);
  u.settle();
  REQUIRE(ctl.undo());
  u.settle();
  CHECK(u.now() == before);
  CHECK_FALSE(ctl.canUndo());
}

TEST_CASE("undo: a stale gesture end after an abort cannot close a newer gesture; a destroyed controller leaves a newer one's flusher", "[undo][rig]") {
  Undo u;
  const auto stale = u.h.p.historyGestureBegin();
  u.h.p.historyAbortGestures();  // the editor went away mid-drag
  CHECK_FALSE(u.h.p.historyInGesture());
  const auto fresh = u.h.p.historyGestureBegin();
  u.h.p.historyGestureEnd(stale);  // the host's late end of the old gesture
  CHECK(u.h.p.historyInGesture());  // the newer gesture is still open
  u.h.p.historyGestureEnd(fresh);
  CHECK_FALSE(u.h.p.historyInGesture());
  // Controller A is replaced by B before A is destroyed: A's destructor leaves B's flusher in place.
  auto a = std::make_unique<RigController>(u.h.p);
  RigController b(u.h.p);
  a.reset();
  b.editDebounced([](Preset& p) { p.a.levelDb = -3.0; });
  REQUIRE(b.hasPending());
  const auto t = u.h.p.historyGestureBegin();  // the history flushes B's pending edit as a step of its own
  CHECK_FALSE(b.hasPending());
  CHECK(u.h.p.undoSteps() == 1);
  u.h.p.historyGestureEnd(t);
}
