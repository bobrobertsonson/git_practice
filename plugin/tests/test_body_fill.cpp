// v0.2 Task C, plugin side: BLEND on an empty path B fills it with a TS boost and a body amp (fallback now, the pool's suggestion
// asynchronously), level match auto, as one undo step. The suggestion rule itself is tested in match/tests.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <functional>
#include <fstream>

#include "PluginProcessor.h"
#include "sawblade/sha256.h"
#include "ExportGlue.h"
#include "presets/PresetLibrary.h"
#include "rig/BodyFill.h"
#include "rig/RigController.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::plugin;
using namespace sawblade::plugin::rig;
using nlohmann::json;
namespace fs = std::filesystem;

#include "processor_harness.h"

namespace {

struct BfEnv {
  fs::path dir;
  std::string name;
  std::optional<std::string> old;
  BfEnv(const char* n, const std::string& v) : name(n) {
    if (const char* c = std::getenv(n)) old = c;
    ::setenv(n, v.c_str(), 1);
  }
  ~BfEnv() {
    if (old) ::setenv(name.c_str(), old->c_str(), 1);
    else ::unsetenv(name.c_str());
  }
};

// Every test here runs a fake tool for real: the opt-out SAWBLADE_NO_NETWORK (set for all tests) is off for them.
struct AllowTool {
  BfEnv env{"SAWBLADE_NO_NETWORK", "0"};
};

// A settings file whose t3kExecutable does not exist (never "the default venv tool is absent").
struct NoTool {
  BfEnv env;
  explicit NoTool(const fs::path& dir) : env("SAWBLADE_SETTINGS_FILE", (dir / "settings_none.json").string()) {
    std::ofstream(dir / "settings_none.json") << json{{"t3kExecutable", (dir / "no-such-dir" / "sawblade-t3k").string()}}.dump();
  }
};

struct BfCache {
  fs::path dir;
  BfCache() {
    static int n = 0;
    dir = fs::temp_directory_path() / ("sawblade_bodyfill_cache_" + std::to_string(std::random_device{}()) + "_" + std::to_string(n++));
    fs::create_directories(dir);
    setCaptureCacheRootOverride(dir);
  }
  ~BfCache() {
    setCaptureCacheRootOverride(std::nullopt);
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  // A cached model as `sawblade-t3k fetch` leaves it: the file and <tone>/meta.json (tone title / creator / licence / url, model sha256).
  void put(const std::string& tone, const std::string& model, const std::string& license = "cc-by") const {
    fs::create_directories(dir / tone);
    fs::copy_file(kFixtures / "nam" / "linear_identity.nam", dir / tone / (model + ".nam"), fs::copy_options::overwrite_existing);
    json meta;
    std::ifstream in(dir / tone / "meta.json");
    if (in) meta = json::parse(in);
    meta["tone"] = {{"title", "Tone " + tone}, {"url", "https://www.tone3000.com/tones/" + tone}, {"license", license},
                    {"user", {{"username", "user" + tone}, {"display_name", "Creator " + tone}}}};
    meta["creatorUsername"] = "user" + tone;
    meta["models"][model] = {{"file", model + ".nam"}, {"sha256", sha256File(dir / tone / (model + ".nam"))}};
    std::ofstream(dir / tone / "meta.json") << meta.dump(2);
  }
};

// A fake `sawblade-t3k`: logs its arguments; suggest-body prints `suggest.txt`; fetch copies a fixture model into the cache
// and prints the JSON the real tool prints.
struct FakeTool {
  fs::path exe, log, suggest;
  explicit FakeTool(const fs::path& dir, const std::string& suggestText, int delaySeconds = 0) {
    exe = dir / "fake-t3k";
    log = dir / "calls.log";
    suggest = dir / "suggest.txt";
    setSuggest(suggestText);
    std::ofstream(exe) << "#!/bin/sh\necho \"$@\" >> '" << log.string() << "'\n"
                       << "cmd=$1\n"
                       << "if [ \"$cmd\" = suggest-body ]; then sleep " << delaySeconds << "; cat '" << suggest.string() << "'; exit 0; fi\n"
                       << "if [ \"$cmd\" = fetch ]; then\n"
                       << "  tone=$2; model=5001; cache=''; shift 2\n"
                       << "  while [ $# -gt 0 ]; do case \"$1\" in --model) model=$2; shift 2;; --cache-dir) cache=$2; shift 2;; *) shift;; esac; done\n"
                       << "  mkdir -p \"$cache/$tone\"; cp '" << (kFixtures / "nam" / "linear_identity.nam").string() << "' \"$cache/$tone/$model.nam\"\n"
                       << "  sha=$(sha256sum \"$cache/$tone/$model.nam\" | cut -d' ' -f1)\n"
                       << "  printf '{\"tone_id\":\"%s\",\"model_id\":\"%s\",\"path\":\"%s/%s/%s.nam\",\"sha256\":\"%s\",\"kind\":\"nam\",\"gear\":\"amp\","
                          "\"source\":{\"provider\":\"tone3000\",\"id\":\"%s\",\"modelId\":\"%s\",\"title\":\"Fetched %s\",\"creator\":\"c\",\"license\":\"cc-by\",\"url\":\"u\"}}\\n' "
                          "\"$tone\" \"$model\" \"$cache\" \"$tone\" \"$model\" \"$sha\" \"$tone\" \"$model\" \"$tone\"\n"
                       << "fi\n";
    fs::permissions(exe, fs::perms::owner_all);
  }
  void setSuggest(const std::string& t) const { std::ofstream(suggest) << t << "\n"; }
  std::vector<std::string> calls() const {
    std::vector<std::string> v;
    std::ifstream in(log);
    for (std::string l; std::getline(in, l);) v.push_back(l);
    return v;
  }
  void configure(const fs::path& dir) const { std::ofstream(dir / "settings.json") << json{{"t3kExecutable", exe.string()}}.dump(); }
};

// Path A: a TONE3000 amp titled "Marshall A"; path B empty and off. Level match off (a legacy single-path preset).
fs::path writeSinglePreset(const fs::path& dir, bool bHasBlock = false) {
  json a = {{"blocks", json::array({{{"id", "a1"}, {"type", "nam"}, {"slot", "amp"},
                                     {"model", {{"file", (kFixtures / "nam" / "linear_identity.nam").string()},
                                                {"source", {{"provider", "tone3000"}, {"id", "T0"}, {"modelId", "m0"}, {"title", "Marshall A"}}}}}}})}};
  json b = {{"enabled", false}, {"blocks", json::array()}};
  if (bHasBlock) b["blocks"].push_back({{"id", "b1"}, {"type", "nam"}, {"slot", "amp"}, {"model", {{"file", (kFixtures / "nam" / "linear_identity.nam").string()}}}});
  json j = {{"schema", "sawblade.preset"}, {"version", 2}, {"name", "single"},
            {"paths", {{"a", a}, {"b", b}}}, {"align", {{"mode", "off"}}}, {"blend", 0.0},
            {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}};
  const fs::path p = dir / "single.json";
  std::ofstream(p) << j.dump(2);
  return p;
}

// Runs the asynchronous steps to the end (each tool run, then its result on the "message thread").
void pump(Host& h, RigController& ctl) {
  for (int i = 0; i < 12; ++i) {
    ctl.bodyFill().waitToolIdle(std::chrono::seconds(20));
    ctl.sync();
    REQUIRE(h.p.waitForLoader());
    if (!ctl.bodyFill().active()) break;
  }
  REQUIRE_FALSE(ctl.bodyFill().active());
}

const NamBlockParams* ampOf(const PathPreset& p) {
  const int i = ampIndex(p);
  return i < 0 ? nullptr : dynamic_cast<const NamBlockParams*>(p.blocks[static_cast<std::size_t>(i)].params.get());
}

}  // namespace

TEST_CASE("Body fill: the TS boost and the fill rule", "[bodyfill][rig]") {
  Preset p = makeInitPreset();
  const Block ts = makeTsBoost(p);
  CHECK(ts.type == "pedal.ts");
  CHECK(ts.slot == "boost");
  CHECK(ts.id == "b1");
  const auto j = ts.params->toJson();
  CHECK(j["params"]["drive"].get<double>() == 0.0);
  CHECK(j["params"]["tone"].get<double>() == 5.0);
  CHECK(j["params"]["level"].get<double>() == 8.0);
  // Fills an empty path B; with an amp: [TS, amp] and unique ids.
  Capture c;
  c.file = (kFixtures / "nam" / "linear_identity.nam").string();
  c.resolvedPath = c.file;
  CHECK(fillBodyPath(p, c));
  REQUIRE(p.b.blocks.size() == 2);
  CHECK(p.b.blocks[0].type == "pedal.ts");
  CHECK(p.b.blocks[1].type == "nam");
  CHECK(p.b.blocks[1].slot == "amp");
  CHECK(p.b.blocks[0].id != p.b.blocks[1].id);
  CHECK(ampIndex(p.b) == 1);
  CHECK_FALSE(fillBodyPath(p, c));  // not empty any more: untouched
  CHECK(p.b.blocks.size() == 2);
  Preset q = makeInitPreset();
  CHECK(fillBodyPath(q, std::nullopt));
  CHECK(q.b.blocks.size() == 1);  // TS only (no amp cached yet)
  setBodyAmp(q, c);
  REQUIRE(q.b.blocks.size() == 2);
  CHECK(ampIndex(q.b) == 1);
  Capture d = c;
  d.file += "x";
  setBodyAmp(q, d);  // replaces the amp, keeps the TS
  CHECK(q.b.blocks.size() == 2);
  CHECK(q.b.blocks[0].type == "pedal.ts");
  CHECK(ampOf(q.b)->model.file == d.file);
}

TEST_CASE("Body fill: the tool's answers are parsed strictly", "[bodyfill][rig]") {
  auto s = parseSuggestBody(R"({"tone_id":"88689","model_id":"747688","title":"EVH 5150iii","cached":true})");
  REQUIRE(s);
  CHECK(s->toneId == "88689");
  CHECK(s->modelId == "747688");
  CHECK(s->title == "EVH 5150iii");
  CHECK(s->cached);
  CHECK_FALSE(parseSuggestBody("null"));
  CHECK_FALSE(parseSuggestBody(""));
  CHECK_FALSE(parseSuggestBody("note\nnull\n"));
  CHECK_FALSE(parseSuggestBody(R"({"tone_id":"1"})"));
  CHECK_FALSE(parseSuggestBody(R"({"tone_id":1,"model_id":2})"));
  s = parseSuggestBody(std::string("warn\n") + R"({"tone_id":"1","model_id":"2"})" + "\n");
  REQUIRE(s);
  CHECK_FALSE(s->cached);
  const fs::path f = kFixtures / "nam" / "linear_identity.nam";
  const auto c = parseFetchedCapture(json{{"tone_id", "1"}, {"path", f.string()}, {"sha256", "ab"},
                                          {"source", {{"provider", "tone3000"}, {"id", "1"}, {"modelId", "2"}, {"title", "T"}, {"creator", "c"}, {"license", "cc-by"}}}}.dump());
  REQUIRE(c);
  CHECK(c->resolvedPath == f);
  CHECK(c->source->title == "T");
  CHECK_FALSE(parseFetchedCapture(R"({"path":"/nonexistent/x.nam","source":{"provider":"tone3000","id":"1"}})"));
  CHECK_FALSE(parseFetchedCapture("{}"));
}

TEST_CASE("Body fill: BLEND fills an empty path B (fallback amp cached), level match on, one undo restores the preset", "[bodyfill][rig]") {
  const BfCache cache;
  cache.put("88689", "5001");
  TempDir t;
  const NoTool noTool(t.dir);
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  const Preset pre = h.p.currentPreset();
  RigController ctl(h.p);
  CHECK_FALSE(ctl.canUndo());
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  REQUIRE(h.p.status().error.empty());
  const Preset cur = h.p.currentPreset();
  CHECK(cur.b.enabled);
  REQUIRE(cur.b.blocks.size() == 2);
  CHECK(cur.b.blocks[0].type == "pedal.ts");
  const auto* amp = ampOf(cur.b);
  REQUIRE(amp != nullptr);
  CHECK(amp->model.source->id == "88689");
  CHECK(amp->model.source->modelId == "5001");
  CHECK(cur.levelMatch.mode == LevelMatchMode::Auto);  // the 10.1 level match
  CHECK(cur.blendLaw == BlendLaw::ConstantLoudness);
  CHECK(h.p.status().info.levelMeasured);
  CHECK(ctl.bodyFill().toolRuns() == 0);  // no tool configured: nothing asynchronous
  // Path A is untouched.
  CHECK(cur.a == pre.a);
  // One undo.
  REQUIRE(ctl.canUndo());
  REQUIRE(ctl.undo());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset() == pre);
  CHECK_FALSE(ctl.canUndo());
  CHECK_FALSE(ctl.undo());
}

