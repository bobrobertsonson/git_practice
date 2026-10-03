// T4: render entry point, golden renders, tonerender CLI, factory presets.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "dr_wav.h"
#include "latency_stub.h"
#include "sawblade/render.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = SAWBLADE_FIXTURES_DIR;
const fs::path kPresetDir = kFixtures / "presets";
const fs::path kGoldenDir = SAWBLADE_GOLDEN_DIR;
const fs::path kDi = kFixtures / "di_riff.wav";
constexpr double kGoldenTol = 1e-4;

struct TempDir {
  fs::path dir;
  TempDir() {
    static int counter = 0;
    dir = fs::temp_directory_path() / ("sawblade_render_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~TempDir() { std::error_code ec; fs::remove_all(dir, ec); }
  fs::path operator/(const std::string& f) const { return dir / f; }
};

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
  return m;
}

bool updateGolden() {
  const char* e = std::getenv("SAWBLADE_UPDATE_GOLDEN");
  return e && std::string(e) == "1";
}

// Compares `r` with tests/golden/<name>.wav (or rewrites it with SAWBLADE_UPDATE_GOLDEN=1).
void checkGolden(const std::string& name, const RenderResult& r) {
  const fs::path golden = kGoldenDir / (name + ".wav");
  if (updateGolden()) {
    fs::create_directories(kGoldenDir);
    writeRenderedWav(golden, r);
    WARN("SAWBLADE_UPDATE_GOLDEN=1: rewrote " << golden.string());
    return;
  }
  REQUIRE(fs::exists(golden));  // run with SAWBLADE_UPDATE_GOLDEN=1 to create it
  const AudioFile g = readWav(golden);
  REQUIRE(g.channels == 1);
  REQUIRE(g.sampleRate == r.sampleRate);
  REQUIRE(g.interleaved.size() == r.samples.size());
  REQUIRE(maxAbsDiff(g.interleaved, r.samples) <= kGoldenTol);
}

// ---- CLI helpers ------------------------------------------------------------------------------
std::string q(const fs::path& p) { return "'" + p.string() + "'"; }

int runCli(const std::string& args, const fs::path& errFile) {
  const std::string cmd = q(SAWBLADE_TONERENDER_EXE) + " " + args + " >/dev/null 2>" + q(errFile);
  const int st = std::system(cmd.c_str());
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

json presetJson(const std::string& name = "t") {
  return {{"schema", "sawblade.preset"}, {"version", 1}, {"name", name},
          {"paths", {{"a", {{"blocks", json::array({{{"id", "a1"}, {"type", "nam"}, {"model", {{"file", "../nam/linear_identity.nam"}}}}})}}},
                     {"b", {{"blocks", json::array({{{"id", "b1"}, {"type", "nam"}, {"model", {{"file", "../nam/linear_identity.nam"}}}}})}}}}},
          {"align", {{"mode", "off"}}},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}}};
}

AudioFile mono(const std::vector<float>& x, double fs = 48000.0) { return AudioFile{fs, 1, x}; }

}  // namespace

// ---- golden renders -----------------------------------------------------------------------------
TEST_CASE("Golden: golden_shared render matches tests/golden", "[golden]") {
  const RenderResult r = renderFile(kPresetDir / "golden_shared.json", kDi);
  REQUIRE(r.samples.size() == 192000);
  REQUIRE(r.info.liveCompatible);
  REQUIRE(r.input.peakDbfs < -5.0);
  REQUIRE(r.output.rmsDbfs > -60.0);  // not a vacuous comparison
  checkGolden("golden_shared", r);
}

TEST_CASE("Golden: golden_perpath render matches tests/golden", "[golden]") {
  const RenderResult r = renderFile(kPresetDir / "golden_perpath.json", kDi);
  REQUIRE(r.samples.size() == 192000);
  REQUIRE_FALSE(r.info.liveCompatible);
  REQUIRE(r.info.alignDelay[1] == 12);
  REQUIRE(r.output.rmsDbfs > -60.0);
  checkGolden("golden_perpath", r);
}

