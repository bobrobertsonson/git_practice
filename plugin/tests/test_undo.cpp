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
