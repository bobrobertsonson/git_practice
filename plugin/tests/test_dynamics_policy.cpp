// Task G.3: the live dynamics policy in the plugin. Plugin presets without a dynamicsMode play their live set; the RECORD DYNAMICS
// toggle (SawbladeProcessor::setDynamicsMode) hands the whole set (gate + bus comp) to the audio thread as one object; gate / comp
// edits go to the active set; the export follows the active set.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <vector>

#include "ExportGlue.h"
#include "ExportNotes.h"
#include "PluginProcessor.h"
#include "PresetMapping.h"
#include "alloc_guard.h"
#include "latency_stub.h"
#include "lock_guard.h"
#include "rig/RigModel.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::plugin;
using nlohmann::json;
namespace fs = std::filesystem;

#include "processor_harness.h"

namespace {

// The identity preset of the harness plus a record gate (hold 10 / release 20) and a bus comp, written by the "matcher".
fs::path writeDynPreset(const fs::path& dir, const std::string& origin, bool withMode = false) {
  const fs::path f = writeIdentityPreset(dir, "dyn_" + origin, 0);
  std::ifstream in(f);
  json j = json::parse(in);
  in.close();
  j["version"] = 4;
  j["origin"] = origin;
  if (withMode) j["dynamicsMode"] = "record";
  j["gate"] = {{"enabled", true}, {"thresholdDb", -50.0}, {"holdMs", 10.0}, {"releaseMs", 20.0}};
  j["busComp"] = {{"enabled", true}, {"thresholdDb", -20.0}, {"ratio", 4.0}, {"releaseMs", 100.0}};
  std::ofstream(f) << j.dump(2);
  return f;
}

Preset parseFile(const fs::path& f) { return sawblade::loadPresetFile(f); }

}  // namespace

TEST_CASE("Dynamics policy: a preset with no dynamicsMode plays live in the plugin; an explicit mode is kept", "[dynamics][processor]") {
  TempDir t;
  Host h(48000.0, 512);
  h.load(writeDynPreset(t.dir, "match"));
  CHECK(h.p.dynamicsMode() == DynamicsMode::Live);
  const Preset cur = h.p.currentPreset();
  CHECK(cur.dynamicsMode == DynamicsMode::Live);  // written back: the plugin state is the preset

  Host r(48000.0, 512);
  r.load(writeDynPreset(t.dir, "user", /*withMode=*/true));
  CHECK(r.p.dynamicsMode() == DynamicsMode::Record);
}

TEST_CASE("Dynamics policy: the toggle hands the whole set over between blocks, with no allocation and no lock", "[dynamics][processor][alloc]") {
  TempDir t;
  Host h(48000.0, 512);
  h.load(writeDynPreset(t.dir, "match"));
  const Preset cur = h.p.currentPreset();
  const DynamicsSet live = liveDynamicsOf(cur), rec = recordDynamicsOf(cur);
  REQUIRE_FALSE(live == rec);

  std::vector<float> x(512, 0.01f), y(512);
  h.process(x.data(), y.data(), 512);
  CHECK(h.p.engineParamState().live.dynamics == live);

  h.allocs = h.locks = 0;
  h.p.setDynamicsMode(DynamicsMode::Record);
  h.process(x.data(), y.data(), 512);
  CHECK(h.p.engineParamState().live.dynamics == rec);  // never half: gate and comp changed together in one block boundary
  h.p.setDynamicsMode(DynamicsMode::Live);
  h.process(x.data(), y.data(), 512);
  CHECK(h.p.engineParamState().live.dynamics == live);
  CHECK(h.allocs == 0);
  CHECK(h.locks == 0);
  CHECK_FALSE(h.nonFinite);
}

TEST_CASE("Dynamics policy: the mode survives the state round trip", "[dynamics][processor]") {
  TempDir t;
  Host a(48000.0, 512);
  a.load(writeDynPreset(t.dir, "match"));
  a.p.setDynamicsMode(DynamicsMode::Record);
  juce::MemoryBlock s;
  a.p.getStateInformation(s);
  const json j = json::parse(std::string(static_cast<const char*>(s.getData()), s.getSize()));
  CHECK(j["dynamicsMode"] == "record");
  CHECK(j["version"] == 4);
  CHECK(j["origin"] == "match");

  SawbladeProcessor b;
  b.setStateInformation(s.getData(), static_cast<int>(s.getSize()));
  b.setRateAndBufferSizeDetails(48000.0, 512);
  b.prepareToPlay(48000.0, 512);
  REQUIRE(b.waitForLoader());
  CHECK(b.dynamicsMode() == DynamicsMode::Record);
}