TEST_CASE("Golden: render is bit-identical across runs and block sizes agree", "[golden]") {
  const RenderResult a = renderFile(kPresetDir / "golden_shared.json", kDi);
  const RenderResult b = renderFile(kPresetDir / "golden_shared.json", kDi);
  REQUIRE(a.samples == b.samples);
  RenderOptions o;
  o.blockSize = 77;
  const RenderResult c = renderFile(kPresetDir / "golden_shared.json", kDi, o);
  REQUIRE(maxAbsDiff(a.samples, c.samples) <= 1e-5);
}

// ---- renderPreset -------------------------------------------------------------------------------
TEST_CASE("Render: output is advanced by the reported latency and keeps the input length", "[render]") {
  test::registerLatencyStub();
  json j = presetJson();
  j["paths"]["a"]["blocks"] = json::array({{{"id", "s1"}, {"type", "test.latency"}, {"latency", 100}}});
  const Preset p = parsePreset(j, kPresetDir);
  const auto x = noise(5000, 3, 0.3f);
  const RenderResult r = renderPreset(p, mono(x));
  REQUIRE(r.info.latencySamples == 100);
  REQUIRE(r.samples.size() == x.size());
  // Identity paths + 100-sample latency + advance by 100 = the input itself (tail flushed with zeros).
  REQUIRE(maxAbsDiff(r.samples, x) <= 1e-6);
  REQUIRE(r.realTimeFactor > 0.0);
}

TEST_CASE("Render: alignment delay stays in the audio (only latency is removed)", "[render]") {
  json j = presetJson();
  j["paths"]["b"]["blocks"][0]["model"]["file"] = "../nam/linear_neg1_at_23.nam";
  j["align"] = {{"mode", "auto"}};
  const auto x = noise(5000, 4, 0.3f);
  const RenderResult r = renderPreset(parsePreset(j, kPresetDir), mono(x));
  REQUIRE(r.info.latencySamples == 0);
  REQUIRE(r.info.alignDelay[0] == 23);
  REQUIRE(r.info.align.invertB);
  for (std::size_t i = 23; i < x.size(); ++i) REQUIRE(std::fabs(r.samples[i] - x[i - 23]) <= 1e-5);
}

TEST_CASE("Render: peak normalization, stereo input, silence", "[render]") {
  const Preset p = parsePreset(presetJson(), kPresetDir);
  const auto x = noise(4000, 5, 0.1f);
  {
    RenderOptions o;
    o.normalizePeakDbfs = -3.0;
    const RenderResult r = renderPreset(p, mono(x), o);
    REQUIRE(std::fabs(r.output.peakDbfs - (-3.0)) < 1e-4);
    REQUIRE(r.normalizeGainDb > 0.0);
  }
  {  // stereo: left channel only, with a warning
    AudioFile st{48000.0, 2, {}};
    for (float v : x) { st.interleaved.push_back(v); st.interleaved.push_back(-v); }
    const RenderResult r = renderPreset(p, st);
    REQUIRE(r.samples.size() == x.size());
    REQUIRE(r.warnings.size() >= 1);
    REQUIRE_THAT(r.warnings[0], ContainsSubstring("channels"));
    REQUIRE(maxAbsDiff(r.samples, renderPreset(p, mono(x)).samples) == 0.0);
  }
  {  // digital silence: -inf stats are reported as null, normalization is skipped with a warning
    RenderOptions o;
    o.normalizePeakDbfs = -3.0;
    const RenderResult r = renderPreset(p, mono(std::vector<float>(2000, 0.0f)), o);
    REQUIRE_FALSE(std::isfinite(r.output.peakDbfs));
    const json rep = reportJson(r);
    REQUIRE(rep["output"]["peakDbfs"].is_null());
    REQUIRE_FALSE(r.warnings.empty());
  }
  REQUIRE_THROWS_AS(renderPreset(p, mono({})), RenderError);
  RenderOptions bad;
  bad.blockSize = 0;
  REQUIRE_THROWS_AS(renderPreset(p, mono(x), bad), RenderError);
}