TEST_CASE("Body fill: nothing cached and no tool: path B gets the TS boost only", "[bodyfill][rig]") {
  const BfCache cache;
  TempDir t;
  const NoTool noTool(t.dir);
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  RigController ctl(h.p);
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  REQUIRE(h.p.status().error.empty());  // an uncached capture is never put in the preset, so the build cannot fail
  const Preset cur = h.p.currentPreset();
  REQUIRE(cur.b.blocks.size() == 1);
  CHECK(cur.b.blocks[0].type == "pedal.ts");
  CHECK(cur.b.enabled);
}

TEST_CASE("Body fill: a path B that already has blocks is left untouched", "[bodyfill][rig]") {
  const BfCache cache;
  cache.put("88689", "5001");
  TempDir t;
  const NoTool noTool(t.dir);
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir, /*bHasBlock=*/true));
  const Preset pre = h.p.currentPreset();
  RigController ctl(h.p);
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  const Preset cur = h.p.currentPreset();
  CHECK(cur.b.enabled);
  CHECK(cur.b.blocks == pre.b.blocks);
  CHECK(ctl.canUndo());  // v0.3 Task D: switching the topology is an edit like any other (one step); it is not a fill
  CHECK(ctl.bodyFill().toolRuns() == 0);
}

TEST_CASE("Body fill: the pool's suggestion replaces the fallback amp, coalesced into the same undo step", "[bodyfill][rig]") {
  const AllowTool allowTool;
  const BfCache cache;
  cache.put("88689", "5001");
  cache.put("T9", "m5");
  TempDir t;
  const FakeTool tool(t.dir, R"({"tone_id":"T9","model_id":"m5","title":"Diezel X","cached":true})");
  tool.configure(t.dir);
  BfEnv settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  const Preset pre = h.p.currentPreset();
  RigController ctl(h.p);
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  CHECK(ampOf(h.p.currentPreset().b)->model.source->id == "88689");  // the immediate fill
  pump(h, ctl);
  const Preset cur = h.p.currentPreset();
  REQUIRE(cur.b.blocks.size() == 2);
  CHECK(cur.b.blocks[0].type == "pedal.ts");
  const auto* amp = ampOf(cur.b);
  REQUIRE(amp != nullptr);
  CHECK(amp->model.source->id == "T9");
  CHECK(amp->model.source->modelId == "m5");
  CHECK(amp->model.source->title == "Tone T9");  // the cache entry's title (the tool's answer is only used when the entry has none)
  CHECK(cur.levelMatch.mode == LevelMatchMode::Auto);
  CHECK(h.p.status().info.levelMeasured);
  const auto calls = tool.calls();
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].find("suggest-body --a-title Marshall A --cache-dir ") == 0);
  CHECK(calls[0].find("--json") != std::string::npos);
  // ONE undo restores the pre-BLEND preset, even though the amp was replaced afterwards.
  REQUIRE(ctl.undo());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset() == pre);
  CHECK_FALSE(ctl.canUndo());
}

