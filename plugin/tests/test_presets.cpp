// Phase 9b, non-visual parts (docs/specs/phase9b_preset_browser.md): the preset library, user operations, A/B compare, the
// resolve-on-load flow with a fake sawblade-t3k, and the info panel's content. The browser UI is tested in test_preset_browser.cpp.
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "PluginProcessor.h"
#include "presets/AbCompare.h"
#include "settings/Settings.h"
#include "presets/PresetInfoPanel.h"
#include "presets/PresetLibrary.h"
#include "presets/PresetLoadFlow.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::plugin;
using nlohmann::json;
namespace fs = std::filesystem;

#include "processor_harness.h"

namespace {

json namBlock(const std::string& file, const json& source = nullptr) {
  json m = {{"file", file}};
  if (!source.is_null()) m["source"] = source;
  return {{"id", "a1"}, {"type", "nam"}, {"slot", "amp"}, {"model", m}};
}
json src(const char* id, const char* model, const char* title, const char* creator, const char* license) {
  return {{"provider", "tone3000"}, {"id", id}, {"modelId", model}, {"title", title}, {"creator", creator}, {"license", license}, {"url", std::string("https://www.tone3000.com/tones/") + id}};
}
json preset(const std::string& name, const std::string& category, const std::string& notes, const json& blockA, const json& ir) {
  json j = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", name}, {"notes", notes},
            {"paths", {{"a", {{"blocks", json::array({blockA})}}}, {"b", {{"blocks", json::array()}}}}},
            {"align", {{"mode", "off"}}}, {"cab", {{"mode", "shared"}, {"ir", ir}}}};
  if (!category.empty()) j["category"] = category;
  return j;
}
void put(const fs::path& f, const json& j) {
  fs::create_directories(f.parent_path());
  std::ofstream(f) << j.dump(2);
}
json irRef(const std::string& file = "x.wav") { return {{"file", file}}; }

// A factory + user tree in a temp dir.
struct Tree {
  TempDir tmp;
  LibraryConfig cfg;
  Tree() {
    cfg.factoryDir = tmp.dir / "factory";
    cfg.userDir = tmp.dir / "user";
    put(cfg.factoryDir / "classic1.json", preset("Chainsaw + Body", "Swedish death (HM-2)", "Default north-star blend", namBlock("a.nam"), irRef()));
    put(cfg.factoryDir / "classic2.json", preset("Tight Body", "", "D-beat tight", namBlock("a.nam"), irRef()));
    put(cfg.factoryDir / "styles" / "grind.json", preset("Grind (Terrorizer-style)", "Grind", "blast beats", namBlock("g.nam", src("11", "22", "Grind amp", "alice", "t3k")), irRef()));
    put(cfg.factoryDir / "matched" / "barb.json", preset("Barbaric matched", "", "from a song", namBlock("b.nam", src("33", "44", "HM-2 maxed", "Bob", "cc-by-nc")), irRef()));
    put(cfg.factoryDir / "matched" / "barb.resolved.json", preset("RESOLVED", "", "", namBlock("b.nam"), irRef()));
    put(cfg.factoryDir / "classic1.resolved.json", preset("RESOLVED", "", "", namBlock("b.nam"), irRef()));
    std::ofstream(cfg.factoryDir / "broken.json") << "{ not json";
    put(cfg.userDir / "mine.json", preset("My tone", "Thrash", "", namBlock("a.nam"), irRef()));
  }
};

int indexByName(const PresetLibrary& l, const std::string& n) {
  for (int i = 0; i < static_cast<int>(l.entries().size()); ++i)
    if (l.entries()[static_cast<std::size_t>(i)].name == n) return i;
  return -1;
}

void setParamValue(SawbladeProcessor& p, int i, double v) {
  auto* prm = p.parameters().getParameter(paramSpec(i).id);
  prm->setValueNotifyingHost(prm->convertTo0to1(static_cast<float>(v)));
}
double paramValue(SawbladeProcessor& p, int i) { return static_cast<double>(p.parameters().getRawParameterValue(paramSpec(i).id)->load()); }

}  // namespace

