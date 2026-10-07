// Tests of the JUCE-free rig model (plugin/src/rig/RigModel.*): topology, slots, EQ, cab, align, gate, comp.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <filesystem>
#include <unistd.h>
#include <fstream>

#include "latency_stub.h"
#include "rig/CapturePedals.h"
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

// ---- v0.4 Task B: capture pedals -----------------------------------------------------------------------------------------------------
TEST_CASE("RigModel: a capture pedal is a nam block in a pedal slot; its LEVEL is the output gain", "[rig][model][pedalb]") {
  Preset p = rig();
  // A: [eq a2, eq a3, nam a1 (amp)]: no capture pedal yet; the amp capture is not a pedal
  for (int i = 0; i < 3; ++i) CHECK_FALSE(isCapturePedal(p.a, i));
  REQUIRE(addBlock(p.a, 1, namBlock("a4", "pedal")));
  CHECK(isCapturePedal(p.a, 1));
  CHECK_FALSE(isCapturePedal(p.a, 3));  // the amp
  CHECK_FALSE(isCapturePedal(p.a, 0));  // an EQ
  CHECK_FALSE(isCapturePedal(p.a, 9));

  CHECK(setBlockOutputGainDb(p.a, 1, 6.5));
  CHECK(static_cast<const NamBlockParams&>(*p.a.blocks[1].params).outputGainDb == 6.5);
  CHECK(setBlockOutputGainDb(p.a, 1, 99.0));
  CHECK(static_cast<const NamBlockParams&>(*p.a.blocks[1].params).outputGainDb == kBlockGainMaxDb);
  CHECK(setBlockOutputGainDb(p.a, 1, -99.0));
  CHECK(static_cast<const NamBlockParams&>(*p.a.blocks[1].params).outputGainDb == kBlockGainMinDb);
  CHECK(static_cast<const NamBlockParams&>(*p.a.blocks[1].params).inputGainDb == 0.0);  // the input gain is untouched
  CHECK_FALSE(setBlockOutputGainDb(p.a, 0, 3.0));  // an EQ has none
  CHECK_FALSE(setBlockOutputGainDb(p.a, 1, std::nan("")));
  CHECK_FALSE(setBlockOutputGainDb(p.a, 7, 1.0));
}

namespace {
// <cache>/<tone>/meta.json + one file per model, as `sawblade-t3k fetch` writes them.
void writeCachedTone(const fs::path& cache, const std::string& tone, const std::string& gear, const std::string& title, const std::vector<std::pair<std::string, std::string>>& models,
                     const std::string& ext = "nam") {
  fs::create_directories(cache / tone);
  json meta = {{"tone", {{"title", title}, {"gear", gear}, {"license", "cc-by-nc"}, {"url", "https://www.tone3000.com/tones/" + tone}, {"user", {{"username", "someone"}}}}},
               {"creatorUsername", "someone"}, {"models", json::object()}};
  for (const auto& [id, name] : models) {
    fs::copy_file(kFixtures / "nam" / "linear_identity.nam", cache / tone / (id + "." + ext), fs::copy_options::overwrite_existing);
    meta["models"][id] = {{"file", id + "." + ext}, {"sha256", "x"}, {"model", {{"name", name}}}};
  }
  std::ofstream(cache / tone / "meta.json") << meta.dump(2);
}
}  // namespace

TEST_CASE("CapturePedals: the cache lists pedal tones (nam files only), by title; settings come in ladder order", "[rig][model][pedalb]") {
  const fs::path cache = fs::temp_directory_path() / ("sawblade_capped_" + std::to_string(::getpid()));
  fs::remove_all(cache);
  writeCachedTone(cache, "11", "pedal", "Tight Boost", {{"112", "Gain 6"}, {"111", "Gain 2"}, {"113", "Gain 10"}});  // ids out of ladder order
  writeCachedTone(cache, "12", "pedal", "alpha drive", {{"121", "Standard"}});
  writeCachedTone(cache, "13", "amp", "Some Amp", {{"131", "Clean"}});
  writeCachedTone(cache, "14", "pedal", "IR-ish", {{"141", "x"}}, "wav");  // no .nam model: not a pedal capture
  writeCachedTone(cache, "15", "pedal", "Plain List", {{"152", "Bright"}, {"151", "Dark"}, {"1510", "Mid"}});
  fs::create_directories(cache / "99");  // no meta.json
  sawblade::setCaptureCacheRootOverride(cache);

  const auto tones = cachedPedalCaptures();
  REQUIRE(tones.size() == 3);
  CHECK(tones[0].toneId == "12");  // "alpha drive" < "Plain List" < "Tight Boost", case-insensitively
  CHECK(tones[1].toneId == "15");
  CHECK(tones[2].toneId == "11");
  CHECK(tones[2].title == "Tight Boost");
  CHECK(tones[2].creator == "someone");
  CHECK(tones[2].license == "cc-by-nc");
  REQUIRE(tones[2].models.size() == 3);
  CHECK(tones[2].models[0].name == "Gain 2");  // the v0.2 ladder order: by the number in the name
  CHECK(tones[2].models[1].name == "Gain 6");
  CHECK(tones[2].models[2].name == "Gain 10");
  REQUIRE(tones[1].models.size() == 3);        // no numbers in the names: the plain list, by model id (numerically)
  CHECK(tones[1].models[0].modelId == "151");
  CHECK(tones[1].models[1].modelId == "152");
  CHECK(tones[1].models[2].modelId == "1510");
  CHECK(tones[0].models.size() == 1);

  CHECK(cachedModelsOf("11").size() == 3);
  CHECK(cachedModelsOf("13").size() == 1);  // any gear
  CHECK(cachedModelsOf("99").empty());
  CHECK(cachedModelsOf("nope").empty());
  CHECK(cachedModelsOf("../x").empty());

  // the same cache gives a swap its capture (licence and creator kept)
  const auto c = cachedToneCapture("11", "112");
  REQUIRE(c.has_value());
  REQUIRE(c->source.has_value());
  CHECK(c->source->modelId == "112");
  CHECK(c->source->license == "cc-by-nc");
  CHECK(c->source->title == "Tight Boost");
  sawblade::setCaptureCacheRootOverride(std::nullopt);
  fs::remove_all(cache);
}