TEST_CASE("Body fill: a suggestion that is not cached is fetched; a failing fetch falls back", "[bodyfill][rig]") {
  const AllowTool allowTool;
  const BfCache cache;
  TempDir t;
  const FakeTool tool(t.dir, R"({"tone_id":"T9","model_id":"m5","title":"Diezel X","cached":false})");
  tool.configure(t.dir);
  BfEnv settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  RigController ctl(h.p);
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset().b.blocks.size() == 1);  // nothing cached: TS only for now
  pump(h, ctl);
  const Preset cur = h.p.currentPreset();
  REQUIRE(cur.b.blocks.size() == 2);
  CHECK(ampOf(cur.b)->model.source->id == "T9");
  CHECK(ampOf(cur.b)->model.source->title == "Fetched T9");
  CHECK(fs::exists(cache.dir / "T9" / "m5.nam"));
  const auto calls = tool.calls();
  REQUIRE(calls.size() == 2);
  CHECK(calls[1].find("fetch T9 --model m5 --json --cache-dir ") == 0);
  REQUIRE(h.p.status().error.empty());
}

TEST_CASE("Body fill: no suggestion (null) or a tool error: the fallback amp is fetched", "[bodyfill][rig]") {
  const AllowTool allowTool;
  for (const char* answer : {"null", "garbage"}) {
    CAPTURE(answer);
    const BfCache cache;
    TempDir t;
    const FakeTool tool(t.dir, answer);
    tool.configure(t.dir);
    BfEnv settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
    Host h(48000.0, 512);
    h.load(writeSinglePreset(t.dir));
    RigController ctl(h.p);
    ctl.setTopology(Topology::Blend);
    REQUIRE(h.p.waitForLoader());
    pump(h, ctl);
    const Preset cur = h.p.currentPreset();
    REQUIRE(cur.b.blocks.size() == 2);
    CHECK(ampOf(cur.b)->model.source->id == "88689");
    const auto calls = tool.calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].find("fetch 88689 --json --cache-dir ") == 0);
    REQUIRE(ctl.undo());
    REQUIRE(h.p.waitForLoader());
    CHECK(h.p.currentPreset().b.blocks.empty());
    CHECK_FALSE(h.p.currentPreset().b.enabled);
  }
}