TEST_CASE("library scan: banks, sub-banks, category fallback, resolved files skipped, broken files listed", "[presets][library]") {
  Tree t;
  PresetLibrary lib(t.cfg);
  lib.rescan();
  REQUIRE(lib.entries().size() == 6);  // classic1, classic2, broken (Classic); grind (Styles); barb (Matched); mine (User)
  for (const auto& e : lib.entries()) CHECK(e.file.string().find(".resolved.") == std::string::npos);

  const auto& c1 = lib.entries()[static_cast<std::size_t>(indexByName(lib, "Chainsaw + Body"))];
  CHECK(c1.bank == SubBank::Classic);
  CHECK(isFactory(c1.bank));
  CHECK(c1.displayCategory() == "Swedish death (HM-2)");
  CHECK(c1.notes == "Default north-star blend");
  const auto& c2 = lib.entries()[static_cast<std::size_t>(indexByName(lib, "Tight Body"))];
  CHECK(c2.displayCategory() == "Uncategorised");
  CHECK(lib.entries()[static_cast<std::size_t>(indexByName(lib, "Grind (Terrorizer-style)"))].bank == SubBank::Styles);
  const auto& m = lib.entries()[static_cast<std::size_t>(indexByName(lib, "Barbaric matched"))];
  CHECK(m.bank == SubBank::Matched);
  CHECK(m.displayCategory() == "Matched");  // uncategorised in matched/
  REQUIRE(m.captures.size() == 2);
  CHECK(m.captures[0].creator == "Bob");
  CHECK(m.captures[0].nonCommercial);
  CHECK_FALSE(m.captures[1].hasSource);  // the cab IR
  const auto& u = lib.entries()[static_cast<std::size_t>(indexByName(lib, "My tone"))];
  CHECK(u.bank == SubBank::User);
  CHECK_FALSE(isFactory(u.bank));

  int broken = -1;
  for (int i = 0; i < static_cast<int>(lib.entries().size()); ++i)
    if (lib.entries()[static_cast<std::size_t>(i)].file.filename() == "broken.json") broken = i;
  REQUIRE(broken >= 0);
  CHECK_FALSE(lib.entries()[static_cast<std::size_t>(broken)].loadable());
  CHECK_FALSE(lib.entries()[static_cast<std::size_t>(broken)].error.empty());

  // a missing user dir is fine
  PresetLibrary empty(LibraryConfig{t.tmp.dir / "none", t.tmp.dir / "none2"});
  empty.rescan();
  CHECK(empty.entries().empty());
}

TEST_CASE("library search: case-insensitive, ANDed terms over name, category, notes and capture titles / creators", "[presets][library]") {
  Tree t;
  PresetLibrary lib(t.cfg);
  lib.rescan();
  auto names = [&](const LibraryFilter& f) {
    std::vector<std::string> v;
    for (int i : lib.filtered(f)) v.push_back(lib.entries()[static_cast<std::size_t>(i)].name);
    return v;
  };
  LibraryFilter f;
  f.search = "CHAINSAW";
  CHECK(names(f) == std::vector<std::string>{"Chainsaw + Body"});
  f.search = "grind";  // name + category of one preset
  CHECK(names(f).size() == 1);
  f.search = "blast";  // notes
  CHECK(names(f) == std::vector<std::string>{"Grind (Terrorizer-style)"});
  f.search = "bob";  // creator
  CHECK(names(f) == std::vector<std::string>{"Barbaric matched"});
  f.search = "hm-2 maxed";  // capture title, two terms
  CHECK(names(f) == std::vector<std::string>{"Barbaric matched"});
  f.search = "hm-2 chainsaw";  // ANDed over different fields: the category of one, the name of the same
  CHECK(names(f) == std::vector<std::string>{"Chainsaw + Body"});
  f.search = "chainsaw bob";  // no preset has both
  CHECK(names(f).empty());
  f.search = "  ";
  CHECK(names(f).size() == 6);

  // combined with the bank and category filters
  f = {};
  f.anyBank = false;
  f.factoryOnly = true;
  CHECK(names(f).size() == 5);
  f.anyBank = false;
  f.factoryOnly = false;
  f.bank = SubBank::User;
  CHECK(names(f) == std::vector<std::string>{"My tone"});
  f = {};
  f.category = "Matched";
  CHECK(names(f) == std::vector<std::string>{"Barbaric matched"});
  f.search = "tight";
  CHECK(names(f).empty());

  const auto cats = lib.categories(LibraryFilter{});
  int total = 0;
  for (const auto& c : cats) total += c.second;
  CHECK(total == 6);
  bool foundGrind = false;
  for (const auto& c : cats) foundGrind = foundGrind || (c.first == "Grind" && c.second == 1);
  CHECK(foundGrind);
}

