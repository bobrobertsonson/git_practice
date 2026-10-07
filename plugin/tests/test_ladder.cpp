// v0.2 Task B, plugin side: gain ladders in the Engine and the processor (rung preloading, GAIN -> rung, gainStep write-back,
// ladder fetch through `sawblade-t3k ladder`, the accessor Task D's UI reads). The core mechanics are in tests/test_gain_ladder.cpp.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <fstream>

#include "Engine.h"
#include "LadderFetch.h"
#include "rig/RigController.h"
#include "settings/Settings.h"
#include "PluginProcessor.h"
#include "alloc_guard.h"
#include "latency_stub.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::plugin;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

#include "processor_harness.h"

namespace {

struct LadderCache {
  fs::path dir;
  LadderCache() {
    static int n = 0;
    dir = fs::temp_directory_path() / ("sawblade_plugin_ladder_" + std::to_string(std::random_device{}()) + "_" + std::to_string(n++));
    fs::create_directories(dir / "T1");
    // Create Settings::shared() FIRST: its first creation clears the capture-cache override (applyCacheEnv), and a tool launch
    // (toolEnvironment) creates it lazily, which would silently drop this override mid-test.
    (void)sawblade::plugin::settings::Settings::shared();
    setCaptureCacheRootOverride(dir);
  }
  ~LadderCache() {
    setCaptureCacheRootOverride(std::nullopt);
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void put(const std::string& model, const char* fixture) const { fs::copy_file(kFixtures / "nam" / fixture, dir / "T1" / (model + ".nam"), fs::copy_options::overwrite_existing); }
};

struct EnvGuard {
  std::string name;
  std::optional<std::string> old;
  EnvGuard(const char* n, const std::string& v) : name(n) {
    if (const char* c = std::getenv(n)) old = c;
    ::setenv(n, v.c_str(), 1);
  }
  ~EnvGuard() {
    if (old) ::setenv(name.c_str(), old->c_str(), 1);
    else ::unsetenv(name.c_str());
  }
};

const char* const kLadderDoc =
    R"({"tone_id":"T1","size":"standard","rungs":[{"model_id":"m1","gain":2.0,"name":"Gain 2"},{"model_id":"m2","gain":5.0,"name":"Gain 5"},{"model_id":"m3","gain":8.0,"name":"Gain 8"}]})";

json ladderJson() {
  return json::parse(R"([{"modelId":"m1","gain":2.0,"name":"Gain 2"},{"modelId":"m2","gain":5.0,"name":"Gain 5"},{"modelId":"m3","gain":8.0,"name":"Gain 8"}])");
}

// Path A: an amp capture of tone T1 (own model `own`, file = identity fixture), blend 0 so only A is heard.
json ladderPreset(const std::string& own, bool withLadder, const json& amp = nullptr) {
  json model = {{"file", (kFixtures / "nam" / "linear_identity.nam").string()},
                {"source", {{"provider", "tone3000"}, {"id", "T1"}, {"modelId", own}}}};
  if (withLadder) model["ladder"] = ladderJson();
  json a = {{"blocks", json::array({{{"id", "a1"}, {"type", "nam"}, {"slot", "amp"}, {"model", model}}})}};
  if (!amp.is_null()) a["ampControls"] = amp;
  json idb = {{"id", "b1"}, {"type", "nam"}, {"model", {{"file", (kFixtures / "nam" / "linear_identity.nam").string()}}}};
  return {{"schema", "sawblade.preset"}, {"version", 2}, {"name", "ladder"},
          {"paths", {{"a", a}, {"b", {{"blocks", json::array({idb})}}}}},
          {"align", {{"mode", "off"}}}, {"blend", 0.0},
          {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}};
}

fs::path writePreset(const fs::path& dir, const std::string& name, const json& j) {
  const fs::path p = dir / (name + ".json");
  std::ofstream(p) << j.dump(2);
  return p;
}

ParamValues defaults() {
  ParamValues v{};
  for (int i = 0; i < kNumParams; ++i) v[static_cast<std::size_t>(i)] = paramSpec(i).def;
  return v;
}

}  // namespace

// The fake-tool tests run the tool for real: SAWBLADE_NO_NETWORK (set for every test by CMake and by the harness) is off in them.
#define ALLOW_FAKE_TOOL EnvGuard allowNetwork("SAWBLADE_NO_NETWORK", "0")

TEST_CASE("Ladder fetch: the tool's JSON is parsed strictly", "[ladder][plugin]") {
  auto r = parseLadderOutput(kLadderDoc);
  REQUIRE(r.ok);
  CHECK(r.toneId == "T1");
  REQUIRE(r.rungs.size() == 3);
  CHECK(r.rungs[1].modelId == "m2");  // ids are strings
  CHECK(r.rungs[1].gain == 5.0);
  CHECK(r.rungs[2].name == "Gain 8");
  // stderr noise around the document, pretty-printed JSON and unsorted rungs.
  r = parseLadderOutput(std::string("warning: slow network\n") + R"({
 "tone_id": "T1",
 "rungs": [{"model_id": "b", "gain": 9}, {"model_id": "a", "gain": 1.5, "name": "x"}]
})" + "\nbye\n");
  REQUIRE(r.ok);
  CHECK(r.rungs[0].modelId == "a");
  CHECK(r.rungs[1].modelId == "b");
  // "No ladder" is a valid answer with no rungs.
  r = parseLadderOutput(R"({"tone_id":"T1","size":"standard","rungs":null})");
  CHECK(r.ok);
  CHECK(r.rungs.empty());
  for (const char* bad : {"", "garbage", "{}", R"({"tone_id":"T1"})", R"({"tone_id":"T1","rungs":[]})",
                          R"({"tone_id":"T1","rungs":[{"model_id":"a","gain":1}]})",
                          R"({"tone_id":"T1","rungs":[{"model_id":"a","gain":1},{"model_id":"a","gain":2}]})",
                          R"({"tone_id":"T1","rungs":[{"model_id":"a","gain":1},{"model_id":"b","gain":1}]})",
                          R"({"tone_id":"T1","rungs":[{"model_id":5,"gain":1},{"model_id":"b","gain":2}]})",
                          R"({"tone_id":"T1","rungs":[{"model_id":"a","gain":-1},{"model_id":"b","gain":2}]})",
                          R"({"tone_id":"T1","rungs":[{"model_id":"a","gain":"1"},{"model_id":"b","gain":2}]})",
                          R"({"tone_id":"T1","rungs":"x"})"}) {
    CAPTURE(bad);
    CHECK_FALSE(parseLadderOutput(bad).ok);
  }
  CHECK(ladderArgs("123") == std::vector<std::string>{"ladder", "123", "--size", "standard", "--json"});
}