TEST_CASE("Body fill: a suggestion is dropped if path B was edited meanwhile, or the fill undone", "[bodyfill][rig]") {
  const AllowTool allowTool;
  const BfCache cache;
  cache.put("88689", "5001");
  cache.put("T9", "m5");
  TempDir t;
  const FakeTool tool(t.dir, R"({"tone_id":"T9","model_id":"m5","title":"Diezel X","cached":true})", /*delaySeconds=*/1);  // the answer comes after the edits below
  tool.configure(t.dir);
  BfEnv settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
  {
    Host h(48000.0, 512);
    h.load(writeSinglePreset(t.dir));
    RigController ctl(h.p);
    ctl.setTopology(Topology::Blend);
    REQUIRE(h.p.waitForLoader());
    ctl.edit([](Preset& p) { setBypass(p.b, 0, true); });  // the user touches the TS before the answer arrives
    REQUIRE(h.p.waitForLoader());
    pump(h, ctl);
    const Preset cur = h.p.currentPreset();
    CHECK(ampOf(cur.b)->model.source->id == "88689");  // still the fallback
    CHECK(cur.b.blocks[0].bypass);
  }
  {
    Host h(48000.0, 512);
    h.load(writeSinglePreset(t.dir));
    const Preset pre = h.p.currentPreset();
    RigController ctl(h.p);
    ctl.setTopology(Topology::Blend);
    REQUIRE(h.p.waitForLoader());
    REQUIRE(ctl.undo());  // before the answer arrives
    REQUIRE(h.p.waitForLoader());
    ctl.bodyFill().waitToolIdle(std::chrono::seconds(20));
    ctl.sync();
    REQUIRE(h.p.waitForLoader());
    CHECK(h.p.currentPreset() == pre);  // the late answer did nothing
  }
  {
    Host h(48000.0, 512);
    h.load(writeSinglePreset(t.dir));
    RigController ctl(h.p);
    ctl.setTopology(Topology::Blend);
    REQUIRE(h.p.waitForLoader());
    ctl.setTopology(Topology::Single);  // BLEND off again before the answer
    REQUIRE(h.p.waitForLoader());
    pump(h, ctl);
    CHECK_FALSE(h.p.currentPreset().b.enabled);
    CHECK(ampOf(h.p.currentPreset().b)->model.source->id == "88689");  // path B kept as the fill left it, not swapped
  }
}