TEST_CASE("save / load round trip: currentPreset() -> Save As -> load -> currentPreset() equal, parameters included", "[presets][user]") {
  Tree t;
  TempDir work;
  SawbladeProcessor proc;
  proc.prepareToPlay(48000.0, 512);
  REQUIRE(proc.loadPresetFile(writeIdentityPreset(work.dir, "init", 0)));
  REQUIRE(proc.waitForLoader());
  setParamValue(proc, kOutputGain, -4.5);
  setParamValue(proc, kBlend, 0.25);
  setParamValue(proc, kInputGain, 2.0);

  PresetLibrary lib(t.cfg);
  lib.rescan();
  Preset original = proc.currentPreset();
  fs::path written;
  std::string err;
  REQUIRE(lib.saveAs(original, "Round Trip: v1/2?", "Prog", false, &written, &err));
  CHECK(written == t.cfg.userDir / "Round Trip_ v1_2_.json");
  CHECK(written.parent_path() == t.cfg.userDir);
  const int idx = indexByName(lib, "Round Trip: v1/2?");
  REQUIRE(idx >= 0);
  CHECK(lib.entries()[static_cast<std::size_t>(idx)].category == "Prog");

  // disturb the processor, then load the saved file
  setParamValue(proc, kOutputGain, 3.0);
  setParamValue(proc, kBlend, 0.9);
  REQUIRE(proc.loadPresetFile(written));
  REQUIRE(proc.waitForLoader());
  REQUIRE(proc.status().error.empty());
  Preset expected = original;
  expected.name = "Round Trip: v1/2?";
  expected.category = "Prog";
  CHECK(proc.currentPreset() == expected);
  CHECK(paramValue(proc, kOutputGain) == -4.5);
  CHECK(paramValue(proc, kBlend) == 0.25);
  // the play-along state is never part of a saved preset
  CHECK(json::parse(std::ifstream(written)).contains("playAlong") == false);

  // a collision does not overwrite unless asked
  CHECK_FALSE(lib.saveAs(original, "Round Trip: v1/2?", "Prog", false, nullptr, &err));
  CHECK(err.find("exists") != std::string::npos);
  CHECK(lib.saveAs(original, "Round Trip: v1/2?", "Thrash", true, nullptr, &err));
  CHECK(lib.entries()[static_cast<std::size_t>(indexByName(lib, "Round Trip: v1/2?"))].category == "Thrash");
  // Save over the current user preset
  setParamValue(proc, kOutputGain, 1.0);
  REQUIRE(lib.save(proc.currentPreset(), written, &err));
  REQUIRE(proc.loadPresetFile(written));
  REQUIRE(proc.waitForLoader());
  CHECK(paramValue(proc, kOutputGain) == 1.0);
}

TEST_CASE("rename and delete user presets; factory presets are read-only", "[presets][user]") {
  Tree t;
  PresetLibrary lib(t.cfg);
  lib.rescan();
  std::string err;
  int mine = indexByName(lib, "My tone");
  REQUIRE(lib.rename(mine, "Renamed tone", &err));
  CHECK_FALSE(fs::exists(t.cfg.userDir / "mine.json"));
  CHECK(fs::exists(t.cfg.userDir / "Renamed tone.json"));
  mine = indexByName(lib, "Renamed tone");
  REQUIRE(mine >= 0);
  CHECK(lib.entries()[static_cast<std::size_t>(mine)].category == "Thrash");  // everything else is kept
  CHECK(json::parse(std::ifstream(t.cfg.userDir / "Renamed tone.json"))["name"] == "Renamed tone");
  // a rename onto another file is refused
  put(t.cfg.userDir / "other.json", preset("Other", "", "", namBlock("a.nam"), irRef()));
  lib.rescan();
  CHECK_FALSE(lib.rename(indexByName(lib, "Other"), "Renamed tone", &err));

  // factory presets cannot be renamed or deleted
  const int f = indexByName(lib, "Tight Body");
  CHECK_FALSE(lib.rename(f, "x", &err));
  CHECK_FALSE(lib.remove(f, [](const fs::path&) { return true; }, &err));
  CHECK(fs::exists(t.cfg.factoryDir / "classic2.json"));

  // delete: through the trash function; a failing trash leaves the file
  mine = indexByName(lib, "Renamed tone");
  CHECK_FALSE(lib.remove(mine, [](const fs::path&) { return false; }, &err));
  CHECK(fs::exists(t.cfg.userDir / "Renamed tone.json"));
  std::vector<fs::path> trashed;
  REQUIRE(lib.remove(mine, [&](const fs::path& p) { trashed.push_back(p); return fs::remove(p); }, &err));
  CHECK(trashed == std::vector<fs::path>{t.cfg.userDir / "Renamed tone.json"});
  CHECK(indexByName(lib, "Renamed tone") < 0);
}

