// Tests of the JUCE-free rig model (plugin/src/rig/RigModel.*): topology, slots, EQ, cab, align, gate, comp.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>

#include "latency_stub.h"
#include "rig/RigModel.h"

using namespace sawblade;
using namespace sawblade::plugin::rig;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = SAWBLADE_FIXTURES_DIR;

json namFields(const char* file = "linear_identity.nam") {
  return {{"model", {{"file", (kFixtures / "nam" / file).string()}}}};
}

Block namBlock(const std::string& id, const std::string& slot = "amp", const char* file = "linear_identity.nam") {
  return makeBlock("nam", id, slot, kFixtures, namFields(file));
}

// A: [eq a2 (fx), eq a3 (pedal), nam a1 (amp)]  B: [nam b1 (amp)], blend 0.5, both paths on.
Preset rig() {
  Preset p;
  p.name = "rig";
  p.cab.ir.file = (kFixtures / "ir" / "impulse.wav").string();
  p.cab.ir.resolvedPath = p.cab.ir.file;
  p.align.mode = AlignMode::Off;
  REQUIRE(addBlock(p.a, 0, makeBlock("eq", "a2", "fx", kFixtures, flatEqFields())));
  REQUIRE(addBlock(p.a, 1, makeBlock("eq", "a3", "pedal", kFixtures, flatEqFields())));
  REQUIRE(addBlock(p.a, 2, namBlock("a1")));
  REQUIRE(addBlock(p.b, 0, namBlock("b1")));
  return p;
}

Preset roundTrip(const Preset& p) { return parsePreset(toJson(p), kFixtures); }

}  // namespace

TEST_CASE("RigModel: amp and pedal-slot definitions", "[rig][model]") {
  Preset p = rig();
  CHECK(ampIndex(p.a) == 2);
  CHECK(isPedalSlot(p.a, 0));
  CHECK_FALSE(isPedalSlot(p.a, 2));
  // The last block labelled "amp" wins over a later nam block.
  p.a.blocks[2].slot = "pedal";
  p.a.blocks[0].slot = "amp";
  CHECK(ampIndex(p.a) == 0);
  p.a.blocks[0].slot = "fx";
  p.a.blocks[2].slot = "amp";
  // No amp-labelled block and no nam: no amp, every block is a pedal slot.
  PathPreset eqOnly;
  eqOnly.blocks.push_back(makeBlock("eq", "e1", "fx", kFixtures, flatEqFields()));
  CHECK(ampIndex(eqOnly) == -1);
  CHECK(isPedalSlot(eqOnly, 0));
  CHECK(ampIndex(PathPreset{}) == -1);
}

TEST_CASE("RigModel: topology round trip keeps every block", "[rig][model][topology]") {
  Preset p = rig();
  const Preset original = p;
  CHECK(topologyOf(p) == Topology::Blend);

  setTopology(p, Topology::Single, 0.5);
  p = roundTrip(p);
  CHECK(topologyOf(p) == Topology::Single);
  CHECK_FALSE(p.b.enabled);
  CHECK(p.blend == 0.0);
  CHECK_FALSE(p.a.blocks[0].bypass);  // the first pedal slot stays active
  CHECK(p.a.blocks[1].bypass);        // later pedal slots are bypassed, not deleted
  CHECK_FALSE(p.a.blocks[2].bypass);  // the amp is never touched
  CHECK(p.b.blocks == original.b.blocks);  // path B is kept

  setTopology(p, Topology::SinglePlusTwoPedals, 0.5);
  p = roundTrip(p);
  CHECK(topologyOf(p) == Topology::SinglePlusTwoPedals);
  CHECK_FALSE(p.b.enabled);
  CHECK(p.blend == 0.0);
  CHECK_FALSE(p.a.blocks[1].bypass);

  setTopology(p, Topology::Blend, 0.3);
  p = roundTrip(p);
  CHECK(topologyOf(p) == Topology::Blend);
  CHECK(p.b.enabled);
  CHECK(p.blend == Catch::Approx(0.3));

  // Same blocks, ids, order and params as the start.
  REQUIRE(p.a.blocks.size() == original.a.blocks.size());
  for (std::size_t i = 0; i < p.a.blocks.size(); ++i) CHECK(p.a.blocks[i] == original.a.blocks[i]);
  CHECK(p.b == original.b);

  // A blend that is not 0 is left alone when going to Blend.
  Preset q = rig();
  q.blend = 0.8;
  setTopology(q, Topology::Blend, 0.3);
  CHECK(q.blend == 0.8);
}