TEST_CASE("Render: report carries info fields and capture attributions", "[render]") {
  json j = presetJson();
  j["paths"]["a"]["blocks"][0]["model"]["source"] = {{"provider", "tone3000"}, {"id", "7"}, {"title", "Test amp"}, {"creator", "someone"}, {"license", "CC-BY-4.0"}};
  j["cab"]["ir"]["source"] = {{"provider", "tone3000"}, {"id", "8"}};
  const RenderResult r = renderPreset(parsePreset(j, kPresetDir), mono(noise(2000, 6)));
  const json rep = reportJson(r);
  for (const char* k : {"preset", "sampleRate", "blockSize", "latencySamples", "pathLatency", "compensationDelay", "alignDelay", "align",
                        "liveCompatible", "exportExactness", "input", "output", "renderSeconds", "realTimeFactor", "warnings", "captures"})
    REQUIRE(rep.contains(k));
  REQUIRE(rep["align"]["resolved"].contains("delaySamplesB"));
  REQUIRE(rep["align"]["resolved"].contains("invertB"));
  REQUIRE(rep["align"]["resolved"].contains("peakCorrelation"));
  REQUIRE(rep["captures"].size() == 2);
  REQUIRE(rep["captures"][0]["where"] == "paths.a.blocks[0].model");
  REQUIRE(rep["captures"][0]["title"] == "Test amp");
  REQUIRE(rep["captures"][0]["creator"] == "someone");
  REQUIRE(rep["captures"][0]["license"] == "CC-BY-4.0");
  REQUIRE(rep["captures"][1]["where"] == "cab.ir");
}

// ---- error classification -----------------------------------------------------------------------
namespace {
RenderErrorKind kindOf(const std::function<void()>& f) {
  try {
    f();
  } catch (const RenderError& e) {
    INFO(e.what());
    return e.kind();
  }
  FAIL("expected RenderError");
  return RenderErrorKind::Io;
}
}  // namespace

TEST_CASE("Render: semantic preset errors map to Preset, I/O and model load errors to Io", "[render][errors]") {
  const auto x = noise(2000, 7);
  // EQ band above 0.49 fs at the render rate: valid JSON, invalid only for this sample rate.
  json eqHigh = presetJson();
  eqHigh["postEq"] = json::array({{{"type", "peak"}, {"freq", 24000.0}, {"gainDb", 1.0}, {"q", 1.0}}});
  const Preset pe = parsePreset(eqHigh, kPresetDir);
  REQUIRE(kindOf([&] { renderPreset(pe, mono(x)); }) == RenderErrorKind::Preset);
  try {
    renderPreset(pe, mono(x));
  } catch (const RenderError& e) {
    REQUIRE_THAT(std::string(e.what()), ContainsSubstring("postEq"));
  }
  // The same band is fine at 96 kHz.
  REQUIRE_NOTHROW(renderPreset(pe, mono(x, 96000.0)));
  // ... and an eq *block* band reports its block path.
  json eqBlock = presetJson();
  eqBlock["paths"]["a"]["blocks"] = json::array({{{"id", "e1"}, {"type", "eq"}, {"bands", json::array({{{"type", "lowPass"}, {"freq", 30000.0}}})}}});
  REQUIRE(kindOf([&] { renderPreset(parsePreset(eqBlock, kPresetDir), mono(x, 44100.0)); }) == RenderErrorKind::Preset);

  // NAM sample-rate mismatch (the model is 48 kHz): surfaces in prepare().
  json nam = presetJson();
  nam["paths"]["a"]["blocks"][0]["model"]["file"] = "../nam/wavenet.nam";
  const Preset pn = parsePreset(nam, kPresetDir);
  REQUIRE(kindOf([&] { renderPreset(pn, mono(x, 44100.0)); }) == RenderErrorKind::Preset);

  // Missing model / IR file, and a model that is not a NAM file: Io.
  json missing = presetJson();
  missing["paths"]["a"]["blocks"][0]["model"]["file"] = "../nam/nope.nam";
  REQUIRE(kindOf([&] { renderPreset(parsePreset(missing, kPresetDir), mono(x)); }) == RenderErrorKind::Io);
  json missingIr = presetJson();
  missingIr["cab"]["ir"]["file"] = "../ir/nope.wav";
  REQUIRE(kindOf([&] { renderPreset(parsePreset(missingIr, kPresetDir), mono(x)); }) == RenderErrorKind::Io);
  TempDir t;
  { std::ofstream(t / "garbage.nam") << "this is not a model"; }
  json notNam = presetJson();
  notNam["paths"]["a"]["blocks"][0]["model"]["file"] = (t / "garbage.nam").string();
  REQUIRE(kindOf([&] { renderPreset(parsePreset(notNam, kPresetDir), mono(x)); }) == RenderErrorKind::Io);

  // renderFile: bad preset content -> Preset; missing preset / input file -> Io.
  { std::ofstream(t / "bad.json") << R"({"schema":"sawblade.preset","version":1,"name":"x","bogus":1})"; }
  REQUIRE(kindOf([&] { renderFile(t / "bad.json", kDi); }) == RenderErrorKind::Preset);
  REQUIRE(kindOf([&] { renderFile(t / "absent.json", kDi); }) == RenderErrorKind::Io);
  REQUIRE(kindOf([&] { renderFile(kPresetDir / "golden_shared.json", t / "absent.wav"); }) == RenderErrorKind::Io);
}