TEST_CASE("file name sanitising", "[presets][user]") {
  CHECK(sanitiseFileName("Death / Metal: v2") == "Death _ Metal_ v2");
  CHECK(sanitiseFileName("  ..hidden.. ") == "hidden");
  CHECK(sanitiseFileName("...").empty());
  CHECK(sanitiseFileName("a..b") == "a_b");
  CHECK(sanitiseFileName("Caf\xc3\xa9 tone") == "Caf\xc3\xa9 tone");
  CHECK(sanitiseFileName(std::string(300, 'x')).size() == 120);
}

TEST_CASE("A/B compare: slots swap, each switch is one engine build", "[presets][ab]") {
  TempDir work;
  SawbladeProcessor proc;
  proc.prepareToPlay(48000.0, 512);
  REQUIRE(proc.loadPresetFile(writeIdentityPreset(work.dir, "init", 0)));
  REQUIRE(proc.waitForLoader());
  AbCompare ab(proc);
  CHECK(ab.active() == 0);
  CHECK(std::string(ab.label()) == "A");
  CHECK_FALSE(ab.hasOther());

  setParamValue(proc, kOutputGain, 3.0);  // a change on A
  auto builds = proc.engineBuilds();
  ab.toggle();  // -> B, which starts identical to A (with the change)
  REQUIRE(proc.waitForLoader());
  CHECK(proc.engineBuilds() == builds + 1);
  CHECK(std::string(ab.label()) == "B");
  CHECK(paramValue(proc, kOutputGain) == 3.0);
  CHECK(ab.slot(0)->outputGainDb == 3.0);

  setParamValue(proc, kInputGain, 2.0);  // another change, on B
  builds = proc.engineBuilds();
  ab.toggle();  // -> A: its value is restored
  REQUIRE(proc.waitForLoader());
  CHECK(proc.engineBuilds() == builds + 1);
  CHECK(paramValue(proc, kInputGain) == 0.0);
  CHECK(paramValue(proc, kOutputGain) == 3.0);
  builds = proc.engineBuilds();
  ab.toggle();  // -> B again: its value is restored
  REQUIRE(proc.waitForLoader());
  CHECK(proc.engineBuilds() == builds + 1);
  CHECK(paramValue(proc, kInputGain) == 2.0);

  // a preset loaded while B is active replaces B only
  REQUIRE(proc.loadPresetFile(writeIdentityPreset(work.dir, "other", 0)));
  REQUIRE(proc.waitForLoader());
  ab.toggle();  // -> A
  REQUIRE(proc.waitForLoader());
  CHECK(proc.currentPreset().name == "init");
  CHECK(paramValue(proc, kInputGain) == 0.0);
  ab.toggle();  // -> B: the loaded preset
  REQUIRE(proc.waitForLoader());
  CHECK(proc.currentPreset().name == "other");

  // copy and reset
  ab.copyAToB();  // B is active: it becomes A's content and is loaded
  REQUIRE(proc.waitForLoader());
  CHECK(proc.currentPreset().name == "init");
  setParamValue(proc, kInputGain, -1.0);
  ab.copyBToA();  // B (active, stored) -> A
  REQUIRE(proc.waitForLoader());
  CHECK(ab.slot(0)->inputGainDb == -1.0);
  ab.reset();
  CHECK(ab.active() == 0);
  CHECK_FALSE(ab.hasOther());
  CHECK(paramValue(proc, kInputGain) == -1.0);
  ab.toggle();
  REQUIRE(proc.waitForLoader());
  CHECK(paramValue(proc, kInputGain) == -1.0);  // B starts as a copy again
}