// ---- review fixes -----------------------------------------------------------------------------------------------
TEST_CASE("Body fill: a cached amp carries its licence, creator, title, url and sha256 (cc-by-nc marks the rig non-commercial)", "[bodyfill][rig][licence]") {
  const BfCache cache;
  cache.put("88689", "5001", "cc-by-nc");
  TempDir t;
  const NoTool noTool(t.dir);
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  RigController ctl(h.p);
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  REQUIRE(h.p.status().error.empty());
  const auto* amp = ampOf(h.p.currentPreset().b);
  REQUIRE(amp != nullptr);
  REQUIRE(amp->model.source.has_value());
  CHECK(amp->model.source->license == "cc-by-nc");
  CHECK(amp->model.source->creator == "Creator 88689");
  CHECK(amp->model.source->title == "Tone 88689");
  CHECK(amp->model.source->url == "https://www.tone3000.com/tones/88689");
  CHECK(amp->model.sha256.size() == 64);
  CHECK(nonCommercialLicense(amp->model.source->license));
  CHECK(summariseRig(h.p.currentPreset()).nonCommercial);
  // And it survives the state round trip (the licence is part of the saved capture).
  juce::MemoryBlock s;
  h.p.getStateInformation(s);
  const json st = json::parse(std::string(static_cast<const char*>(s.getData()), s.getSize()));
  CHECK(st["paths"]["b"]["blocks"][1]["model"]["source"]["license"] == "cc-by-nc");
  CHECK(st["paths"]["b"]["blocks"][1]["model"]["source"]["creator"] == "Creator 88689");
}