TEST_CASE("RigModel: switching to Blend gives a preset with default level-match keys auto + constant loudness", "[rig][model][topology][levelmatch]") {
  Preset p = rig();
  setTopology(p, Topology::Single, 0.5);
  setTopology(p, Topology::Blend, 0.5);
  CHECK(p.levelMatch.mode == LevelMatchMode::Auto);
  CHECK(p.blendLaw == BlendLaw::ConstantLoudness);
  // Presets that carry their own values keep them.
  Preset q = rig();
  q.levelMatch = {LevelMatchMode::Manual, 1.0, 2.0};
  setTopology(q, Topology::Single, 0.5);
  setTopology(q, Topology::Blend, 0.5);
  CHECK(q.levelMatch.mode == LevelMatchMode::Manual);
  CHECK(q.blendLaw == BlendLaw::Linear);
}

TEST_CASE("RigModel: Single to SinglePlusTwoPedals with no bypassed pedal changes nothing", "[rig][model][topology]") {
  Preset p = rig();
  p.a.blocks.erase(p.a.blocks.begin() + 1);  // one pedal slot only
  setTopology(p, Topology::Single, 0.5);
  const Preset before = p;
  setTopology(p, Topology::SinglePlusTwoPedals, 0.5);
  CHECK(p == before);
  CHECK(topologyOf(p) == Topology::Single);  // the panel keeps the user's choice as UI state
}

TEST_CASE("RigModel: slots", "[rig][model][slots]") {
  Preset p = rig();
  CHECK(newBlockId(p, 'a') == "a4");
  CHECK(newBlockId(p, 'b') == "b2");
  p.b.blocks.push_back(makeBlock("eq", "b2", "fx", kFixtures, flatEqFields()));
  CHECK(newBlockId(p, 'b') == "b3");

  // add / remove / move
  const Block extra = makeBlock("eq", "x", "fx", kFixtures, flatEqFields());
  PathPreset path = p.a;
  CHECK(addBlock(path, 99, extra));  // clamped to the end
  CHECK(path.blocks.back().id == "x");
  CHECK(addBlock(path, 0, makeBlock("eq", "y", "fx", kFixtures, flatEqFields())));
  CHECK(path.blocks.front().id == "y");
  while (static_cast<int>(path.blocks.size()) < kMaxBlocksPerPath) CHECK(addBlock(path, 0, extra));
  CHECK_FALSE(addBlock(path, 0, extra));  // the 9th is refused
  CHECK(static_cast<int>(path.blocks.size()) == kMaxBlocksPerPath);

  PathPreset q = p.a;  // a2 a3 a1
  moveBlock(q, 2, 0);
  CHECK(q.blocks[0].id == "a1");
  CHECK(q.blocks[1].id == "a2");
  moveBlock(q, 0, 2);
  CHECK(q.blocks[2].id == "a1");
  moveBlock(q, 5, 0);  // out of range: ignored
  moveBlock(q, 0, 99);  // clamped to the last position
  CHECK(q.blocks[2].id == "a2");
  removeBlock(q, 0);
  removeBlock(q, 42);
  CHECK(q.blocks.size() == 2);
  setBypass(q, 0, true);
  CHECK(q.blocks[0].bypass);

  // input gain: nam only, clamped; the old params object is untouched (shared immutable)
  PathPreset g = p.a;
  const auto before = g.blocks[2].params;
  CHECK(setBlockInputGainDb(g, 2, 40.0));
  CHECK(static_cast<const NamBlockParams&>(*g.blocks[2].params).inputGainDb == kBlockGainMaxDb);
  CHECK(static_cast<const NamBlockParams&>(*before).inputGainDb == 0.0);
  CHECK(setBlockInputGainDb(g, 2, -3.5));
  CHECK(static_cast<const NamBlockParams&>(*g.blocks[2].params).inputGainDb == -3.5);
  CHECK_FALSE(setBlockInputGainDb(g, 0, 1.0));  // an eq block
  CHECK_FALSE(setBlockInputGainDb(g, 9, 1.0));

  // slot labels
  CHECK(defaultSlotFor(PathPreset{}, "nam") == "amp");
  CHECK(defaultSlotFor(p.a, "nam") == "pedal");
  CHECK(defaultSlotFor(p.a, "eq") == "fx");
  CHECK(defaultSlotFor(PathPreset{}, "pedal.x") == "pedal");
}