// ---- resolve on load --------------------------------------------------------------------------------------------------
namespace {

struct EnvVar {
  std::string k;
  EnvVar(const std::string& key, const std::string& v) : k(key) { ::setenv(k.c_str(), v.c_str(), 1); }
  ~EnvVar() {
    ::unsetenv(k.c_str());
    if (k == "SAWBLADE_APPDATA") sawblade::plugin::settings::Settings::resetSharedForTests();  // the cached settings file path may have followed it
  }
};

// A preset whose NAM capture is a TONE3000 one with a file that does not exist.
fs::path writeUnresolved(const fs::path& dir, const std::string& stem = "needs") {
  put(dir / (stem + ".json"), preset("Needs resolve", "Thrash", "", namBlock("captures/58569_496942.nam", src("58569", "496942", "Some amp", "carol", "cc-by")), irRef((kFixtures / "ir" / "impulse.wav").string())));
  return dir / (stem + ".json");
}
fs::path writeResolvedCopy(const fs::path& dir) {
  put(dir / "prepared.json", preset("Needs resolve", "Thrash", "", namBlock((kFixtures / "nam" / "linear_identity.nam").string(), src("58569", "496942", "Some amp", "carol", "cc-by")), irRef((kFixtures / "ir" / "impulse.wav").string())));
  return dir / "prepared.json";
}

struct Queue {
  std::mutex m;
  std::deque<std::function<void()>> q;
  void post(std::function<void()> f) {
    std::lock_guard<std::mutex> lk(m);
    q.push_back(std::move(f));
  }
  void pumpUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < end) {
      std::function<void()> f;
      {
        std::lock_guard<std::mutex> lk(m);
        if (!q.empty()) {
          f = std::move(q.front());
          q.pop_front();
        }
      }
      if (f) f();
      else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
};

fs::path script(const fs::path& dir, const std::string& name, const std::string& body) {
  const fs::path p = dir / name;
  std::ofstream(p) << "#!/bin/sh\n" << body << "\n";
  fs::permissions(p, fs::perms::owner_all);
  return p;
}

}  // namespace

TEST_CASE("load plan: a capture in the cache counts as present; a fresh resolved file is used directly", "[presets][resolve]") {
  TempDir tmp;
  EnvVar appdata("SAWBLADE_APPDATA", (tmp.dir / "data").string());
  EnvVar cache("SAWBLADE_CACHE_DIR", (tmp.dir / "cache").string());
  const fs::path pf = writeUnresolved(tmp.dir / "presets");

  LoadPlan plan = planPresetLoad(pf, SubBank::Matched);
  CHECK(plan.kind == LoadPlan::Kind::NeedsResolve);
  CHECK(plan.resolvedOut == tmp.dir / "data" / "resolved" / "matched" / "needs.resolved.json");

  // a resolved file that is newer than the preset and whose files exist is used
  fs::create_directories(plan.resolvedOut.parent_path());
  fs::copy_file(writeResolvedCopy(tmp.dir), plan.resolvedOut);
  fs::last_write_time(plan.resolvedOut, fs::last_write_time(pf) + std::chrono::seconds(5));
  plan = planPresetLoad(pf, SubBank::Matched);
  CHECK(plan.kind == LoadPlan::Kind::UseResolved);
  CHECK(plan.load == plan.resolvedOut);
  // older than the preset: resolve again
  fs::last_write_time(plan.resolvedOut, fs::last_write_time(pf) - std::chrono::seconds(5));
  CHECK(planPresetLoad(pf, SubBank::Matched).kind == LoadPlan::Kind::NeedsResolve);
  fs::remove(plan.resolvedOut);

  // cache hit: no resolve needed
  fs::create_directories(tmp.dir / "cache" / "58569");
  fs::copy_file(kFixtures / "nam" / "linear_identity.nam", tmp.dir / "cache" / "58569" / "496942.nam");
  plan = planPresetLoad(pf, SubBank::Matched);
  CHECK(plan.kind == LoadPlan::Kind::Direct);
  CHECK(plan.load == pf);
  // a preset whose captures are all local is direct; a broken one is invalid
  CHECK(planPresetLoad(writeResolvedCopy(tmp.dir), SubBank::User).kind == LoadPlan::Kind::Direct);
  std::ofstream(tmp.dir / "bad.json") << "nope";
  CHECK(planPresetLoad(tmp.dir / "bad.json", SubBank::User).kind == LoadPlan::Kind::Invalid);
}