TEST_CASE("Ladder fetch: which captures need a ladder, and applying one", "[ladder][plugin]") {
  Preset p = parsePreset(ladderPreset("m3", false), kPresetDir);
  CHECK(toneIdsNeedingLadder(p) == std::vector<std::string>{"T1"});
  const auto doc = parseLadderOutput(kLadderDoc);
  REQUIRE(applyLadderToPreset(p, "T1", doc.rungs));
  const auto& m = static_cast<const NamBlockParams&>(*p.a.blocks[0].params).model;
  CHECK(m.ladder.size() == 3);
  CHECK(p.a.ampControls.gain == 10.0);  // own rung m3 sits at position 10: the knob is placed there
  CHECK(toneIdsNeedingLadder(p).empty());
  CHECK_FALSE(applyLadderToPreset(p, "T1", doc.rungs));  // already has one
  // A different tone, a ladder that does not contain the own model, or an explicit GAIN: no change / knob kept.
  Preset q = parsePreset(ladderPreset("m3", false), kPresetDir);
  CHECK_FALSE(applyLadderToPreset(q, "T2", doc.rungs));
  CHECK_FALSE(applyLadderToPreset(q, "T1", {{"x", 1.0, ""}, {"y", 2.0, ""}}));
  Preset r = parsePreset(ladderPreset("m3", false, json{{"gain", 3.5}}), kPresetDir);
  REQUIRE(applyLadderToPreset(r, "T1", doc.rungs));
  CHECK(r.a.ampControls.gain == 3.5);
  Preset s = parsePreset(ladderPreset("m3", false, json{{"gainStep", "m2"}}), kPresetDir);
  REQUIRE(applyLadderToPreset(s, "T1", doc.rungs));
  CHECK(s.a.ampControls.gain == 5.0);
  // A capture that is not from TONE3000 never needs one.
  json j = ladderPreset("m3", false);
  j["paths"]["a"]["blocks"][0]["model"].erase("source");
  CHECK(toneIdsNeedingLadder(parsePreset(j, kPresetDir)).empty());
}

