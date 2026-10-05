// Phase 11 settings tests (docs/specs/phase11_settings.md section 9, tests 1-9): the settings store,
// auto-detect, the secret-key rule, ToolRunner with fake scripts and the login state machine. Headless:
// temp dirs only, no network, no message loop (ToolRunner callbacks run on the worker thread).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include <cerrno>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "settings/LoginFlow.h"
#include "settings/Settings.h"
#include "settings/ToolRunner.h"

using namespace sawblade::plugin::settings;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;

namespace {

struct TempDir {
  fs::path dir;
  TempDir() {
    static std::atomic<int> n{0};
    dir = fs::temp_directory_path() / ("sawblade_settings_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  fs::path operator/(const char* s) const { return dir / s; }
};

// An Env that lives under `root`: home = root/home, an injectable variable table, the real filesystem.
Env makeEnv(const fs::path& root, std::map<std::string, std::string> vars = {}, bool mac = false) {
  Env e;
  auto table = std::make_shared<std::map<std::string, std::string>>(std::move(vars));
  e.getenv = [table](std::string_view n) -> std::optional<std::string> {
    auto it = table->find(std::string(n));
    if (it == table->end()) return std::nullopt;
    return it->second;
  };
  e.home = root / "home";
  e.executable = "/usr/bin/host";
  e.sourceDir = "";
  e.exists = [](const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
  };
  e.isMac = mac;
  return e;
}

void touch(const fs::path& p, const std::string& content = "") {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << content;
}
void makeVenv(const fs::path& venv, bool t3k = true, bool match = true) {
  if (t3k) touch(venv / "bin" / "sawblade-t3k", "#!/bin/sh\n");
  if (match) touch(venv / "bin" / "sawblade-match", "#!/bin/sh\n");
}
std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
fs::path tool(const char* name) { return fs::path(SAWBLADE_TEST_TOOLS_DIR) / name; }

}  // namespace

// --- 1 ------------------------------------------------------------------------------------------
TEST_CASE("settings: round trip keeps every key and unknown keys, file is 0600, no .tmp left", "[settings]") {
  TempDir t;
  const fs::path file = t / "cfg/settings.json";
  {
    Settings s(file, makeEnv(t.dir));
    REQUIRE(s.load().empty());
    CHECK(s.setMatchVenvDir("/venv/x").ok);
    CHECK(s.setCaptureCacheDir("/cache/x").ok);
    CHECK(s.setTone3000ClientId("t3k_pub_abc").ok);
    CHECK(s.setSeparationModel("htdemucs_ft").ok);
    CHECK(s.setTakesDir("/takes/x").ok);
    CHECK(s.setTheme("dark").ok);
    CHECK(s.setUiScale(1.25).ok);
    s.markFirstRunCompleted();
  }
  // a parallel session adds a key
  auto j = nlohmann::json::parse(slurp(file));
  CHECK(j["version"] == 1);
  j["future"] = {{"x", 1}};
  std::ofstream(file) << j.dump();
  {
    Settings s(file, makeEnv(t.dir));
    REQUIRE(s.load().empty());
    CHECK(s.matchVenvDir() == fs::path("/venv/x"));
    CHECK(s.captureCacheDir() == fs::path("/cache/x"));
    CHECK(s.tone3000ClientId() == "t3k_pub_abc");
    CHECK(s.separationModel() == "htdemucs_ft");
    CHECK(s.takesDir() == fs::path("/takes/x"));
    CHECK(s.theme() == "dark");
    CHECK(s.uiScale() == 1.25);
    CHECK(s.firstRunCompleted());
    CHECK(s.setUiScale(1.5).ok);  // saves again
  }
  const auto j2 = nlohmann::json::parse(slurp(file));
  CHECK(j2["future"]["x"] == 1);
  CHECK(j2["uiScale"] == 1.5);
  struct stat st {};
  REQUIRE(::stat(file.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  REQUIRE(::stat(file.parent_path().c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0700);
  CHECK_FALSE(fs::exists(file.string() + ".tmp"));
}

TEST_CASE("settings: setters validate", "[settings]") {
  TempDir t;
  Settings s(t / "s.json", makeEnv(t.dir));
  CHECK(s.setUiScale(9.0).ok);
  CHECK(s.uiScale() == 2.0);
  CHECK(s.setUiScale(0.1).ok);
  CHECK(s.uiScale() == 0.5);
  CHECK_FALSE(s.setSeparationModel("spleeter").ok);
  CHECK(s.separationModel() == "htdemucs_6s");
  CHECK_FALSE(s.setTheme("light").ok);
  CHECK(s.setMatchVenvDir("~/not expanded").ok);
  CHECK(s.matchVenvDir() == fs::path("~/not expanded"));
  CHECK(s.setMatchVenvDir(std::nullopt).ok);
  CHECK_FALSE(s.matchVenvDir().has_value());
}

// --- 2 ------------------------------------------------------------------------------------------
TEST_CASE("settings: defaults with no file; a malformed file gives defaults, an error, and is overwritten by save", "[settings]") {
  TempDir t;
  const Env env = makeEnv(t.dir);
  {
    Settings s(t / "none.json", env);
    CHECK(s.load().empty());
    CHECK(s.separationModel() == "htdemucs_6s");
    CHECK(s.uiScale() == 1.0);
    CHECK_FALSE(s.matchVenvDir().has_value());
    CHECK_FALSE(s.effectiveMatchVenvDir().has_value());
    CHECK(s.effectiveCaptureCacheDir() == env.home / ".cache/sawblade/captures");
    CHECK(s.effectiveTakesDir() == env.home / ".local/share/sawblade/takes");
    CHECK(s.theme() == "dark");
    CHECK(s.effectiveTone3000ClientId().empty());
    CHECK_FALSE(fs::exists(t / "none.json"));  // load() never writes
  }
  {
    Env e2 = makeEnv(t.dir, {{"SAWBLADE_CACHE_DIR", (t.dir / "envcache").string()}});
    Settings s(t / "none.json", e2);
    s.load();
    CHECK(s.effectiveCaptureCacheDir() == t.dir / "envcache");
  }
  const fs::path bad = t / "bad.json";
  touch(bad, "{ this is not json");
  Settings s(bad, env);
  const std::string err = s.load();
  CHECK_FALSE(err.empty());
  CHECK(s.separationModel() == "htdemucs_6s");
  CHECK(s.uiScale() == 1.0);
  CHECK(s.save().empty());
  CHECK_FALSE(nlohmann::json::parse(slurp(bad), nullptr, false).is_discarded());
  Settings again(bad, env);
  CHECK(again.load().empty());
}

TEST_CASE("settings: paths follow the platform table", "[settings]") {
  TempDir t;
  const Env linux = makeEnv(t.dir);
  CHECK(Paths::settingsFile(linux) == linux.home / ".local/share/sawblade/settings.json");
  CHECK(Paths::tokenFile(linux) == linux.home / ".config/sawblade/t3k_tokens.json");
  const Env xdg = makeEnv(t.dir, {{"XDG_DATA_HOME", "/xdg"}});
  CHECK(Paths::settingsFile(xdg) == fs::path("/xdg/sawblade/settings.json"));  // same file as presets/T3kTool settingsFile()
  const Env appdata = makeEnv(t.dir, {{"SAWBLADE_APPDATA", "/ad"}});
  CHECK(Paths::settingsFile(appdata) == fs::path("/ad/settings.json"));
  CHECK(Paths::tokenFile(xdg) == xdg.home / ".config/sawblade/t3k_tokens.json");  // the Python tools ignore XDG
  const Env mac = makeEnv(t.dir, {}, true);
  CHECK(Paths::settingsFile(mac) == mac.home / "Library/Application Support/Sawblade/settings.json");
  const Env over = makeEnv(t.dir, {{"SAWBLADE_SETTINGS_FILE", "/o/s.json"}, {"SAWBLADE_T3K_TOKEN_FILE", "/o/tok.json"}}, true);
  CHECK(Paths::settingsFile(over) == fs::path("/o/s.json"));
  CHECK(Paths::tokenFile(over) == fs::path("/o/tok.json"));
}

// --- 3 ------------------------------------------------------------------------------------------
TEST_CASE("settings: first run", "[settings]") {
  TempDir t;
  const fs::path file = t / "first/settings.json";
  const Env env = makeEnv(t.dir);
  {
    Settings s(file, env);
    s.load();
    CHECK(s.isFirstRun());
    CHECK_FALSE(s.fileExists());
    s.markFirstRunCompleted();
    CHECK(s.fileExists());
    CHECK_FALSE(s.isFirstRun());
  }
  Settings fresh(file, env);
  fresh.load();
  CHECK_FALSE(fresh.isFirstRun());
  CHECK(fresh.firstRunCompleted());

  touch(t / "pre.json", "{\"version\": 1}");
  Settings pre(t / "pre.json", env);
  pre.load();
  CHECK_FALSE(pre.isFirstRun());
}

TEST_CASE("settings: listeners hear every save", "[settings]") {
  TempDir t;
  struct L : Settings::Listener {
    int n = 0;
    void settingsChanged() override { ++n; }
  } l;
  Settings s(t / "s.json", makeEnv(t.dir));
  s.addListener(&l);
  s.setUiScale(1.0);
  s.setSeparationModel("htdemucs");
  CHECK(l.n == 2);
  s.removeListener(&l);
  s.setUiScale(1.5);
  CHECK(l.n == 2);
}

// --- 4 ------------------------------------------------------------------------------------------
TEST_CASE("settings: match venv auto-detect order", "[settings][detect]") {
  TempDir t;
  const fs::path src = t / "checkout";
  const fs::path buildVenv = src / "match/.venv", homeVenv = t / "home/sawblade/match/.venv", envVenv = t / "elsewhere/venv";
  auto env = [&](std::map<std::string, std::string> vars, fs::path exe) {
    Env e = makeEnv(t.dir, std::move(vars));
    e.sourceDir = src;
    e.executable = std::move(exe);
    return e;
  };
  const fs::path inside = src / "build-plugin/plugin/Sawblade";

  SECTION("(e) nothing valid") { CHECK_FALSE(detectMatchVenv(env({}, inside)).has_value()); }

  makeVenv(homeVenv);
  SECTION("home layout is found last") { CHECK(detectMatchVenv(env({}, inside)) == homeVenv); }

  makeVenv(buildVenv);
  SECTION("(b) build tree beats home") { CHECK(detectMatchVenv(env({}, inside)) == buildVenv); }
  SECTION("(c) build tree ignored when the executable is outside sourceDir") {
    CHECK(detectMatchVenv(env({}, "/usr/lib/vst3/Sawblade.vst3/x")) == homeVenv);
    CHECK(detectMatchVenv(env({}, t / "checkout-other/bin")) == homeVenv);  // prefix of the name is not "inside"
  }

  makeVenv(envVenv);
  SECTION("(a) env var beats build tree") { CHECK(detectMatchVenv(env({{"SAWBLADE_MATCH_VENV", envVenv.string()}}, inside)) == envVenv); }

  SECTION("(d) an invalid candidate is skipped") {
    const fs::path bad = t / "bad/venv";
    fs::create_directories(bad / "bin");  // no sawblade-t3k
    CHECK(detectMatchVenv(env({{"SAWBLADE_MATCH_VENV", bad.string()}}, inside)) == buildVenv);
    fs::remove_all(buildVenv);
    CHECK(detectMatchVenv(env({{"SAWBLADE_MATCH_VENV", bad.string()}}, inside)) == homeVenv);
  }

  SECTION("(f) a stored matchVenvDir wins over everything, even when invalid") {
    Settings s(t / "s.json", env({{"SAWBLADE_MATCH_VENV", envVenv.string()}}, inside));
    s.load();
    CHECK(s.effectiveMatchVenvDir() == envVenv);
    REQUIRE(s.setMatchVenvDir(t / "nowhere").ok);
    CHECK(s.effectiveMatchVenvDir() == t / "nowhere");
    CHECK(s.toolPath("sawblade-t3k") == t / "nowhere/bin/sawblade-t3k");
    s.setMatchVenvDir(std::nullopt);
    CHECK(s.effectiveMatchVenvDir() == envVenv);
  }
}

// --- 5 ------------------------------------------------------------------------------------------
TEST_CASE("settings: cache dir precedence and takes dir per platform", "[settings]") {
  TempDir t;
  {
    Settings s(t / "s.json", makeEnv(t.dir, {{"SAWBLADE_CACHE_DIR", "/env/cache"}}));
    s.load();
    CHECK(s.effectiveCaptureCacheDir() == fs::path("/env/cache"));
    s.setCaptureCacheDir("/explicit");
    CHECK(s.effectiveCaptureCacheDir() == fs::path("/explicit"));
    s.setCaptureCacheDir(std::nullopt);
    CHECK(s.effectiveCaptureCacheDir() == fs::path("/env/cache"));
  }
  {
    Settings lin(t / "l.json", makeEnv(t.dir, {}, false));
    Settings mac(t / "m.json", makeEnv(t.dir, {}, true));
    CHECK(lin.effectiveTakesDir() == t.dir / "home/.local/share/sawblade/takes");
    CHECK(mac.effectiveTakesDir() == t.dir / "home/Library/Application Support/Sawblade/takes");
    mac.setTakesDir("/my/takes");
    CHECK(mac.effectiveTakesDir() == fs::path("/my/takes"));
  }
}

// --- 6 ------------------------------------------------------------------------------------------
TEST_CASE("settings: the secret key is refused and never stored", "[settings][secret]") {
  TempDir t;
  const fs::path file = t / "s.json";
  Settings s(file, makeEnv(t.dir));
  s.load();
  REQUIRE(s.setTone3000ClientId("t3k_pub_keep").ok);
  const std::string before = slurp(file);

  for (const char* bad : {"t3k_cs_abc", " T3K_CS_abc ", "xt3k_cs_y"}) {
    const Result r = s.setTone3000ClientId(bad);
    INFO(bad);
    CHECK_FALSE(r.ok);
    CHECK_THAT(r.error, ContainsSubstring("TONE3000 *secret* key"));
    CHECK_THAT(r.error, ContainsSubstring("Sawblade never stores it"));
    CHECK_THAT(r.error, ContainsSubstring("*publishable* key"));
    CHECK(s.tone3000ClientId() == "t3k_pub_keep");
    CHECK(slurp(file) == before);  // not written
  }

  Result r = s.setTone3000ClientId("  t3k_pub_abc  ");
  CHECK(r.ok);
  CHECK(r.warning.empty());
  CHECK(s.tone3000ClientId() == "t3k_pub_abc");
  r = s.setTone3000ClientId("other_key");
  CHECK(r.ok);
  CHECK_THAT(r.warning, ContainsSubstring("does not look like a publishable key"));
  CHECK(s.tone3000ClientId() == "other_key");
  CHECK(s.setTone3000ClientId("").ok);
  CHECK(s.tone3000ClientId().empty());

  // an existing file with a secret key loads with the key dropped and an error
  touch(t / "leak.json", "{\"version\":1,\"tone3000ClientId\":\"t3k_cs_leaked\"}");
  Settings leak(t / "leak.json", makeEnv(t.dir));
  const std::string err = leak.load();
  CHECK_FALSE(err.empty());
  CHECK_THAT(err, !ContainsSubstring("leaked"));
  CHECK(leak.tone3000ClientId().empty());
  leak.save();
  CHECK_THAT(slurp(t / "leak.json"), !ContainsSubstring("t3k_cs_"));

  // the env fallback never yields a secret key either
  Settings envS(t / "e.json", makeEnv(t.dir, {{"TONE3000_CLIENT_ID", "t3k_cs_fromenv"}}));
  CHECK(envS.effectiveTone3000ClientId().empty());
  Settings envP(t / "e2.json", makeEnv(t.dir, {{"TONE3000_CLIENT_ID", "t3k_pub_fromenv"}}));
  CHECK(envP.effectiveTone3000ClientId() == "t3k_pub_fromenv");
}

// --- 7 ------------------------------------------------------------------------------------------
TEST_CASE("tool resolution: venv, then PATH, then an error that names Settings", "[settings][toolrunner]") {
  TempDir t;
  const fs::path venv = t / "venv", pathDir = t / "pathdir";
  touch(pathDir / "sawblade-t3k", "#!/bin/sh\n");
  touch(pathDir / "only-on-path", "#!/bin/sh\n");
  const std::string path = "/nonexistent:" + pathDir.string();
  Settings s(t / "s.json", makeEnv(t.dir, {{"PATH", path}}));
  s.load();
  ToolRunner runner(s);
  std::string err;

  CHECK(runner.resolve("sawblade-t3k", &err) == pathDir / "sawblade-t3k");  // no venv: PATH
  makeVenv(venv);
  s.setMatchVenvDir(venv);
  CHECK(runner.resolve("sawblade-t3k", &err) == venv / "bin/sawblade-t3k");  // venv wins
  CHECK(runner.resolve("only-on-path", &err) == pathDir / "only-on-path");
  err.clear();
  CHECK(runner.resolve("sawblade-nothing", &err).empty());
  CHECK_THAT(err, ContainsSubstring("Settings"));
  CHECK_THAT(err, ContainsSubstring("sawblade-nothing"));
}

// --- 8 ------------------------------------------------------------------------------------------
namespace {
struct Collected {
  std::mutex m;
  std::condition_variable cv;
  std::vector<std::string> lines;
  std::vector<std::chrono::steady_clock::time_point> lineTimes;
  std::optional<ToolResult> done;
  std::chrono::steady_clock::time_point doneTime;
  int doneCalls = 0;
  bool runningAtFirstLine = false;
  std::shared_ptr<ToolRunner::Job> job;
};

std::shared_ptr<Collected> runTool(ToolRunner& r, ToolRequest req) {
  auto c = std::make_shared<Collected>();
  req.callbacksOnMessageThread = false;
  c->job = r.run(
      std::move(req),
      [c](const std::string& line) {
        std::lock_guard<std::mutex> lk(c->m);
        if (c->lines.empty() && c->job) c->runningAtFirstLine = c->job->isRunning();
        c->lines.push_back(line);
        c->lineTimes.push_back(std::chrono::steady_clock::now());
      },
      [c](const ToolResult& res) {
        std::lock_guard<std::mutex> lk(c->m);
        c->done = res;
        c->doneTime = std::chrono::steady_clock::now();
        ++c->doneCalls;
        c->cv.notify_all();
      });
  return c;
}

bool waitDone(Collected& c, std::chrono::milliseconds t) {
  std::unique_lock<std::mutex> lk(c.m);
  return c.cv.wait_for(lk, t, [&] { return c.done.has_value(); });
}

struct RunnerFixture {
  TempDir t;
  Settings s;
  ToolRunner runner;
  RunnerFixture() : s(t / "s.json", makeEnv(t.dir)), runner(s) { s.load(); }
  std::shared_ptr<Collected> run(const char* script, std::vector<std::string> args = {}, std::map<std::string, std::string> env = {},
                                 std::chrono::milliseconds timeout = 0ms) {
    ToolRequest r;
    r.tool = "x";
    r.executable = tool(script);
    r.args = std::move(args);
    r.env = std::move(env);
    r.timeout = timeout;
    return runTool(runner, std::move(r));
  }
};
}  // namespace

TEST_CASE("ToolRunner: lines stream as they arrive", "[toolrunner]") {
  RunnerFixture f;
  auto c = f.run("lines.sh");
  REQUIRE(waitDone(*c, 5000ms));
  std::lock_guard<std::mutex> lk(c->m);
  REQUIRE(c->lines.size() == 5);
  for (int i = 0; i < 5; ++i) CHECK(c->lines[static_cast<size_t>(i)] == "line " + std::to_string(i + 1));
  CHECK(c->runningAtFirstLine);  // the first line arrived while the job was still running
  CHECK(c->doneTime - c->lineTimes.front() > 100ms);
  CHECK(c->doneCalls == 1);
  CHECK(c->done->outcome == ToolResult::Outcome::Ok);
  CHECK(c->done->exitCode == 0);
  CHECK(c->done->lines.size() == 5);
  CHECK(c->done->text == "line 1\nline 2\nline 3\nline 4\nline 5");
  CHECK(c->done->executable == tool("lines.sh"));
  CHECK_FALSE(c->done->json.has_value());
  CHECK_FALSE(c->job->isRunning());
}

TEST_CASE("ToolRunner: a non-zero exit is reported", "[toolrunner]") {
  RunnerFixture f;
  auto c = f.run("exit3.sh");
  REQUIRE(waitDone(*c, 5000ms));
  CHECK(c->done->outcome == ToolResult::Outcome::NonZeroExit);
  CHECK(c->done->exitCode == 3);
  CHECK(c->done->lines == std::vector<std::string>{"about to fail"});
}

TEST_CASE("ToolRunner: JSON result", "[toolrunner]") {
  RunnerFixture f;
  auto c = f.run("json.sh");
  REQUIRE(waitDone(*c, 5000ms));
  REQUIRE(c->done->json.has_value());
  CHECK((*c->done->json)["username"] == "gate");
  CHECK((*c->done->json)["n"].size() == 3);
  auto p = f.run("json_pretty.sh");
  REQUIRE(waitDone(*p, 5000ms));
  REQUIRE(p->done->json.has_value());
  CHECK((*p->done->json)["name"] == "pretty");
  CHECK((*p->done->json)["items"].size() == 2);

  // extractJson never throws and ignores noise
  CHECK_FALSE(extractJson({"hello", "[1/3] downloading", "{not json"}).has_value());
  CHECK(extractJson({"{\"a\":1}", "trailing log"}).value()["a"] == 1);
  CHECK(extractJson({"{\"a\":1}", "{\"a\":2}"}).value()["a"] == 2);  // the last one wins
}

TEST_CASE("ToolRunner: cancel kills the child", "[toolrunner]") {
  RunnerFixture f;
  auto c = f.run("sleep.sh");
  std::this_thread::sleep_for(200ms);
  const auto t0 = std::chrono::steady_clock::now();
  c->job->cancel();
  REQUIRE(waitDone(*c, 2000ms));
  CHECK(std::chrono::steady_clock::now() - t0 < 2000ms);
  CHECK(c->done->outcome == ToolResult::Outcome::Cancelled);
  REQUIRE_FALSE(c->done->lines.empty());
  const int pid = std::stoi(c->done->lines.front());
  CHECK(pid > 1);
  CHECK(c->job->wait(1000ms));
  errno = 0;
  CHECK(::kill(pid, 0) == -1);
  CHECK(errno == ESRCH);
}

TEST_CASE("ToolRunner: the environment ToolRunner builds and request.env", "[toolrunner]") {
  RunnerFixture f;
  REQUIRE(f.s.setTone3000ClientId("t3k_pub_testkey").ok);
  auto c = f.run("env.sh", {}, {{"EXTRA", "extra value"}});
  REQUIRE(waitDone(*c, 5000ms));
  REQUIRE(c->done->outcome == ToolResult::Outcome::Ok);
  CHECK(c->done->lines[0] == "TONE3000_CLIENT_ID=t3k_pub_testkey");
  CHECK(c->done->lines[1] == "SAWBLADE_CACHE_DIR=" + f.s.effectiveCaptureCacheDir().string());
  CHECK(c->done->lines[2] == "PYTHONUNBUFFERED=1");
  CHECK(c->done->lines[3] == "EXTRA=extra value");
  // request.env wins over ToolRunner's own
  auto d = f.run("env.sh", {}, {{"PYTHONUNBUFFERED", "0"}});
  REQUIRE(waitDone(*d, 5000ms));
  CHECK(d->done->lines[2] == "PYTHONUNBUFFERED=0");
}

TEST_CASE("ToolRunner: secret keys in the output are redacted", "[toolrunner][secret]") {
  RunnerFixture f;
  auto c = f.run("secret.sh");
  REQUIRE(waitDone(*c, 5000ms));
  std::lock_guard<std::mutex> lk(c->m);
  REQUIRE(c->lines.size() == 1);
  CHECK(c->lines[0] == "token t3k_cs_[redacted] end");
  CHECK(c->done->lines[0] == "token t3k_cs_[redacted] end");
  CHECK(c->done->text.find("SECRET123") == std::string::npos);
  CHECK(redactSecrets("a t3k_cs_ab-C_9 b t3k_cs_x") == "a t3k_cs_[redacted] b t3k_cs_[redacted]");
  CHECK(redactSecrets("t3k_pub_ok") == "t3k_pub_ok");
}

TEST_CASE("ToolRunner: a missing tool is StartFailed and names Settings", "[toolrunner]") {
  RunnerFixture f;
  ToolRequest r;
  r.tool = "sawblade-does-not-exist";
  auto c = runTool(f.runner, r);
  REQUIRE(waitDone(*c, 5000ms));
  CHECK(c->done->outcome == ToolResult::Outcome::StartFailed);
  CHECK_THAT(c->done->error, ContainsSubstring("Settings"));
  CHECK(c->doneCalls == 1);

  ToolRequest r2;
  r2.tool = "x";
  r2.executable = f.t.dir / "no/such/file";
  auto c2 = runTool(f.runner, r2);
  REQUIRE(waitDone(*c2, 5000ms));
  CHECK(c2->done->outcome == ToolResult::Outcome::StartFailed);
  CHECK_THAT(c2->done->error, ContainsSubstring("Settings"));
}

TEST_CASE("ToolRunner: timeout", "[toolrunner]") {
  RunnerFixture f;
  const auto t0 = std::chrono::steady_clock::now();
  auto c = f.run("sleep.sh", {}, {}, 300ms);
  REQUIRE(waitDone(*c, 2000ms));
  CHECK(std::chrono::steady_clock::now() - t0 < 2000ms);
  CHECK(c->done->outcome == ToolResult::Outcome::TimedOut);
  CHECK_THAT(c->done->error, ContainsSubstring("timed out"));
}

TEST_CASE("ToolRunner: destroying the runner cancels and joins its jobs", "[toolrunner]") {
  TempDir t;
  Settings s(t / "s.json", makeEnv(t.dir));
  std::shared_ptr<Collected> c;
  const auto t0 = std::chrono::steady_clock::now();
  {
    ToolRunner runner(s);
    ToolRequest r;
    r.tool = "x";
    r.executable = tool("sleep.sh");
    c = runTool(runner, r);
    std::this_thread::sleep_for(150ms);
  }
  CHECK(std::chrono::steady_clock::now() - t0 < 2000ms);
  CHECK_FALSE(c->job->isRunning());  // the Job outlives the runner
  CHECK(c->job->result().outcome == ToolResult::Outcome::Cancelled);
}

// --- 9 ------------------------------------------------------------------------------------------
TEST_CASE("LoginFlow: parses the login --json events", "[settings][login]") {
  LoginFlow f;
  f.begin();
  CHECK(f.state == LoginFlow::State::Starting);
  CHECK_FALSE(f.feedLine("some log noise"));
  CHECK_FALSE(f.feedLine("{\"no_event\": 1}"));
  CHECK(f.feedLine(
      "{\"event\": \"device_code\", \"user_code\": \"ABCD-1234\", \"verification_uri\": \"https://t/device\", "
      "\"verification_uri_complete\": \"https://t/device?code=ABCD-1234\", \"expires_in\": 600}"));
  CHECK(f.state == LoginFlow::State::WaitingForApproval);
  CHECK(f.code == "ABCD-1234");
  CHECK(f.url == "https://t/device");
  CHECK(f.openUrl == "https://t/device?code=ABCD-1234");
  CHECK(f.expiresIn == 600);
  CHECK(f.feedLine("{\"event\": \"logged_in\", \"username\": \"gate\", \"display_name\": \"Gate Fan\", \"id\": \"u1\", \"token_file\": \"/x\"}"));
  CHECK(f.state == LoginFlow::State::LoggedIn);
  CHECK(f.loggedInAs == "gate");
  CHECK(f.loggedInText() == "Logged in as @gate (Gate Fan)");

  LoginFlow g;
  g.begin();
  g.feedLine("{\"event\": \"device_code\", \"user_code\": \"X\", \"verification_uri\": \"https://t/d\", \"verification_uri_complete\": null, \"expires_in\": 5}");
  CHECK(g.openUrl == "https://t/d");  // no complete uri: OPEN uses the plain one
  CHECK(g.feedLine("{\"event\": \"error\", \"message\": \"expired\"}"));
  CHECK(g.state == LoginFlow::State::Failed);
  CHECK(g.message == "expired");

  LoginFlow h;  // the process died without an event
  h.begin();
  ToolResult r;
  r.outcome = ToolResult::Outcome::NonZeroExit;
  r.exitCode = 1;
  r.lines = {"Traceback", "boom"};
  h.finish(r);
  CHECK(h.state == LoginFlow::State::Failed);
  CHECK(h.message == "boom");
}

TEST_CASE("whoami --json parsing", "[settings][login]") {
  ToolResult ok;
  ok.json = nlohmann::json{{"username", "gate"}, {"display_name", "Gate Fan"}};
  CHECK(parseWhoami(ok).ok);
  CHECK(parseWhoami(ok).text == "Logged in as @gate (Gate Fan)");
  ToolResult bad;
  bad.json = nlohmann::json{{"error", "TONE3000_CLIENT_ID is not set"}};
  CHECK_FALSE(parseWhoami(bad).ok);
  CHECK(parseWhoami(bad).text == "TONE3000_CLIENT_ID is not set");
  ToolResult none;
  none.error = "sawblade-t3k not found. Set the match venv in Settings (gear icon).";
  CHECK(parseWhoami(none).text == none.error);
}