TEST_CASE("resolve flow: progress, success, not logged in, missing tool, and no child process for a cached resolved file", "[presets][resolve]") {
  TempDir tmp;
  EnvVar appdata("SAWBLADE_APPDATA", (tmp.dir / "data").string());
  EnvVar cache("SAWBLADE_CACHE_DIR", (tmp.dir / "emptycache").string());
  SawbladeProcessor proc;
  proc.prepareToPlay(48000.0, 512);
  REQUIRE(proc.loadPresetFile(writeIdentityPreset(tmp.dir, "init", 0)));
  REQUIRE(proc.waitForLoader());
  const fs::path pf = writeUnresolved(tmp.dir / "presets");
  const fs::path prepared = writeResolvedCopy(tmp.dir);

  Queue queue;
  std::vector<std::string> progress;
  std::vector<PresetLoadFlow::Outcome> outcomes;
  PresetLoadFlow flow(proc,
                      {[&](int d, int t, const std::string& title) { progress.push_back(std::to_string(d) + "/" + std::to_string(t) + " " + title); },
                       [&](const PresetLoadFlow::Outcome& o) { outcomes.push_back(o); }},
                      [&](std::function<void()> f) { queue.post(std::move(f)); });
  auto run = [&] {
    const auto n = outcomes.size();
    REQUIRE(flow.load(pf, SubBank::Matched));
    queue.pumpUntil([&] { return outcomes.size() > n; });
    REQUIRE(outcomes.size() == n + 1);
  };

  // exit 4: the not-logged-in message, and the current sound is unchanged
  std::ofstream(tmp.dir / "data_settings") << "";
  fs::create_directories(tmp.dir / "data");
  auto setExe = [&](const fs::path& exe) { std::ofstream(tmp.dir / "data" / "settings.json") << json{{"t3kExecutable", exe.string()}}.dump(); };
  setExe(script(tmp.dir, "login", "echo 'auth' >&2\nexit 4"));
  const auto builds = proc.engineBuilds();
  run();
  CHECK(outcomes.back().status == PresetLoadFlow::Outcome::Status::NotLoggedIn);
  CHECK(outcomes.back().message == "Not logged in to TONE3000. Run `sawblade-t3k login` in a terminal, then try again.");
  CHECK(proc.currentPreset().name == "init");
  CHECK(proc.engineBuilds() == builds);
  CHECK_FALSE(flow.resolving());

  // missing executable
  setExe(tmp.dir / "gone" / "sawblade-t3k");
  run();
  CHECK(outcomes.back().status == PresetLoadFlow::Outcome::Status::MissingExecutable);
  CHECK(proc.currentPreset().name == "init");

  // success: progress lines, the resolved file is written to <appdata>/resolved/<bank>/<stem>.resolved.json and loaded
  const fs::path argsFile = tmp.dir / "args.txt";
  setExe(script(tmp.dir, "fake-t3k",
                "echo \"$@\" > \"" + argsFile.string() + "\"\n"
                "echo '{\"done\": 1, \"total\": 2, \"capture\": \"paths.a.blocks[0].model\", \"title\": \"Some amp\"}'\n"
                "echo '{\"done\": 2, \"total\": 2, \"capture\": \"cab.ir\", \"title\": \"A cab\"}'\n"
                "cp \"" + prepared.string() + "\" \"$4\""));
  run();
  CHECK(outcomes.back().status == PresetLoadFlow::Outcome::Status::Loaded);
  CHECK(progress == std::vector<std::string>{"1/2 Some amp", "2/2 A cab"});
  CHECK(outcomes.back().loadedFile == tmp.dir / "data" / "resolved" / "matched" / "needs.resolved.json");
  std::string args;
  std::getline(std::ifstream(argsFile), args);
  CHECK(args == "resolve " + pf.string() + " -o " + outcomes.back().loadedFile.string() + " --progress-json");
  REQUIRE(proc.waitForLoader());
  CHECK(proc.status().error.empty());
  CHECK(proc.currentPreset().name == "Needs resolve");

  // the resolved file is now newer than the preset and complete: loaded again with no child process (the tool is gone)
  REQUIRE(proc.loadPresetFile(writeIdentityPreset(tmp.dir, "init2", 0)));
  REQUIRE(proc.waitForLoader());
  setExe(tmp.dir / "gone" / "sawblade-t3k");
  fs::last_write_time(outcomes.back().loadedFile, fs::last_write_time(pf) + std::chrono::seconds(5));
  run();
  CHECK(outcomes.back().status == PresetLoadFlow::Outcome::Status::Loaded);
  REQUIRE(proc.waitForLoader());
  CHECK(proc.currentPreset().name == "Needs resolve");

  // an invalid preset is reported, not loaded
  std::ofstream(tmp.dir / "presets" / "bad.json") << "{";
  REQUIRE(flow.load(tmp.dir / "presets" / "bad.json", SubBank::User));
  CHECK(outcomes.back().status == PresetLoadFlow::Outcome::Status::Invalid);
}