TEST_CASE("Ladder engine: rung models are preloaded at build; GAIN moves the rung; no allocation; latency constant", "[ladder][plugin][engine][rt]") {
  const LadderCache cache;
  cache.put("m2", "linear_05_025.nam");
  cache.put("m3", "linear_identity.nam");
  const Preset p = parsePreset(ladderPreset("m1", true), kPresetDir);
  auto e = Engine::build(p, 48000.0, 512);
  const int lat = e->latencySamples();
  LadderState st = e->ladderState(0);
  REQUIRE(st.has);
  CHECK(st.rungCount == 3);
  CHECK(st.active == 0);
  CHECK_FALSE(e->ladderState(1).has);
  const auto x = noise(512, 3, 0.2f);
  std::vector<float> y(512);
  ParamValues v = defaults();
  e->setParams(v);
  e->process(x.data(), y.data(), 512);  // the preloaded batch is taken by the audio thread
  CHECK(e->ladderState(0).loadedMask == 0b111);
  {
    AllocGuard g;
    for (int i = 0; i < 500; ++i) {
      v[ampParam(0, kAmpGain)] = 5.0 + 5.0 * std::sin(i * 0.04);  // sweeps over every rung
      v[ampParam(0, kAmpTreble)] = 5.0 + 4.0 * std::cos(i * 0.09);
      e->setParams(v);
      e->process(x.data(), y.data(), 1 + (i * 61) % 512);
    }
    CHECK(g.count() == 0);
  }
  CHECK(e->latencySamples() == lat);
  v[ampParam(0, kAmpGain)] = 10.0;
  e->setParams(v);
  for (int i = 0; i < 10; ++i) e->process(x.data(), y.data(), 512);
  st = e->ladderState(0);
  CHECK(st.active == 2);
  CHECK_FALSE(st.pending);
  CHECK(e->latencySamples() == lat);
  CHECK(e->ladderMessages().empty());
}

TEST_CASE("Ladder engine: a rung that is not cached is pending (drive only) until refreshRungs finds it", "[ladder][plugin][engine]") {
  const LadderCache cache;
  cache.put("m2", "linear_05_025.nam");  // m3 is missing
  const Preset p = parsePreset(ladderPreset("m1", true), kPresetDir);
  auto e = Engine::build(p, 48000.0, 512);
  const auto x = sine(300.0, 48000.0, 512, 0.1);
  std::vector<float> y(512);
  ParamValues v = defaults();
  v[ampParam(0, kAmpGain)] = 10.0;
  e->setParams(v);
  for (int i = 0; i < 6; ++i) e->process(x.data(), y.data(), 512);
  LadderState st = e->ladderState(0);
  CHECK(st.target == 2);
  CHECK(st.pending);
  CHECK(st.active == 0);
  CHECK(e->refreshRungs(nullptr) == 1);  // m3 still not cached
  cache.put("m3", "linear_identity.nam");
  CHECK(e->refreshRungs(nullptr) == 0);
  for (int i = 0; i < 10; ++i) e->process(x.data(), y.data(), 512);
  st = e->ladderState(0);
  CHECK(st.active == 2);
  CHECK_FALSE(st.pending);
}