// ---- tonerender CLI -----------------------------------------------------------------------------
TEST_CASE("CLI: golden preset renders, matches the golden, report parses", "[cli]") {
  TempDir t;
  for (const char* name : {"golden_shared", "golden_perpath"}) {
    INFO(name);
    const fs::path out = t / (std::string(name) + ".wav"), rep = t / (std::string(name) + ".json");
    const int rc = runCli("--preset " + q(kPresetDir / (std::string(name) + ".json")) + " --in " + q(kDi) + " --out " + q(out) +
                              " --report " + q(rep),
                          t / "err.txt");
    INFO(slurp(t / "err.txt"));
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(out));
    const AudioFile o = readWav(out);
    REQUIRE(o.channels == 1);
    REQUIRE(o.sampleRate == 48000.0);
    if (!updateGolden()) {
      const AudioFile g = readWav(kGoldenDir / (std::string(name) + ".wav"));
      REQUIRE(o.interleaved.size() == g.interleaved.size());
      REQUIRE(maxAbsDiff(o.interleaved, g.interleaved) <= kGoldenTol);
    }
    const json r = json::parse(slurp(rep));
    REQUIRE(r.contains("liveCompatible"));
    REQUIRE(r["liveCompatible"].is_boolean());
    REQUIRE(r["liveCompatible"].get<bool>() == (std::string(name) == "golden_shared"));
    REQUIRE(r["frames"] == 192000);
    REQUIRE(r["realTimeFactor"].get<double>() > 0.0);
  }
}

TEST_CASE("CLI: --block and --normalize-peak", "[cli]") {
  TempDir t;
  const int rc = runCli("--preset " + q(kPresetDir / "golden_shared.json") + " --in " + q(kDi) + " --out " + q(t / "o.wav") +
                            " --block 100 --normalize-peak -1.5 --report " + q(t / "r.json"),
                        t / "err.txt");
  REQUIRE(rc == 0);
  const json r = json::parse(slurp(t / "r.json"));
  REQUIRE(r["blockSize"] == 100);
  REQUIRE(std::fabs(r["output"]["peakDbfs"].get<double>() - (-1.5)) < 1e-3);
  const AudioFile o = readWav(t / "o.wav");
  float peak = 0.0f;
  for (float v : o.interleaved) peak = std::max(peak, std::fabs(v));
  REQUIRE(std::fabs(20.0 * std::log10(peak) - (-1.5)) < 1e-3);
}