TEST_CASE("Classic factory presets resolve through TONE3000 like Matched ones: NeedsResolve when uncached, Direct and loadable when cached", "[presets][resolve][classic]") {
  TempDir tmp;
  EnvVar appdata("SAWBLADE_APPDATA", (tmp.dir / "data").string());
  EnvVar cache("SAWBLADE_CACHE_DIR", (tmp.dir / "cache").string());
  const fs::path factory = fs::path(SAWBLADE_PRESETS_DIR);
  const char* const classics[] = {"chainsaw_body", "studio_split", "swedeath_saw", "tight_body"};

  // Nothing cached: every Classic preset needs a resolve (not a red "file not found"), written under resolved/classic/.
  for (const char* stem : classics) {
    INFO(stem);
    const LoadPlan plan = planPresetLoad(factory / (std::string(stem) + ".json"), SubBank::Classic);
    CHECK(plan.kind == LoadPlan::Kind::NeedsResolve);
    CHECK(plan.resolvedOut == tmp.dir / "data" / "resolved" / "classic" / (std::string(stem) + ".resolved.json"));
  }

  // The flow runs `sawblade-t3k resolve <preset> -o <out> --progress-json` and loads the result.
  SawbladeProcessor proc;
  proc.prepareToPlay(48000.0, 512);
  REQUIRE(proc.loadPresetFile(writeIdentityPreset(tmp.dir, "init", 0)));
  REQUIRE(proc.waitForLoader());
  const fs::path prepared = writeResolvedCopy(tmp.dir);
  const fs::path argsFile = tmp.dir / "args.txt";
  fs::create_directories(tmp.dir / "data");
  std::ofstream(tmp.dir / "data" / "settings.json")
      << json{{"t3kExecutable", script(tmp.dir, "fake-t3k", "echo \"$@\" > \"" + argsFile.string() + "\"\ncp \"" + prepared.string() + "\" \"$4\"").string()}}.dump();
  Queue queue;
  std::vector<PresetLoadFlow::Outcome> outcomes;
  PresetLoadFlow flow(proc, {nullptr, [&](const PresetLoadFlow::Outcome& o) { outcomes.push_back(o); }},
                      [&](std::function<void()> f) { queue.post(std::move(f)); });
  const fs::path chainsaw = factory / "chainsaw_body.json";
  REQUIRE(flow.load(chainsaw, SubBank::Classic));
  queue.pumpUntil([&] { return !outcomes.empty(); });
  REQUIRE(outcomes.size() == 1);
  CHECK(outcomes[0].status == PresetLoadFlow::Outcome::Status::Loaded);
  CHECK(outcomes[0].message.empty());
  std::string args;
  std::getline(std::ifstream(argsFile), args);
  CHECK(args == "resolve " + chainsaw.string() + " -o " + outcomes[0].loadedFile.string() + " --progress-json");
  REQUIRE(proc.waitForLoader());

  // Everything cached (stand-ins for `sawblade-t3k fetch`): Direct, and the real preset loads without error.
  struct Pair { const char* id; const char* model; const char* ext; };
  for (const Pair& c : {Pair{"58569", "496942", "nam"}, Pair{"86089", "731435", "nam"}, Pair{"70977", "584871", "nam"},
                        Pair{"84863", "721117", "wav"}, Pair{"75087", "656946", "wav"}}) {
    fs::create_directories(tmp.dir / "cache" / c.id);
    fs::copy_file(kFixtures / (std::string(c.ext) == "nam" ? "nam/linear_identity.nam" : "ir/impulse.wav"),
                  tmp.dir / "cache" / c.id / (std::string(c.model) + "." + c.ext));
  }
  for (const char* stem : classics) {
    INFO(stem);
    const fs::path pf = factory / (std::string(stem) + ".json");
    CHECK(planPresetLoad(pf, SubBank::Classic).kind == LoadPlan::Kind::Direct);
    REQUIRE(proc.loadPresetFile(pf));
    REQUIRE(proc.waitForLoader());
    CHECK(proc.status().error.empty());
  }
}