TEST_CASE("Ladder engine: a long ladder keeps the nearest eight rungs loaded", "[ladder][plugin][engine]") {
  const LadderCache cache;
  json l = json::array();
  for (int i = 0; i < 12; ++i) {
    l.push_back({{"modelId", "r" + std::to_string(i)}, {"gain", static_cast<double>(i)}});
    cache.put("r" + std::to_string(i), "linear_identity.nam");
  }
  json j = ladderPreset("r0", true);
  j["paths"]["a"]["blocks"][0]["model"]["ladder"] = l;
  j["paths"]["a"]["blocks"][0]["model"]["source"]["modelId"] = "r0";
  const Preset p = parsePreset(j, kPresetDir);
  auto e = Engine::build(p, 48000.0, 512);
  const auto x = noise(512, 3, 0.2f);
  std::vector<float> y(512);
  ParamValues v = defaults();
  e->setParams(v);
  e->process(x.data(), y.data(), 512);
  CHECK(__builtin_popcountll(e->ladderState(0).loadedMask) == kMaxLoadedRungs);
  CHECK((e->ladderState(0).loadedMask & 0xFF) == 0xFF);  // rungs 0..7 around the start
  v[ampParam(0, kAmpGain)] = 10.0;  // the top rung (11)
  e->setParams(v);
  e->process(x.data(), y.data(), 512);
  e->refreshRungs(nullptr);   // new centre: loads 4..11, evicts 0..3 (rung 0 sounds until the swap, so it stays)
  e->process(x.data(), y.data(), 512);
  for (int i = 0; i < 12; ++i) e->process(x.data(), y.data(), 512);
  e->refreshRungs(nullptr);
  e->process(x.data(), y.data(), 512);
  const LadderState st = e->ladderState(0);
  CHECK(st.active == 11);
  CHECK(((st.loadedMask >> 11) & 1ull) == 1);
  CHECK(__builtin_popcountll(st.loadedMask) <= kMaxLoadedRungs);
}

TEST_CASE("Ladder processor: GAIN moves the rung, the preset records gainStep, state round-trips the ladder", "[ladder][plugin][processor]") {
  const LadderCache cache;
  cache.put("m2", "linear_05_025.nam");
  cache.put("m3", "linear_identity.nam");
  TempDir t;
  std::ofstream(t.dir / "none.json") << json{{"t3kExecutable", (t.dir / "gone" / "sawblade-t3k").string()}}.dump();
  EnvGuard noSettings("SAWBLADE_SETTINGS_FILE", (t.dir / "none.json").string());
  Host h(48000.0, 512);
  h.p.setLadderFetchEnabled(false);
  h.load(writePreset(t.dir, "lad", ladderPreset("m1", true)));
  auto info = h.p.ladderInfo(0);
  REQUIRE(info.has);
  CHECK(info.rungCount == 3);
  CHECK(info.activeIndex == 0);
  CHECK(info.activeName == "Gain 2");
  CHECK(info.activeGain == 2.0);
  CHECK_FALSE(info.pending);
  CHECK_FALSE(h.p.ladderInfo(1).has);
  CHECK(h.p.currentPreset().a.ampControls.gainStep.empty());  // untouched: nothing recorded
  const std::uint64_t builds = h.p.engineBuilds();
  const int lat = h.p.getLatencySamples();

  const auto x = noise(48000, 5, 0.2f);
  std::vector<float> y;
  h.run(x, y, {512});
  h.p.ladderTick();
  h.setParam(ampParam(0, kAmpGain), 10.0);
  h.run(x, y, {512});
  info = h.p.ladderInfo(0);
  CHECK(info.targetIndex == 2);
  CHECK(info.activeIndex == 2);
  CHECK(info.activeName == "Gain 8");
  CHECK_FALSE(info.pending);
  h.p.ladderTick();  // writes the active rung back
  CHECK(h.p.currentPreset().a.ampControls.gainStep == "m3");
  CHECK(h.allocs == 0);
  CHECK(h.p.engineBuilds() == builds);  // moving GAIN never rebuilds
  CHECK(h.p.getLatencySamples() == lat);

  juce::MemoryBlock s1;
  h.p.getStateInformation(s1);
  const json st = json::parse(std::string(static_cast<const char*>(s1.getData()), s1.getSize()));
  CHECK(st["paths"]["a"]["ampControls"]["gainStep"] == "m3");
  CHECK(st["paths"]["a"]["ampControls"]["gain"].get<double>() == Catch::Approx(10.0));
  CHECK(st["paths"]["a"]["blocks"][0]["model"]["ladder"].size() == 3);
  CHECK(st["paths"]["a"]["blocks"][0]["model"]["ladder"][2]["modelId"] == "m3");
  // A restored session starts on the recorded rung (the engine is built from gainStep).
  Host b(48000.0, 512);
  b.p.setLadderFetchEnabled(false);
  b.p.setStateInformation(s1.getData(), static_cast<int>(s1.getSize()));
  REQUIRE(b.p.waitForLoader());
  info = b.p.ladderInfo(0);
  CHECK(info.activeIndex == 2);
  CHECK(info.activeModelId == "m3");
  CHECK(b.p.currentPreset().a.ampControls.gainStep == "m3");
}