TEST_CASE("RigModel: makeBlock parses through the registry and reports errors", "[rig][model][slots]") {
  test::registerLatencyStub();
  const Block b = makeBlock("test.latency", "t1", "pedal", kFixtures, {{"latency", 12}});
  CHECK(b.type == "test.latency");
  CHECK(blockTitle(b) == "test.latency");
  CHECK(blockCredit(b).empty());
  CHECK_THROWS_WITH(makeBlock("test.latency", "t1", "pedal", kFixtures, json::object()),
                    Catch::Matchers::ContainsSubstring("latency"));
  CHECK_THROWS_WITH(makeBlock("nam", "n1", "amp", kFixtures, json::object()), Catch::Matchers::ContainsSubstring("model"));
  CHECK_THROWS_WITH(makeBlock("nope", "n1", "amp", kFixtures, json::object()), Catch::Matchers::ContainsSubstring("unknown"));
  CHECK_THROWS(makeBlock("eq", "e", "fx", kFixtures, {{"bands", json::array()}, {"bogus", 1}}));
  CHECK(makeBlock("eq", "e", "fx", kFixtures, flatEqFields()).slot == "fx");
}

TEST_CASE("RigModel: block title and credit come from the capture metadata", "[rig][model][display]") {
  Block b = namBlock("a1");
  CHECK(blockTitle(b) == "linear_identity");
  CHECK(blockCredit(b) == "LOCAL FILE");

  json f = namFields();
  f["model"]["source"] = {{"provider", "tone3000"}, {"id", "42"}, {"title", "Some Amp"}, {"creator", "someone"}, {"license", "cc-by"}};
  b = makeBlock("nam", "a1", "amp", kFixtures, f);
  CHECK(blockTitle(b) == "Some Amp");
  CHECK(blockCredit(b) == "@someone \xC2\xB7 cc-by \xC2\xB7 VIA TONE3000");

  const Block eq = makeBlock("eq", "e", "fx", kFixtures, flatEqFields());
  CHECK(blockTitle(eq) == "EQ 1 band");
  CHECK(blockCredit(eq).empty());
}

TEST_CASE("RigModel: EQ bands", "[rig][model][eq]") {
  Preset p = rig();
  for (const EqTarget t : {EqTarget::PreA, EqTarget::EqA, EqTarget::PreB, EqTarget::EqB, EqTarget::Post}) {
    CAPTURE(static_cast<int>(t));
    CHECK(eqBands(p, t).empty());
    while (static_cast<int>(eqBands(p, t).size()) < ParametricEq::kMaxBands) CHECK(addBand(p, t, EqBand{}));
    CHECK_FALSE(addBand(p, t, EqBand{}));
    removeBand(p, t, 0);
    removeBand(p, t, 99);
    CHECK(static_cast<int>(eqBands(p, t).size()) == ParametricEq::kMaxBands - 1);
  }
  CHECK(&eqBands(p, EqTarget::PreA) == &p.a.preEq);
  CHECK(&eqBands(p, EqTarget::EqB) == &p.b.eq);
  CHECK(&eqBands(p, EqTarget::Post) == &p.postEq);

  Preset q = rig();
  addBand(q, EqTarget::Post, EqBand{EqType::Peak, 1000.0, 0.0, 1.0, true});
  addBand(q, EqTarget::Post, EqBand{EqType::HighPass, 80.0, 0.0, 0.7, true});
  setBandLive(q, EqTarget::Post, 0, 5.0, 40.0, 100.0);  // clamps
  CHECK(q.postEq[0].freq == 20.0);
  CHECK(q.postEq[0].gainDb == 18.0);
  CHECK(q.postEq[0].q == 20.0);
  setBandLive(q, EqTarget::Post, 0, 1e6, -40.0, 0.01);
  CHECK(q.postEq[0].freq == 20000.0);
  CHECK(q.postEq[0].gainDb == -18.0);
  CHECK(q.postEq[0].q == 0.1);
  setBandLive(q, EqTarget::Post, 1, 120.0, 9.0, 2.0);  // pass filters have no gain
  CHECK(q.postEq[1].gainDb == 0.0);
  CHECK(q.postEq[1].freq == 120.0);
  const Preset before = q;
  setBandLive(q, EqTarget::Post, 5, 120.0, 9.0, 2.0);
  setBandLive(q, EqTarget::Post, 0, std::nan(""), 0.0, 1.0);
  CHECK(q == before);
  setBandType(q, EqTarget::Post, 0, EqType::LowPass);
  CHECK(q.postEq[0].type == EqType::LowPass);
  CHECK(q.postEq[0].gainDb == 0.0);
  setBandEnabled(q, EqTarget::Post, 0, false);
  CHECK_FALSE(q.postEq[0].enabled);
  CHECK(hasGain(EqType::LowShelf));
  CHECK_FALSE(hasGain(EqType::HighPass));
  CHECK(roundTrip(q) == q);
}