TEST_CASE("resolve flow: cancel kills the tool and keeps the sound", "[presets][resolve]") {
  TempDir tmp;
  EnvVar appdata("SAWBLADE_APPDATA", (tmp.dir / "data").string());
  EnvVar cache("SAWBLADE_CACHE_DIR", (tmp.dir / "emptycache").string());
  SawbladeProcessor proc;
  proc.prepareToPlay(48000.0, 512);
  REQUIRE(proc.loadPresetFile(writeIdentityPreset(tmp.dir, "init", 0)));
  REQUIRE(proc.waitForLoader());
  const fs::path pf = writeUnresolved(tmp.dir / "presets");
  fs::create_directories(tmp.dir / "data");
  std::ofstream(tmp.dir / "data" / "settings.json")
      << json{{"t3kExecutable", script(tmp.dir, "slow", "echo '{\"done\": 1, \"total\": 5, \"capture\": \"x\", \"title\": \"t\"}'\nexec sleep 60").string()}}.dump();
  Queue queue;
  std::vector<PresetLoadFlow::Outcome> outcomes;
  int progressed = 0;
  PresetLoadFlow flow(proc, {[&](int, int, const std::string&) { ++progressed; }, [&](const PresetLoadFlow::Outcome& o) { outcomes.push_back(o); }},
                      [&](std::function<void()> f) { queue.post(std::move(f)); });
  REQUIRE(flow.load(pf, SubBank::User));
  CHECK(flow.resolving());
  CHECK_FALSE(flow.load(pf, SubBank::User));  // busy
  queue.pumpUntil([&] { return progressed > 0; });
  flow.cancel();
  queue.pumpUntil([&] { return !outcomes.empty(); });
  REQUIRE(outcomes.size() == 1);
  CHECK(outcomes[0].status == PresetLoadFlow::Outcome::Status::Cancelled);
  CHECK(proc.currentPreset().name == "init");
}

TEST_CASE("info panel: every capture's creator and licence, the NC tag and the no-attribution line", "[presets][info]") {
  juce::ScopedJuceInitialiser_GUI gui;
  PresetEntry e;
  e.name = "Test";
  e.category = "Grind";
  e.bank = SubBank::Styles;
  e.file = "/p/test.json";
  e.notes = "some notes";
  auto cap = [](const std::string& where, const std::string& title, const std::string& creator, const std::string& lic, bool src_) {
    CaptureSummary c;
    c.where = where;
    c.title = title;
    c.creator = creator;
    c.license = lic;
    c.url = "https://www.tone3000.com/tones/" + title;
    c.hasSource = src_;
    c.file = "local.wav";
    c.nonCommercial = nonCommercialLicense(lic);
    return c;
  };
  e.captures = {cap("Path A / a1 (amp)", "HM2", "alice", "cc-by-nc", true), cap("Path B / b1 (amp)", "Body", "bob", "cc-by", true),
                cap("Cab irA (mic 1)", "V30", "carol", "CC-BY-NC-SA", true), cap("Cab irB (mic 2)", "", "", "", false)};
  PresetInfoPanel panel;
  panel.setBounds(0, 0, 518, 600);
  panel.setEntry(&e);
  const juce::String text = panel.plainText();
  for (const char* s : {"Test", "GRIND", "STYLES", "/p/test.json", "some notes", "Path A / a1 (amp)", "@alice", "cc-by-nc", "@bob", "cc-by", "@carol",
                        "CC-BY-NC-SA", "Cab irB (mic 2)", "https://www.tone3000.com/tones/HM2"})
    CHECK(text.contains(s));
  CHECK(text.contains("local file: no attribution recorded"));
  CHECK(panel.nonCommercialTags() == 2);  // alice (cc-by-nc) and carol (cc-by-nc-sa), not bob
  CHECK(text.contains("NON-COMMERCIAL"));
  CHECK(nonCommercialLicense("cc-by-nc-nd"));
  CHECK_FALSE(nonCommercialLicense("cc-by"));
  CHECK_FALSE(nonCommercialLicense("t3k"));
  CHECK_FALSE(nonCommercialLicense("cc0"));

  // an unloadable preset shows its error and no captures
  PresetEntry bad;
  bad.name = "broken";
  bad.error = "invalid JSON";
  panel.setEntry(&bad);
  CHECK(panel.plainText().contains("invalid JSON"));
  CHECK_FALSE(panel.plainText().contains("CAPTURES"));
  panel.setEntry(nullptr);
  CHECK(panel.rows().isEmpty());

  // summaries from a real preset: perPath and irMix cabs list both IRs
  Preset p = parsePreset(preset("m", "", "", namBlock("a.nam", src("1", "2", "T", "c", "cc-by-nc")), irRef()), "/");
  p.cab.mode = CabMode::IrMix;
  p.cab.irA.file = "a.wav";
  p.cab.irB.file = "b.wav";
  const auto s = summariseCaptures(p);
  REQUIRE(s.size() == 3);
  CHECK(s[0].nonCommercial);
  CHECK(s[1].where.find("irA") != std::string::npos);
  CHECK(s[2].where.find("irB") != std::string::npos);
}