TEST_CASE("Ladder processor: a ladder fetched through sawblade-t3k is stored, the knob placed, one rebuild", "[ladder][plugin][processor]") {
  ALLOW_FAKE_TOOL;
  const LadderCache cache;
  cache.put("m2", "linear_05_025.nam");
  cache.put("m3", "linear_identity.nam");
  TempDir t;
  const fs::path exe = t.dir / "fake-t3k";
  std::ofstream(exe) << "#!/bin/sh\necho 'note on stderr' >&2\necho '" << kLadderDoc << "'\n";
  fs::permissions(exe, fs::perms::owner_all);
  std::ofstream(t.dir / "settings.json") << json{{"t3kExecutable", exe.string()}}.dump();
  EnvGuard settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
  Host h(48000.0, 512);
  h.load(writePreset(t.dir, "nolad", ladderPreset("m3", false)));  // own model m3, no ladder yet
  CHECK_FALSE(h.p.ladderInfo(0).has);
  const std::uint64_t builds = h.p.engineBuilds();
  h.p.ladderTick();  // starts `sawblade-t3k ladder T1 --size standard --json`
  REQUIRE(h.p.waitForLadderWork());
  CHECK(h.p.ladderFetches() == 1);
  h.p.ladderTick();  // applies it: one rebuild
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.engineBuilds() == builds + 1);
  const auto info = h.p.ladderInfo(0);
  REQUIRE(info.has);
  CHECK(info.activeIndex == 2);  // the block's own capture (m3)
  CHECK(h.param(ampParam(0, kAmpGain)) == Catch::Approx(10.0));  // the knob sits at the own rung's position
  const Preset cur = h.p.currentPreset();
  CHECK(static_cast<const NamBlockParams&>(*cur.a.blocks[0].params).model.ladder.size() == 3);
  CHECK(cur.a.ampControls.gainStep.empty());
  // Asked once per tone per session: a further tick starts nothing.
  h.p.ladderTick();
  REQUIRE(h.p.waitForLadderWork());
  CHECK(h.p.ladderFetches() == 1);
  // The rebuilt rig sounds like the unladdered one (own rung, knob at its position: residual 0).
  Host plain(48000.0, 512);
  plain.p.setLadderFetchEnabled(false);
  plain.load(writePreset(t.dir, "nolad2", ladderPreset("m3", false)));
  const auto x = noise(20000, 9, 0.2f);
  std::vector<float> y1, y2;
  h.run(x, y1, {256});
  plain.run(x, y2, {256});
  CHECK(y1 == y2);
}

TEST_CASE("Ladder processor: no ladder when the tool says none, fails, or is missing", "[ladder][plugin][processor]") {
  ALLOW_FAKE_TOOL;
  const LadderCache cache;
  TempDir t;
  const auto tryTool = [&](const std::string& body, bool exists) {
    const fs::path exe = t.dir / "tool";
    fs::remove(exe);
    if (exists) {
      std::ofstream(exe) << "#!/bin/sh\n" << body << "\n";
      fs::permissions(exe, fs::perms::owner_all);
    }
    std::ofstream(t.dir / "settings.json") << json{{"t3kExecutable", exe.string()}}.dump();
    EnvGuard settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
    Host h(48000.0, 512);
    h.load(writePreset(t.dir, "x", ladderPreset("m3", false)));
    const std::uint64_t builds = h.p.engineBuilds();
    h.p.ladderTick();
    REQUIRE(h.p.waitForLadderWork());
    h.p.ladderTick();
    h.p.waitForLoader();
    CHECK_FALSE(h.p.ladderInfo(0).has);
    CHECK(h.p.engineBuilds() == builds);
    CHECK(h.p.currentPreset() == [&] { Preset p; h.p.loadPresetFile(t.dir / "x.json"); h.p.waitForLoader(); return h.p.currentPreset(); }());
    return h.p.ladderFetches();
  };
  CHECK(tryTool(R"(echo '{"tone_id":"T1","size":"standard","rungs":null}')", true) == 1);
  CHECK(tryTool("echo nonsense", true) == 1);
  CHECK(tryTool("echo boom >&2; exit 1", true) == 1);
  CHECK(tryTool("", false) == 0);  // no executable: nothing is started
}