TEST_CASE("CLI: exit codes", "[cli]") {
  TempDir t;
  const fs::path err = t / "err.txt";
  const std::string good = "--preset " + q(kPresetDir / "golden_shared.json") + " --in " + q(kDi) + " --out " + q(t / "o.wav");

  SECTION("usage errors -> 2") {
    REQUIRE(runCli("", err) == 2);
    REQUIRE(runCli("--preset " + q(kPresetDir / "golden_shared.json"), err) == 2);
    REQUIRE(runCli(good + " --bogus", err) == 2);
    REQUIRE(runCli(good + " --block 0", err) == 2);
    REQUIRE(runCli(good + " --block abc", err) == 2);
    REQUIRE(runCli(good + " --normalize-peak", err) == 2);
    REQUIRE_THAT(slurp(err), ContainsSubstring("usage"));
    REQUIRE(runCli("--help", err) == 0);
  }
  SECTION("bad preset -> 3") {
    { std::ofstream(t / "bad.json") << R"({"schema":"sawblade.preset","version":1,"name":"x","paths":{"a":{},"b":{}},"cab":{"mode":"shared","ir":{"file":"x.wav"}},"bogus":1})"; }
    REQUIRE(runCli("--preset " + q(t / "bad.json") + " --in " + q(kDi) + " --out " + q(t / "o.wav"), err) == 3);
    REQUIRE_THAT(slurp(err), ContainsSubstring("bogus"));
    { std::ofstream(t / "garbage.json") << "{ not json"; }
    REQUIRE(runCli("--preset " + q(t / "garbage.json") + " --in " + q(kDi) + " --out " + q(t / "o.wav"), err) == 3);
    REQUIRE_FALSE(fs::exists(t / "o.wav"));
  }
  SECTION("semantic preset error at load time (EQ band >= 0.49 fs) -> 3") {
    json j = presetJson();
    j["postEq"] = json::array({{{"type", "peak"}, {"freq", 24000.0}, {"gainDb", 1.0}, {"q", 1.0}}});
    j["paths"]["a"]["blocks"][0]["model"]["file"] = (kFixtures / "nam" / "linear_identity.nam").string();
    j["paths"]["b"]["blocks"][0]["model"]["file"] = (kFixtures / "nam" / "linear_identity.nam").string();
    j["cab"]["ir"]["file"] = (kFixtures / "ir" / "impulse.wav").string();
    { std::ofstream(t / "eq.json") << j.dump(); }
    REQUIRE(runCli("--preset " + q(t / "eq.json") + " --in " + q(kDi) + " --out " + q(t / "o.wav"), err) == 3);
    REQUIRE_THAT(slurp(err), ContainsSubstring("postEq"));
  }
  SECTION("NAM sample-rate mismatch -> 3") {
    const fs::path di44 = t / "di44.wav";
    writeWavFloat32(di44, 44100.0, noise(4410, 9));
    REQUIRE(runCli("--preset " + q(kPresetDir / "golden_shared.json") + " --in " + q(di44) + " --out " + q(t / "o.wav"), err) == 3);
    REQUIRE_THAT(slurp(err), ContainsSubstring("sample rate"));
  }
  SECTION("missing input -> 4") {
    REQUIRE(runCli("--preset " + q(kPresetDir / "golden_shared.json") + " --in " + q(t / "absent.wav") + " --out " + q(t / "o.wav"), err) == 4);
    REQUIRE_THAT(slurp(err), ContainsSubstring("absent.wav"));
  }
  SECTION("missing preset file, missing model, unwritable output -> 4") {
    REQUIRE(runCli("--preset " + q(t / "absent.json") + " --in " + q(kDi) + " --out " + q(t / "o.wav"), err) == 4);
    json j = presetJson();
    j["paths"]["a"]["blocks"][0]["model"]["file"] = (t / "nope.nam").string();
    { std::ofstream(t / "nomodel.json") << j.dump(); }
    REQUIRE(runCli("--preset " + q(t / "nomodel.json") + " --in " + q(kDi) + " --out " + q(t / "o.wav"), err) == 4);
    REQUIRE(runCli(good.substr(0, good.rfind("--out")) + "--out " + q(t / "no_such_dir" / "o.wav"), err) == 4);
  }
  SECTION("success -> 0") { REQUIRE(runCli(good, err) == 0); }
}

// ---- factory presets ----------------------------------------------------------------------------
TEST_CASE("Factory presets in presets/ all parse (no resource loading)", "[presets]") {
  const fs::path dir = SAWBLADE_PRESETS_DIR;
  int count = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() != ".json") continue;
    INFO(e.path().string());
    json j;
    {
      std::ifstream f(e.path());
      REQUIRE(f);
      j = json::parse(f);
    }
    REQUIRE_NOTHROW(parsePreset(j, dir));
    ++count;
  }
  REQUIRE(count >= 4);
}