TEST_CASE("Body fill: a cached suggestion carries its licence and creator too", "[bodyfill][rig][licence]") {
  const AllowTool allowTool;
  const BfCache cache;
  cache.put("88689", "5001");
  cache.put("T9", "m5", "cc-by-nc");
  TempDir t;
  const FakeTool tool(t.dir, R"({"tone_id":"T9","model_id":"m5","title":"Diezel X","cached":true})");
  tool.configure(t.dir);
  BfEnv settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  RigController ctl(h.p);
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  pump(h, ctl);
  const auto* amp = ampOf(h.p.currentPreset().b);
  REQUIRE(amp != nullptr);
  CHECK(amp->model.source->id == "T9");
  CHECK(amp->model.source->license == "cc-by-nc");
  CHECK(amp->model.source->creator == "Creator T9");
  CHECK(amp->model.sha256.size() == 64);
  CHECK(summariseRig(h.p.currentPreset()).nonCommercial);
}

TEST_CASE("Body fill: a model file without a meta entry is not cached; the model pick without an id", "[bodyfill][rig][licence]") {
  const BfCache cache;
  fs::create_directories(cache.dir / "T5");
  fs::copy_file(kFixtures / "nam" / "linear_identity.nam", cache.dir / "T5" / "77.nam");  // a file, but no meta.json
  CHECK_FALSE(cachedToneCapture("T5"));
  CHECK_FALSE(cachedToneCapture("T5", "77"));
  cache.put("T5", "900");
  cache.put("T5", "88");  // meta now lists 88 and 900 (77 has a file but no entry)
  const auto c = cachedToneCapture("T5");
  REQUIRE(c);
  CHECK(c->source->modelId == "88");  // the smallest model id with an entry and a file
  CHECK(cachedToneCapture("T5", "900")->source->modelId == "900");
  CHECK_FALSE(cachedToneCapture("T5", "77"));
  CHECK_FALSE(cachedToneCapture("../T5"));
  CHECK_FALSE(cachedToneCapture("nope"));
  TempDir t;
  const NoTool noTool(t.dir);
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  RigController ctl(h.p);
  ctl.setTopology(Topology::Blend);  // 88689 is not cached (no meta): TS only
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset().b.blocks.size() == 1);
}