TEST_CASE("RigModel: cab", "[rig][model][cab]") {
  Preset p = rig();
  p.cab.ir.source = CaptureSource{"tone3000", "7", "", "", "Cab", "me", "cc-by"};
  setCabMode(p, CabMode::PerPath);
  CHECK(p.cab.mode == CabMode::PerPath);
  CHECK(p.cab.irA == p.cab.ir);
  CHECK(p.cab.irB == p.cab.ir);
  Capture other = p.cab.ir;
  other.file = (kFixtures / "ir" / "ir_a.wav").string();
  other.resolvedPath = other.file;
  other.source.reset();
  setCabIr(p, CabSlot::A, other);
  CHECK(p.cab.irA == other);
  CHECK(p.cab.irB != other);
  const Preset rt = roundTrip(p);  // perPath saves irA / irB only
  CHECK(rt.cab.irA == p.cab.irA);
  CHECK(rt.cab.irB == p.cab.irB);
  setCabMode(p, CabMode::Shared);
  CHECK(p.cab.ir == other);  // irA becomes the shared IR
  setCabMode(p, CabMode::Shared);
  CHECK(p.cab.ir == other);
  setCabEnabled(p, false);
  CHECK_FALSE(p.cab.enabled);
  setCabIr(p, CabSlot::Shared, p.cab.irB);
  CHECK(p.cab.ir == p.cab.irB);
  CHECK(captureTitle(other) == "ir_a");
  CHECK(captureCredit(other) == "LOCAL FILE");
}

TEST_CASE("RigModel: align", "[rig][model][align]") {
  Preset p = rig();
  p.align.mode = AlignMode::Auto;
  const AlignResult measured{-17, true, 0.93};
  nudgeAlign(p, 3, measured);  // Auto: becomes Manual seeded with the measured values, then moves
  CHECK(p.align.mode == AlignMode::Manual);
  CHECK(p.align.delaySamplesB == -14);
  CHECK(p.align.invertB);
  nudgeAlign(p, 10, AlignResult{99, false, 0.0});  // Manual: the measured values are ignored
  CHECK(p.align.delaySamplesB == -4);
  nudgeAlign(p, 100000);
  CHECK(p.align.delaySamplesB == kAlignMaxSamples);
  nudgeAlign(p, -100000);
  CHECK(p.align.delaySamplesB == -kAlignMaxSamples);
  setInvertB(p, false);
  CHECK_FALSE(p.align.invertB);

  Preset q = rig();
  q.align.mode = AlignMode::Auto;
  setInvertB(q, true, measured);
  CHECK(q.align.mode == AlignMode::Manual);
  CHECK(q.align.delaySamplesB == -17);
  CHECK(q.align.invertB);
  setAlignMode(q, AlignMode::Off);
  CHECK(q.align.mode == AlignMode::Off);
  nudgeAlign(q, 1);  // Off -> Manual from zero
  CHECK(q.align.mode == AlignMode::Manual);
  CHECK(q.align.delaySamplesB == 1);
  CHECK_FALSE(q.align.invertB);
}

TEST_CASE("RigModel: gate and comp setters clamp to the schema ranges", "[rig][model][gate]") {
  Preset p = rig();
  for (const GateField f : {GateField::Threshold, GateField::Hysteresis, GateField::Attack, GateField::Hold,
                            GateField::Release, GateField::Range, GateField::Ratio}) {
    CAPTURE(static_cast<int>(f));
    const Range r = gateRange(f);
    setGateField(p, f, r.lo - 1000.0);
    CHECK(gateField(p.gate, f) == r.lo);
    setGateField(p, f, r.hi + 1000.0);
    CHECK(gateField(p.gate, f) == r.hi);
    setGateField(p, f, std::nan(""));
    CHECK(gateField(p.gate, f) == r.hi);
  }
  setGateField(p, GateField::KeyHpf, 10.0);
  CHECK(p.gate.keyHighPassHz == 0.0);
  setGateField(p, GateField::KeyHpf, 30.0);
  CHECK(p.gate.keyHighPassHz == 40.0);
  setGateField(p, GateField::KeyHpf, 150.0);
  CHECK(p.gate.keyHighPassHz == 150.0);
  setGateField(p, GateField::KeyHpf, 900.0);
  CHECK(p.gate.keyHighPassHz == 400.0);
  setGateEnabled(p, true);
  setGateMode(p, GateMode::Expander);
  setGateReleaseCurve(p, GateReleaseCurve::LinearDb);
  CHECK(roundTrip(p) == p);  // everything stays within what the parser accepts

  for (const CompField f : {CompField::Threshold, CompField::Ratio, CompField::Knee, CompField::Attack, CompField::Release,
                            CompField::Makeup}) {
    CAPTURE(static_cast<int>(f));
    const Range r = compRange(f);
    setCompField(p, f, r.lo - 1000.0);
    CHECK(compField(p.busComp, f) == r.lo);
    setCompField(p, f, r.hi + 1000.0);
    CHECK(compField(p.busComp, f) == r.hi);
  }
  setCompEnabled(p, true);
  CHECK(roundTrip(p) == p);
}
