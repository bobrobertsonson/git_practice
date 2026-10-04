#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#include "sawblade/block_registry.h"
#include "sawblade/preset.h"
#include "sawblade/sha256.h"

using namespace sawblade;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kPresets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

json minimal() {
  return json::parse(R"({
    "schema": "sawblade.preset", "version": 1, "name": "t",
    "paths": {
      "a": { "blocks": [ { "id": "a1", "type": "nam", "model": { "file": "a.nam" } } ] },
      "b": { "blocks": [ { "id": "b1", "type": "nam", "model": { "file": "b.nam" } } ] }
    },
    "cab": { "mode": "shared", "ir": { "file": "cab.wav" } }
  })");
}

// Parses `j` and requires a PresetError whose JSON path is exactly `path` and whose message
// contains it.
void requireErrorAt(const json& j, const std::string& path) {
  try {
    (void)parsePreset(j, "/base");
    FAIL("expected PresetError at " << path);
  } catch (const PresetError& e) {
    INFO(e.what());
    REQUIRE(e.jsonPath() == path);
    REQUIRE_THAT(std::string(e.what()), ContainsSubstring(path));
  }
}

}  // namespace

TEST_CASE("SHA-256 known vectors", "[sha256]") {
  REQUIRE(sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  REQUIRE(sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  REQUIRE(sha256Hex(two.data(), two.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::string million(1000000, 'a');
  REQUIRE(sha256Hex(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // File hashing agrees with the in-memory one.
  const auto f = fs::temp_directory_path() / "sawblade_sha_test.bin";
  { std::ofstream(f, std::ios::binary) << "abc"; }
  REQUIRE(sha256File(f) == sha256Hex("abc", 3));
  fs::remove(f);
  REQUIRE_THROWS_AS(sha256File(f), std::runtime_error);
}

TEST_CASE("Preset: the schema document's example parses", "[preset]") {
  const Preset p = loadPresetFile(kPresets / "schema_example.json");
  REQUIRE(p.name == "Chainsaw + Body");
  REQUIRE(p.gate.enabled);
  REQUIRE(p.gate.thresholdDb == -55.0);
  REQUIRE(p.gate.hysteresisDb == 6.0);  // default
  REQUIRE(p.a.role == "saw");
  REQUIRE(p.a.blocks.size() == 2);
  REQUIRE(p.a.blocks[1].slot == "amp");
  REQUIRE(p.a.preEq.size() == 1);
  REQUIRE(p.a.preEq[0].type == EqType::HighPass);
  REQUIRE(p.b.blocks[0].slot == "boost");
  REQUIRE(p.align.mode == AlignMode::Auto);
  REQUIRE(p.align.maxLagMs == 5.0);
  REQUIRE(p.blend == 0.55);
  REQUIRE(p.cab.mode == CabMode::Shared);
  REQUIRE(p.cab.enabled);
  REQUIRE(p.cab.ir.file == "v30_4x12.wav");
  REQUIRE(p.postEq.size() == 1);
  REQUIRE(p.outputGainDb == -3.0);
  REQUIRE_FALSE(p.busComp.enabled);
  // Paths resolve against the preset file's directory.
  const auto& m = static_cast<const NamBlockParams&>(*p.a.blocks[0].params);
  REQUIRE(m.model.resolvedPath == fs::absolute(kPresets) / "hm2.nam");
}

TEST_CASE("Preset: defaults, omitted gate, absolute and relative paths", "[preset]") {
  json j = minimal();
  j["paths"]["a"]["blocks"][0]["model"]["file"] = "/abs/x.nam";
  const Preset p = parsePreset(j, "/base");
  REQUIRE_FALSE(p.gate.enabled);  // gate omitted -> disabled
  REQUIRE(p.align.mode == AlignMode::Auto);
  REQUIRE(p.blend == 0.5);
  REQUIRE(p.inputGainDb == 0.0);
  REQUIRE(p.a.enabled);
  REQUIRE(p.a.levelDb == 0.0);
  REQUIRE_FALSE(p.a.invert);
  REQUIRE(p.cab.normalize);
  REQUIRE(static_cast<const NamBlockParams&>(*p.a.blocks[0].params).model.resolvedPath == fs::path("/abs/x.nam"));
  REQUIRE(static_cast<const NamBlockParams&>(*p.b.blocks[0].params).model.resolvedPath == fs::path("/base/b.nam"));

  j["gate"] = json::object();  // present but empty -> enabled with defaults
  const Preset q = parsePreset(j, "/base");
  REQUIRE(q.gate.enabled);
  REQUIRE(q.gate.releaseMs == 60.0);
}

TEST_CASE("Preset: the plugin's playAlong UI state is accepted, ignored and never written", "[preset][playalong]") {
  json j = minimal();
  const Preset plain = parsePreset(j, "/base");
  j["playAlong"] = {{"folder", "/home/x/stems"}, {"offsetMs", 12.5}, {"loop", {{"on", true}}}, {"future", {1, 2, 3}}};
  const Preset withState = parsePreset(j, "/base");
  REQUIRE(withState == plain);
  REQUIRE_FALSE(toJson(withState).contains("playAlong"));
  j["playAlong"] = 3;  // still a structured object, not a free-form value
  REQUIRE_THROWS_AS(parsePreset(j, "/base"), PresetError);
}

TEST_CASE("Preset: round trip parse(toJson(p)) == p", "[preset]") {
  SECTION("schema example") {
    const Preset p = loadPresetFile(kPresets / "schema_example.json");
    REQUIRE(parsePreset(toJson(p), fs::absolute(kPresets)) == p);
  }
  SECTION("every feature") {
    json j = minimal();
    j["notes"] = "hello";
    j["input"] = {{"gainDb", 3.5}};
    j["gate"] = {{"enabled", true}, {"thresholdDb", -50.0}, {"holdMs", 12.0}};
    j["paths"]["a"]["role"] = "saw";
    j["paths"]["a"]["levelDb"] = -2.25;
    j["paths"]["a"]["invert"] = true;
    j["paths"]["a"]["enabled"] = false;
    j["paths"]["a"]["preEq"] = json::array({{{"type", "lowShelf"}, {"freq", 120.0}, {"gainDb", -2.0}, {"q", 0.5}, {"enabled", false}}});
    j["paths"]["a"]["blocks"][0]["slot"] = "pedal";
    j["paths"]["a"]["blocks"][0]["bypass"] = true;
    j["paths"]["a"]["blocks"][0]["inputGainDb"] = 1.5;
    j["paths"]["a"]["blocks"][0]["normalizeLoudness"] = true;
    j["paths"]["a"]["blocks"][0]["model"]["sha256"] = std::string(64, 'A');
    j["paths"]["a"]["blocks"][0]["model"]["source"] = {{"provider", "tone3000"}, {"id", "1"}, {"modelId", "m7"}, {"url", "https://x"},
                                                       {"title", "T"}, {"creator", "c"}, {"license", "CC-BY-4.0"}};
    j["paths"]["a"]["blocks"].push_back({{"id", "a2"}, {"type", "eq"}, {"bands", json::array({{{"type", "peak"}, {"freq", 900.0}, {"gainDb", 2.0}, {"q", 1.1}}})}});
    j["align"] = {{"mode", "manual"}, {"maxLagMs", 2.5}, {"delaySamplesB", -7}, {"invertB", true}};
    j["blend"] = 0.3;
    j["cab"] = {{"mode", "perPath"}, {"irA", {{"file", "a.wav"}}}, {"irB", {{"file", "b.wav"}}}, {"enabled", false}, {"normalize", false}};
    j["postEq"] = json::array({{{"type", "highPass"}, {"freq", 70.0}}});
    j["busComp"] = {{"enabled", true}, {"ratio", 4.0}, {"releaseMs", 200.0}};
    j["output"] = {{"gainDb", -1.0}};
    const Preset p = parsePreset(j, "/base");
    REQUIRE(p.a.blocks[0].params);
    REQUIRE(static_cast<const NamBlockParams&>(*p.a.blocks[0].params).model.sha256 == std::string(64, 'a'));
    const Preset q = parsePreset(toJson(p), "/base");
    REQUIRE(q == p);
    // Equality is not vacuous.
    Preset r = p;
    r.blend = 0.31;
    REQUIRE_FALSE(r == p);
    json j2 = toJson(p);
    j2["paths"]["a"]["blocks"][1]["bands"][0]["gainDb"] = 2.5;
    REQUIRE_FALSE(parsePreset(j2, "/base") == p);
  }
}

TEST_CASE("Preset: minimal source (provider + id) is accepted and round-trips", "[preset]") {
  json j = minimal();
  j["paths"]["a"]["blocks"][0]["model"]["source"] = {{"provider", "tone3000"}, {"id", "42"}, {"modelId", "v2"}};
  const Preset p = parsePreset(j, "/base");
  const auto& s = *static_cast<const NamBlockParams&>(*p.a.blocks[0].params).model.source;
  REQUIRE(s.modelId == "v2");
  REQUIRE(s.license.empty());
  REQUIRE(parsePreset(toJson(p), "/base") == p);
}

TEST_CASE("Preset: gate expander fields parse, round trip and validate", "[preset][gate]") {
  json j = minimal();
  j["gate"] = {{"mode", "expander"}, {"ratio", 8.0}, {"keyHighPassHz", 120.0}, {"releaseCurve", "linear-db"}};
  const Preset p = parsePreset(j, "/base");
  REQUIRE(p.gate.mode == GateMode::Expander);
  REQUIRE(p.gate.ratio == 8.0);
  REQUIRE(p.gate.keyHighPassHz == 120.0);
  REQUIRE(p.gate.releaseCurve == GateReleaseCurve::LinearDb);
  REQUIRE(parsePreset(toJson(p), "/base") == p);

  j = minimal(); j["gate"] = json::object();  // defaults preserve v1 behaviour
  const Preset d = parsePreset(j, "/base");
  REQUIRE(d.gate.mode == GateMode::Gate);
  REQUIRE(d.gate.ratio == 4.0);
  REQUIRE(d.gate.keyHighPassHz == 0.0);
  REQUIRE(d.gate.releaseCurve == GateReleaseCurve::OnePole);

  j = minimal(); j["gate"] = {{"mode", "bogus"}}; requireErrorAt(j, "gate.mode");
  j = minimal(); j["gate"] = {{"ratio", 1.0}}; requireErrorAt(j, "gate.ratio");
  j = minimal(); j["gate"] = {{"ratio", 11.0}}; requireErrorAt(j, "gate.ratio");
  j = minimal(); j["gate"] = {{"keyHighPassHz", 20.0}}; requireErrorAt(j, "gate.keyHighPassHz");
  j = minimal(); j["gate"] = {{"keyHighPassHz", 500.0}}; requireErrorAt(j, "gate.keyHighPassHz");
  j = minimal(); j["gate"] = {{"releaseCurve", "log"}}; requireErrorAt(j, "gate.releaseCurve");
}

TEST_CASE("Preset errors carry the JSON path", "[preset]") {
  SECTION("unknown keys") {
    json j = minimal(); j["bogus"] = 1; requireErrorAt(j, "bogus");
    j = minimal(); j["paths"]["a"]["bogus"] = 1; requireErrorAt(j, "paths.a.bogus");
    j = minimal(); j["paths"]["a"]["blocks"][0]["model"]["bogus"] = 1; requireErrorAt(j, "paths.a.blocks[0].model.bogus");
    j = minimal(); j["paths"]["a"]["blocks"][0]["bogus"] = 1; requireErrorAt(j, "paths.a.blocks[0].bogus");
    j = minimal(); j["gate"] = {{"thresh", -5}}; requireErrorAt(j, "gate.thresh");
    j = minimal(); j["cab"]["irA"] = {{"file", "x"}}; requireErrorAt(j, "cab.irA");  // shared mode
    j = minimal(); j["input"] = {{"gainDb", 1.0}, {"bogus", 1}}; requireErrorAt(j, "input.bogus");
    j = minimal(); j["output"] = {{"bogus", 1}}; requireErrorAt(j, "output.bogus");
    j = minimal(); j["align"] = {{"mode", "auto"}, {"bogus", 1}}; requireErrorAt(j, "align.bogus");
    j = minimal(); j["busComp"] = {{"enabled", true}, {"bogus", 1}}; requireErrorAt(j, "busComp.bogus");
    j = minimal(); j["cab"]["bogus"] = 1; requireErrorAt(j, "cab.bogus");
    j = minimal(); j["postEq"] = json::array({{{"type", "peak"}, {"freq", 100.0}, {"bogus", 1}}}); requireErrorAt(j, "postEq[0].bogus");
    j = minimal(); j["paths"]["c"] = json::object(); requireErrorAt(j, "paths.c");
    j = minimal();
    j["paths"]["a"]["blocks"].push_back({{"id", "a2"}, {"type", "eq"}, {"bands", json::array()}, {"bogus", 1}});
    requireErrorAt(j, "paths.a.blocks[1].bogus");
    j = minimal();
    j["paths"]["a"]["blocks"].push_back({{"id", "a2"}, {"type", "eq"}, {"bands", json::array({{{"type", "peak"}, {"freq", 100.0}, {"bogus", 1}}})}});
    requireErrorAt(j, "paths.a.blocks[1].bands[0].bogus");  // band object inside an eq block
  }
  SECTION("wrong types") {
    json j = minimal(); j["blend"] = "half"; requireErrorAt(j, "blend");
    j = minimal(); j["paths"]["b"]["blocks"][0]["model"]["file"] = 5; requireErrorAt(j, "paths.b.blocks[0].model.file");
    j = minimal(); j["paths"]["a"]["enabled"] = 1; requireErrorAt(j, "paths.a.enabled");
    j = minimal(); j["paths"]["a"]["blocks"] = json::object(); requireErrorAt(j, "paths.a.blocks");
    j = minimal(); j["paths"] = json::array(); requireErrorAt(j, "paths");
    j = minimal(); j["version"] = 1.5; requireErrorAt(j, "version");
    j = minimal(); j["align"] = {{"delaySamplesB", 1.5}}; requireErrorAt(j, "align.delaySamplesB");
    j = minimal(); j["paths"]["a"]["blocks"].push_back({{"id", "a2"}, {"type", "nam"}, {"model", json::array()}});
    requireErrorAt(j, "paths.a.blocks[1].model");
  }
  SECTION("missing required fields") {
    json j = minimal(); j.erase("name"); requireErrorAt(j, "name");
    j = minimal(); j.erase("cab"); requireErrorAt(j, "cab");
    j = minimal(); j.erase("version"); requireErrorAt(j, "version");
    j = minimal(); j["paths"].erase("b"); requireErrorAt(j, "paths.b");
    j = minimal(); j["paths"]["a"]["blocks"][0].erase("model"); requireErrorAt(j, "paths.a.blocks[0].model");
    j = minimal(); j["paths"]["a"]["blocks"][0]["model"].erase("file"); requireErrorAt(j, "paths.a.blocks[0].model.file");
    j = minimal(); j["paths"]["a"]["blocks"][0].erase("id"); requireErrorAt(j, "paths.a.blocks[0].id");
    j = minimal(); j["cab"] = {{"mode", "perPath"}, {"irA", {{"file", "a"}}}}; requireErrorAt(j, "cab.irB");
    j = minimal(); j["paths"]["a"]["blocks"].push_back({{"id", "a2"}, {"type", "eq"}}); requireErrorAt(j, "paths.a.blocks[1].bands");
    j = minimal(); j["paths"]["a"]["blocks"][0]["model"]["source"] = {{"provider", "tone3000"}};
    requireErrorAt(j, "paths.a.blocks[0].model.source.id");
    j = minimal(); j["paths"]["a"]["blocks"][0]["model"]["source"] = {{"provider", "tone3000"}, {"id", "9"}, {"extra", 1}};
    requireErrorAt(j, "paths.a.blocks[0].model.source.extra");
  }
  SECTION("out of range / invalid values") {
    json j = minimal(); j["blend"] = 1.5; requireErrorAt(j, "blend");
    j = minimal(); j["gate"] = {{"thresholdDb", 5.0}}; requireErrorAt(j, "gate.thresholdDb");
    j = minimal(); j["paths"]["a"]["eq"] = json::array({{{"type", "peak"}, {"freq", -1.0}}}); requireErrorAt(j, "paths.a.eq[0].freq");
    j = minimal(); j["postEq"] = json::array({{{"type", "peak"}, {"freq", 100.0}, {"q", 0.0}}}); requireErrorAt(j, "postEq[0].q");
    j = minimal(); j["postEq"] = json::array({{{"type", "wobble"}, {"freq", 100.0}}}); requireErrorAt(j, "postEq[0].type");
    j = minimal(); j["busComp"] = {{"ratio", 0.5}}; requireErrorAt(j, "busComp.ratio");
    j = minimal(); j["align"] = {{"mode", "sometimes"}}; requireErrorAt(j, "align.mode");
    j = minimal(); j["paths"]["a"]["role"] = "lead"; requireErrorAt(j, "paths.a.role");
    j = minimal(); j["paths"]["a"]["blocks"][0]["slot"] = "cab"; requireErrorAt(j, "paths.a.blocks[0].slot");
    j = minimal(); j["paths"]["a"]["blocks"][0]["model"]["sha256"] = "abc"; requireErrorAt(j, "paths.a.blocks[0].model.sha256");
    j = minimal(); j["paths"]["a"]["blocks"][0]["model"]["file"] = ""; requireErrorAt(j, "paths.a.blocks[0].model.file");
    j = minimal(); j["schema"] = "other"; requireErrorAt(j, "schema");
    j = minimal();
    for (int i = 0; i < 8; ++i) j["paths"]["a"]["blocks"].push_back({{"id", "x" + std::to_string(i)}, {"type", "eq"}, {"bands", json::array()}});
    requireErrorAt(j, "paths.a.blocks");  // 9 blocks
    j = minimal();
    json bands = json::array();
    for (int i = 0; i < 17; ++i) bands.push_back({{"type", "peak"}, {"freq", 100.0 + i}});
    j["postEq"] = bands;
    requireErrorAt(j, "postEq");
  }
  SECTION("version") {
    json j = minimal(); j["version"] = 2; requireErrorAt(j, "version");
    j["version"] = 0; requireErrorAt(j, "version");
  }
  SECTION("root must be an object") { REQUIRE_THROWS_AS(parsePreset(json::array(), "/b"), PresetError); }
}

TEST_CASE("Registry: unknown block type and duplicate ids", "[preset][registry]") {
  json j = minimal();
  j["paths"]["a"]["blocks"].push_back({{"id", "a2"}, {"type", "flanger"}});
  requireErrorAt(j, "paths.a.blocks[1].type");

  j = minimal();
  j["paths"]["a"]["blocks"][0]["id"] = "dup";
  j["paths"]["b"]["blocks"][0]["id"] = "dup";
  requireErrorAt(j, "paths.b.blocks[0].id");

  j = minimal();
  j["paths"]["a"]["blocks"].push_back({{"id", "a1"}, {"type", "eq"}, {"bands", json::array()}});
  requireErrorAt(j, "paths.a.blocks[1].id");

  REQUIRE(BlockRegistry::instance().find("nam") != nullptr);
  REQUIRE(BlockRegistry::instance().find("eq") != nullptr);
  REQUIRE(BlockRegistry::instance().find("nope") == nullptr);
  REQUIRE(BlockRegistry::instance().find("nam")->traits.namTrainable);
  REQUIRE_THROWS_AS(BlockRegistry::instance().add("nam", {}), std::invalid_argument);
}

TEST_CASE("loadPresetFile: invalid JSON and missing file", "[preset]") {
  const auto f = fs::temp_directory_path() / "sawblade_bad_preset.json";
  { std::ofstream(f) << "{ not json"; }
  REQUIRE_THROWS_AS(loadPresetFile(f), PresetError);
  fs::remove(f);
  REQUIRE_THROWS_AS(loadPresetFile(f), std::runtime_error);
}