TEST_CASE("Body fill undo: later edits are steps of their own; undo walks back through them, the fill is one step", "[bodyfill][rig][undo]") {
  const BfCache cache;
  cache.put("88689", "5001");
  TempDir t;
  const NoTool noTool(t.dir);
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  const Preset pre = h.p.currentPreset();
  RigController ctl(h.p);
  // BLEND, then an edit of path A: two steps. (v0.2 refused to undo the fill once anything else had changed.)
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  const Preset filled = h.p.currentPreset();
  ctl.edit([](Preset& p) { setBypass(p.a, 0, true); });
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.undoSteps() == 2);
  REQUIRE(ctl.undo());  // the bypass
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(h.p.currentPreset().a.blocks[0].bypass);
  CHECK(h.p.currentPreset() == filled);
  CHECK(h.p.currentPreset().b.enabled);
  REQUIRE(ctl.undo());  // the fill
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset() == pre);
  CHECK_FALSE(ctl.canUndo());
  // Redo walks forward again, to the fill and then the bypass.
  REQUIRE(ctl.redo());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset() == filled);
  REQUIRE(ctl.redo());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset().a.blocks[0].bypass);
  CHECK_FALSE(ctl.canRedo());
}

TEST_CASE("Body fill undo: a host parameter change (automation) is not a step and is not rewound by an undo", "[bodyfill][rig][undo]") {
  const BfCache cache;
  cache.put("88689", "5001");
  TempDir t;
  const NoTool noTool(t.dir);
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  RigController ctl(h.p);
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  REQUIRE(h.p.undoSteps() == 1);
  h.setParam(kOutputGain, -3.0);  // the host: no gesture
  CHECK(h.p.undoSteps() == 1);
  REQUIRE(ctl.undo());
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(h.p.currentPreset().b.enabled);
  CHECK(h.param(kOutputGain) == Catch::Approx(-3.0));  // the automation is still there: the fill step did not touch OUTPUT
  REQUIRE(ctl.redo());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset().b.enabled);
  CHECK(h.param(kOutputGain) == Catch::Approx(-3.0));
}

TEST_CASE("Body fill undo: a new edit after an undo ends the redo branch; a second fill never resurrects the first", "[bodyfill][rig][undo]") {
  const BfCache cache;
  cache.put("88689", "5001");
  TempDir t;
  const NoTool noTool(t.dir);
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  const Preset pre1 = h.p.currentPreset();
  RigController ctl(h.p);
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  REQUIRE(ctl.undo());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset() == pre1);
  CHECK_FALSE(ctl.canUndo());
  CHECK(ctl.canRedo());
  ctl.edit([](Preset& p) { p.a.levelDb = -4.0; });  // a change made after the first undo
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(ctl.canRedo());  // the first fill can no longer be redone
  const Preset pre2 = h.p.currentPreset();
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  REQUIRE(ctl.undo());
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset() == pre2);  // not pre1
  CHECK(h.p.currentPreset().a.levelDb == Catch::Approx(-4.0));
  REQUIRE(ctl.undo());  // the level edit is a step of its own now
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset() == pre1);
  CHECK_FALSE(ctl.undo());
}

TEST_CASE("Body fill: the suggestion is dropped if anything of path B or the blend changed", "[bodyfill][rig]") {
  const AllowTool allowTool;
  const BfCache cache;
  cache.put("88689", "5001");
  cache.put("T9", "m5");
  TempDir t;
  const FakeTool tool(t.dir, R"({"tone_id":"T9","model_id":"m5","title":"Diezel X","cached":true})", /*delaySeconds=*/1);
  tool.configure(t.dir);
  BfEnv settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
  const std::function<void(Host&, RigController&)> edits[] = {
      [](Host&, RigController& c) { c.edit([](Preset& p) { p.b.levelDb = -3.0; }); },                                // path B level
      [](Host&, RigController& c) { c.edit([](Preset& p) { p.b.ampControls.gain = 7.0; }); },                        // path B amp controls
      [](Host&, RigController& c) { c.edit([](Preset& p) { p.b.invert = true; }); },                                 // path B polarity
      [](Host& h, RigController&) { h.setParam(kBlend, 0.9); },                                                      // the blend knob
  };
  for (std::size_t i = 0; i < std::size(edits); ++i) {
    CAPTURE(i);
    Host h(48000.0, 512);
    h.load(writeSinglePreset(t.dir));
    RigController ctl(h.p);
    ctl.setTopology(Topology::Blend);
    REQUIRE(h.p.waitForLoader());
    edits[i](h, ctl);
    REQUIRE(h.p.waitForLoader());
    pump(h, ctl);
    CHECK(ampOf(h.p.currentPreset().b)->model.source->id == "88689");  // the fallback stayed
  }
}