TEST_CASE("Dynamics policy: the gate threshold parameter follows the active set", "[dynamics][mapping]") {
  Preset p = parseFile(writeDynPreset(TempDir().dir, "user"));
  p.dynamicsMode = DynamicsMode::Live;
  p.liveDynamics = recordDynamicsOf(p);
  p.liveDynamics->gate.thresholdDb = -61.0;
  CHECK(paramsFromPreset(p)[kGateThreshold] == Catch::Approx(-61.0));
  p.dynamicsMode = DynamicsMode::Record;
  CHECK(paramsFromPreset(p)[kGateThreshold] == Catch::Approx(-50.0));
  ParamValues v = paramsFromPreset(p);
  v[kGateThreshold] = -40.0;
  applyParams(p, v);  // record mode edits the record gate, never the live one
  CHECK(p.gate.thresholdDb == -40.0);
  CHECK(p.liveDynamics->gate.thresholdDb == -61.0);
  p.dynamicsMode = DynamicsMode::Live;
  v[kGateThreshold] = -45.0;
  applyParams(p, v);
  CHECK(p.liveDynamics->gate.thresholdDb == -45.0);
  CHECK(p.gate.thresholdDb == -40.0);
  Preset fresh;  // no dynamicsMode: the file-format default is record, the plugin plays live
  CHECK_FALSE(fresh.dynamicsMode.has_value());
  CHECK(clampedToParams(fresh).dynamicsMode == DynamicsMode::Live);
}

TEST_CASE("Dynamics policy: gate and comp edits go to the active set", "[dynamics][rig]") {
  Preset p = parseFile(writeDynPreset(TempDir().dir, "match"));
  const DynamicsSet recBefore = recordDynamicsOf(p);

  p.dynamicsMode = DynamicsMode::Live;
  rig::setGateField(p, rig::GateField::Hold, 90.0);
  rig::setCompEnabled(p, true);
  rig::setCompField(p, rig::CompField::Ratio, 3.0);
  REQUIRE(p.liveDynamics.has_value());  // the derived live set was made explicit
  CHECK(p.liveDynamics->gate.holdMs == 90.0);
  CHECK(p.liveDynamics->gate.thresholdMode == GateThresholdMode::FloorRelative);  // the rest of the derived set is kept
  CHECK(p.liveDynamics->busComp.enabled);
  CHECK(p.liveDynamics->busComp.ratio == 3.0);
  CHECK(recordDynamicsOf(p) == recBefore);  // the record set is untouched

  p.dynamicsMode = DynamicsMode::Record;
  rig::setGateField(p, rig::GateField::Hold, 33.0);
  CHECK(p.gate.holdMs == 33.0);
  CHECK(p.liveDynamics->gate.holdMs == 90.0);
}

TEST_CASE("Dynamics policy: the export follows the active set", "[dynamics][export]") {
  Preset p = parseFile(writeDynPreset(TempDir().dir, "match"));
  p.cab.enabled = false;
  p.dynamicsMode = DynamicsMode::Live;  // a matched preset: live comp off
  RigSummary live = summariseRig(p);
  CHECK(live.dynamics == "live");
  CHECK_FALSE(live.compOn);
  CHECK(resolveDynamics(p).busComp.enabled == false);
  CHECK(json(buildExportNotes(p, "nocab", false))["dynamics"] == "live");

  p.dynamicsMode = DynamicsMode::Record;
  RigSummary rec = summariseRig(p);
  CHECK(rec.dynamics == "record");
  CHECK(rec.compOn);
  CHECK(rec.compReleaseMs == 100.0);
  CHECK(json(buildExportNotes(p, "nocab", false))["dynamics"] == "record");
}

TEST_CASE("Dynamics policy: a host threshold move in live mode leaves the record gate alone", "[dynamics][mapping]") {
  Preset p = parseFile(writeDynPreset(TempDir().dir, "match"));
  p.dynamicsMode = DynamicsMode::Live;  // live set derived, not explicit
  REQUIRE_FALSE(p.liveDynamics.has_value());
  ParamValues v = paramsFromPreset(p);
  applyParams(p, v);  // an unchanged value makes nothing explicit
  CHECK_FALSE(p.liveDynamics.has_value());
  v[kGateThreshold] = -43.0;
  applyParams(p, v);
  REQUIRE(p.liveDynamics.has_value());
  CHECK(p.liveDynamics->gate.thresholdDb == -43.0);
  CHECK(p.liveDynamics->gate.thresholdMode == GateThresholdMode::FloorRelative);  // still the derived set otherwise
  CHECK(p.gate.thresholdDb == -50.0);  // the record gate is untouched
}

TEST_CASE("Dynamics policy: toggling the mode invalidates the applied-candidate export source", "[dynamics][audition]") {
  TempDir t;
  Host h(48000.0, 512);
  h.load(writeIdentityPreset(t.dir, "orig", 0));
  const fs::path cand = writeDynPreset(t.dir, "match");
  std::string err;
  REQUIRE(h.p.audition().audition(cand, &err));
  REQUIRE(h.p.audition().apply());
  REQUIRE(h.p.waitForLoader());
  REQUIRE(h.p.audition().currentCandidateFile().has_value());  // applied candidate == current preset (live by default)
  h.p.setDynamicsMode(DynamicsMode::Record);
  CHECK_FALSE(h.p.audition().currentCandidateFile().has_value());  // the file says live, the rig records: not the same preset
}