TEST_CASE("Ladder processor: missing rung models are fetched one at a time through sawblade-t3k fetch, then picked up", "[ladder][plugin][processor]") {
  ALLOW_FAKE_TOOL;
  const LadderCache cache;  // empty: the own model m1 is the preset's file, m2 and m3 are missing
  TempDir t;
  const fs::path log = t.dir / "calls.log";
  const fs::path exe = t.dir / "fake-t3k";
  // `fetch <tone> --model <id> --json --cache-dir <dir>`: puts a fixture model into <dir>/<tone>/<id>.nam.
  std::ofstream(exe) << "#!/bin/sh\necho \"$@\" >> '" << log.string() << "'\nif [ \"$1\" = fetch ]; then mkdir -p \"$7/$2\"; cp '"
                     << (kFixtures / "nam" / "linear_identity.nam").string() << "' \"$7/$2/$4.nam\"; echo '{}'; fi\n";
  fs::permissions(exe, fs::perms::owner_all);
  std::ofstream(t.dir / "settings.json") << json{{"t3kExecutable", exe.string()}}.dump();
  EnvGuard settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
  Host h(48000.0, 512);
  h.load(writePreset(t.dir, "lad", ladderPreset("m1", true)));
  CHECK(h.p.ladderInfo(0).missingRungs == 0);
  const auto x = noise(4096, 3, 0.2f);
  std::vector<float> y;
  for (int i = 0; i < 8 && h.p.rungFetches() < 2; ++i) {
    h.p.ladderTick();  // starts at most one fetch
    CHECK(h.p.rungFetches() <= 2);
    REQUIRE(h.p.waitForLadderWork());
    h.run(x, y, {512});
  }
  CHECK(h.p.rungFetches() == 2);
  CHECK(h.p.ladderFetches() == 0);  // the preset already had its ladder
  CHECK(fs::exists(cache.dir / "T1" / "m2.nam"));
  CHECK(fs::exists(cache.dir / "T1" / "m3.nam"));
  h.p.ladderTick();  // the rung loader is asked at once
  REQUIRE(h.p.waitForLadderWork());
  h.run(x, y, {512});
  h.p.ladderTick();
  CHECK(h.p.rungFetches() == 2);  // never fetched twice
  h.setParam(ampParam(0, kAmpGain), 10.0);
  h.run(x, y, {512});
  h.run(x, y, {512});
  const auto info = h.p.ladderInfo(0);
  CHECK(info.activeIndex == 2);
  CHECK_FALSE(info.pending);
  // The calls were exactly the two fetches of the missing rungs (nearest the sounding rung first).
  std::ifstream in(log);
  std::string l1, l2, l3;
  std::getline(in, l1);
  std::getline(in, l2);
  CHECK(l1.find("fetch T1 --model m2 --json --cache-dir") == 0);
  CHECK(l2.find("fetch T1 --model m3 --json --cache-dir") == 0);
  CHECK_FALSE(std::getline(in, l3));
}

TEST_CASE("Ladder processor: no rung is fetched when ladder fetching is off or no tool exists", "[ladder][plugin][processor]") {
  ALLOW_FAKE_TOOL;
  const LadderCache cache;
  TempDir t;
  std::ofstream(t.dir / "none.json") << json{{"t3kExecutable", (t.dir / "gone" / "sawblade-t3k").string()}}.dump();
  EnvGuard settings("SAWBLADE_SETTINGS_FILE", (t.dir / "none.json").string());  // an executable that does not exist
  Host h(48000.0, 512);
  h.load(writePreset(t.dir, "lad", ladderPreset("m1", true)));
  h.p.ladderTick();
  CHECK(h.p.rungFetches() == 0);
  const fs::path exe = t.dir / "tool";
  std::ofstream(exe) << "#!/bin/sh\nexit 0\n";
  fs::permissions(exe, fs::perms::owner_all);
  std::ofstream(t.dir / "s2.json") << json{{"t3kExecutable", exe.string()}}.dump();
  EnvGuard s2("SAWBLADE_SETTINGS_FILE", (t.dir / "s2.json").string());
  h.p.setLadderFetchEnabled(false);
  h.p.ladderTick();
  CHECK(h.p.rungFetches() == 0);
}