TEST_CASE("CapturePedals: a malformed or hostile cache entry is skipped, never thrown on and never read outside the tone's folder", "[rig][model][pedalb]") {
  const fs::path cache = fs::temp_directory_path() / ("sawblade_capmal_" + std::to_string(::getpid()));
  fs::remove_all(cache);
  writeCachedTone(cache, "20", "pedal", "Good One", {{"201", "Gain 1"}});
  const auto raw = [&](const std::string& tone, const std::string& text) {
    fs::create_directories(cache / tone);
    fs::copy_file(kFixtures / "nam" / "linear_identity.nam", cache / tone / "301.nam", fs::copy_options::overwrite_existing);
    std::ofstream(cache / tone / "meta.json") << text;
  };
  raw("21", "this is not json {");                                                                     // not JSON
  raw("22", R"([1, 2, 3])");                                                                           // not an object
  raw("23", R"({"tone": {"gear": "pedal", "title": "T"}, "models": [1, 2]})");                         // models an array
  raw("24", R"({"tone": "pedal", "models": {"301": {"file": "301.nam"}}})");                           // tone a string
  raw("25", R"({"tone": {"gear": "pedal", "title": "T"}, "models": {"301": {"file": 7}}})");           // file not a string: 301.nam by default
  raw("26", R"({"tone": {"gear": "pedal", "title": "T"}, "models": {"301": {"file": "gone.nam"}}})");  // the model file is missing
  raw("27", R"({"tone": {"gear": "pedal", "title": "T"}, "models": {"301": {"file": "../20/201.nam"}}})");  // traversal in the file
  raw("28", R"({"tone": {"gear": "pedal", "title": "T"}, "models": {"..": {"file": "301.nam"}, "a/b": {"file": "301.nam"}, "": {"file": "301.nam"}}})");  // traversal in the key
  raw("29", R"({"tone": {"gear": "pedal", "title": "T"}, "models": {"301": {"file": "sub\\301.nam"}}})");  // a backslash
  raw("30", R"({"tone": {"gear": "pedal", "title": "T"}, "models": {"301": "not an object"}})");
  sawblade::setCaptureCacheRootOverride(cache);

  std::vector<CachedPedal> tones;
  REQUIRE_NOTHROW(tones = cachedPedalCaptures());
  std::vector<std::string> ids;
  for (const auto& t : tones) ids.push_back(t.toneId);
  std::sort(ids.begin(), ids.end());
  CHECK(ids == std::vector<std::string>{"20", "25"});  // 25: a non-string file falls back to <model>.nam, like an absent one

  for (const char* t : {"21", "22", "23", "26", "27", "28", "29", "30"}) {
    INFO("tone " << t);
    CHECK(cachedModelsOf(t).empty());
    CHECK_FALSE(cachedToneCapture(t).has_value());
    CHECK_FALSE(cachedToneCapture(t, "301").has_value());
  }
  CHECK_NOTHROW(cachedModelsOf("24"));  // a tone that is a string: no gear, so no pedal; its model is still readable
  CHECK_NOTHROW(cachedModelsOf("25"));
  CHECK(cachedToneCapture("25", "301").has_value());
  CHECK_FALSE(cachedToneCapture("20", "..").has_value());
  CHECK_FALSE(cachedToneCapture("20", "../20/201").has_value());
  CHECK(plainCacheId("201"));
  CHECK_FALSE(plainCacheId(".."));
  CHECK_FALSE(plainCacheId(""));
  CHECK_FALSE(plainCacheId("a\\b"));
  CHECK(plainCacheFile("201.nam"));
  for (const char* bad : {"", "..", "../x.nam", "a/b.nam", "a\\b.nam", "x..nam", "a.b.nam"}) CHECK_FALSE(plainCacheFile(bad));
  sawblade::setCaptureCacheRootOverride(std::nullopt);
  fs::remove_all(cache);
}