// --- v0.3 Task D: the BLEND fill and the undo history ------------------------------------------------------------------------------------

TEST_CASE("Body fill undo: the fill's amp arriving is patched into the history; undoing a LATER edit keeps the amp (and adds no step)", "[bodyfill][rig][undo]") {
  const AllowTool allowTool;
  const BfCache cache;
  cache.put("88689", "5001");
  cache.put("T9", "m5");
  TempDir t;
  const FakeTool tool(t.dir, R"({"tone_id":"T9","model_id":"m5","title":"Diezel X","cached":true})");
  tool.configure(t.dir);
  BfEnv settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  const Preset pre = h.p.currentPreset();
  RigController ctl(h.p);
  ctl.setTopology(Topology::Blend);
  REQUIRE(h.p.waitForLoader());
  REQUIRE(ampOf(h.p.currentPreset().b)->model.source->id == "88689");  // the fallback, first
  ctl.edit([](Preset& p) { p.a.levelDb = -2.0; });  // a later step: its "before" snapshot holds the fill with the FALLBACK amp
  REQUIRE(h.p.waitForLoader());
  pump(h, ctl);  // the suggestion arrives
  REQUIRE(ampOf(h.p.currentPreset().b)->model.source->id == "T9");
  CHECK(h.p.undoSteps() == 2);  // the fill and the edit: the arrival added none
  REQUIRE(ctl.undo());  // the level edit
  REQUIRE(h.p.waitForLoader());
  const Preset cur = h.p.currentPreset();
  CHECK(cur.a.levelDb == Catch::Approx(0.0));
  REQUIRE(ampOf(cur.b) != nullptr);
  CHECK(ampOf(cur.b)->model.source->id == "T9");  // the amp survived the undo
  REQUIRE(ctl.redo());
  REQUIRE(h.p.waitForLoader());
  CHECK(ampOf(h.p.currentPreset().b)->model.source->id == "T9");
  // The snapshot from before BLEND did NOT gain the amp (its path B is not what the fill made): undo through the fill to the pre-BLEND preset.
  REQUIRE(ctl.undo());  // the level edit
  REQUIRE(h.p.waitForLoader());
  REQUIRE(ctl.undo());  // the fill
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset() == pre);
  CHECK(ampOf(h.p.currentPreset().b) == nullptr);
}

TEST_CASE("Body fill undo: redo into a blend path B that has no amp restarts the fill; an undo that empties path B cancels one in flight", "[bodyfill][rig][undo]") {
  const AllowTool allowTool;
  const BfCache cache;
  TempDir t;
  const FakeTool tool(t.dir, "null", /*delaySeconds=*/1);
  tool.configure(t.dir);
  BfEnv settings("SAWBLADE_SETTINGS_FILE", (t.dir / "settings.json").string());
  Host h(48000.0, 512);
  h.load(writeSinglePreset(t.dir));
  RigController ctl(h.p);
  ctl.setTopology(Topology::Blend);  // nothing cached: boost only for now, the fill is in flight
  REQUIRE(h.p.waitForLoader());
  REQUIRE(ctl.bodyFill().active());
  REQUIRE(ampOf(h.p.currentPreset().b) == nullptr);
  const Preset boostOnly = h.p.currentPreset();
  REQUIRE(ctl.undo());  // path B is empty again: the fill is cancelled
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(ctl.bodyFill().active());
  CHECK(ctl.bodyFill().status().kind == FillStatus::Kind::Idle);
  ctl.bodyFill().waitToolIdle(std::chrono::seconds(20));
  ctl.sync();
  CHECK(h.p.currentPreset().b.blocks.empty());  // the late answer did nothing
  const std::size_t runsBefore = tool.calls().size();
  REQUIRE(ctl.redo());  // a blend path B with a boost and no amp: the fill starts again
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.currentPreset().b.blocks.size() == boostOnly.b.blocks.size());
  CHECK(ctl.bodyFill().active());
  pump(h, ctl);
  CHECK(tool.calls().size() > runsBefore);  // it asked the tool again
  CHECK(ampOf(h.p.currentPreset().b) != nullptr);  // and the fallback amp (fetched by the fake tool) arrived
}