TEST_CASE("Network opt-out: SAWBLADE_NO_NETWORK blocks the ladder fetch, the rung fetch and the body-path tool runs", "[ladder][plugin][processor][nonetwork]") {
  const LadderCache cache;
  TempDir t;
  const fs::path log = t.dir / "calls.log";
  const fs::path exe = t.dir / "fake-t3k";
  std::ofstream(exe) << "#!/bin/sh\necho \"$@\" >> '" << log.string() << "'\necho '{}'\n";
  fs::permissions(exe, fs::perms::owner_all);
  std::ofstream(t.dir / "settings.json") << json{{"t3kExecutable", exe.string()}}.dump();
  EnvGuard settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
  const auto nCalls = [&] {
    std::ifstream in(log);
    int n = 0;
    for (std::string l; std::getline(in, l);) ++n;
    return n;
  };
  {
    EnvGuard off("SAWBLADE_NO_NETWORK", "1");
    CHECK(networkToolsDisabled());
    // 1. ladder fetch: a tone3000 amp capture without a ladder.
    Host h(48000.0, 512);
    h.load(writePreset(t.dir, "nolad", ladderPreset("m3", false)));
    h.p.ladderTick();
    CHECK(h.p.waitForLadderWork());
    CHECK(h.p.ladderFetches() == 0);
    // 2. rung fetch: a ladder whose rungs m2 / m3 are not cached.
    Host h2(48000.0, 512);
    h2.load(writePreset(t.dir, "lad", ladderPreset("m1", true)));
    for (int i = 0; i < 3; ++i) h2.p.ladderTick();
    CHECK(h2.p.waitForLadderWork());
    CHECK(h2.p.rungFetches() == 0);
    // 3. BLEND body path: no suggest-body, no fallback fetch.
    rig::RigController ctl(h2.p);
    ctl.bodyFill().begin(h2.p.currentPreset());
    CHECK(ctl.bodyFill().toolRuns() == 0);
    CHECK_FALSE(ctl.bodyFill().active());
    CHECK(nCalls() == 0);
  }
  {  // The opt-out off: the same tool is started (the check above is not vacuous).
    EnvGuard on("SAWBLADE_NO_NETWORK", "0");
    CHECK_FALSE(networkToolsDisabled());
    Host h(48000.0, 512);
    h.load(writePreset(t.dir, "nolad2", ladderPreset("m3", false)));
    h.p.ladderTick();
    REQUIRE(h.p.waitForLadderWork());
    CHECK(h.p.ladderFetches() == 1);
    CHECK(nCalls() >= 1);
  }
}

TEST_CASE("Ladder processor: an own model that is not in the fetched ladder is reported", "[ladder][plugin][processor]") {
  ALLOW_FAKE_TOOL;
  const LadderCache cache;
  TempDir t;
  const fs::path exe = t.dir / "fake-t3k";
  std::ofstream(exe) << "#!/bin/sh\necho '" << kLadderDoc << "'\n";
  fs::permissions(exe, fs::perms::owner_all);
  std::ofstream(t.dir / "settings.json") << json{{"t3kExecutable", exe.string()}}.dump();
  EnvGuard settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
  Host h(48000.0, 512);
  h.load(writePreset(t.dir, "other", ladderPreset("other-size-model", false)));  // not m1 / m2 / m3
  h.p.ladderTick();
  REQUIRE(h.p.waitForLadderWork());
  h.p.ladderTick();
  CHECK_FALSE(h.p.ladderInfo(0).has);
  const auto msgs = h.p.ladderMessages();
  REQUIRE(msgs.size() == 1);
  CHECK(msgs[0].find("T1") != std::string::npos);
  CHECK(msgs[0].find("standard") != std::string::npos);
}
