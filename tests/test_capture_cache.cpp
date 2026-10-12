// CaptureCache: reuse of parsed NAM models / IRs across renders, bit-identity with uncached renders,
// invalidation, error paths, and concurrent use.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include <unistd.h>

#include "sawblade/capture_cache.h"
#include "sawblade/render.h"
#include "sawblade/sha256.h"

using namespace sawblade;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = SAWBLADE_FIXTURES_DIR;
const fs::path kPresetDir = kFixtures / "presets";
const fs::path kDi = kFixtures / "di_riff.wav";

json readJson(const fs::path& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return json::parse(ss.str());
}

Preset load(const char* name, double blend = -1.0) {
  json j = readJson(kPresetDir / name);
  if (blend >= 0.0) j["blend"] = blend;
  return parsePreset(j, kPresetDir);
}

AudioFile shortDi() {
  AudioFile a = readWav(kDi);
  a.interleaved.resize(24000);  // 0.5 s keeps the tests fast
  return a;
}

struct TempDir {
  fs::path dir;
  TempDir() {
    static int n = 0;
    dir = fs::temp_directory_path() / ("sawblade_cache_test_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
    fs::create_directories(dir);
  }
  ~TempDir() { std::error_code ec; fs::remove_all(dir, ec); }
};

RenderResult renderWith(const Preset& p, const AudioFile& in, CaptureCache* cache) {
  RenderOptions o;
  o.cache = cache;
  return renderPreset(p, in, o);
}

}  // namespace

TEST_CASE("CaptureCache: cached renders are bit-identical to uncached (both golden presets)", "[cache]") {
  const AudioFile in = shortDi();
  for (const char* name : {"golden_shared.json", "golden_perpath.json"}) {
    const Preset p = load(name);
    const RenderResult ref = renderWith(p, in, nullptr);
    CaptureCache cache;
    const RenderResult first = renderWith(p, in, &cache);   // fills the cache
    const RenderResult second = renderWith(p, in, &cache);  // served from it
    REQUIRE(first.samples == ref.samples);
    REQUIRE(second.samples == ref.samples);
    REQUIRE(reportJson(first)["latencySamples"] == reportJson(ref)["latencySamples"]);
  }
}

TEST_CASE("CaptureCache: a second render with a different blend reloads nothing", "[cache]") {
  const AudioFile in = shortDi();
  CaptureCache cache;
  renderWith(load("golden_shared.json", 0.55), in, &cache);
  const auto s1 = cache.stats();
  REQUIRE(s1.files == 4);   // wavenet, lstm, linear_identity, ir_a
  REQUIRE(s1.misses == 4);  // one load per distinct capture; repeats within the render are hits
  REQUIRE(s1.hits > 0);

  const Preset other = load("golden_shared.json", 0.2);
  const RenderResult cached = renderWith(other, in, &cache);
  const auto s2 = cache.stats();
  REQUIRE(s2.misses == s1.misses);  // no reload
  REQUIRE(s2.hits > s1.hits);
  REQUIRE(cached.samples == renderWith(other, in, nullptr).samples);  // and the audio is unaffected

  cache.resetStats();
  REQUIRE(cache.stats().hits == 0);
  REQUIRE(cache.stats().files == 4);
  cache.clear();
  REQUIRE(cache.stats().files == 0);
}

TEST_CASE("CaptureCache: IRs are cached per render rate and normalization", "[cache]") {
  const AudioFile in = shortDi();
  const Preset p = load("golden_shared.json");
  CaptureCache cache;
  RenderOptions o;
  o.cache = &cache;
  renderPreset(p, in, o);
  const auto s1 = cache.stats();
  o.renderRate = 44100.0;  // NAM models at 48k cannot run here, so only the IR keying is checked
  Capture ir = p.cab.ir;
  (void)cache.ir(ir, "cab.ir.file", 48000.0, true);
  REQUIRE(cache.stats().misses == s1.misses);                    // same (rate, normalize): hit
  (void)cache.ir(ir, "cab.ir.file", 44100.0, true);
  REQUIRE(cache.stats().misses == s1.misses + 1);                // new rate: miss
  (void)cache.ir(ir, "cab.ir.file", 48000.0, false);
  REQUIRE(cache.stats().misses == s1.misses + 2);                // new normalize: miss
}

TEST_CASE("CaptureCache: a changed file is reloaded, an unchanged one is not", "[cache]") {
  TempDir t;
  fs::copy_file(kFixtures / "nam" / "linear_05_025.nam", t.dir / "m.nam");
  Capture c;
  c.file = "m.nam";
  c.resolvedPath = t.dir / "m.nam";
  CaptureCache cache;
  const auto a = cache.namModel(c, "x.file");
  REQUIRE(cache.namModel(c, "x.file") == a);  // same object: hit
  REQUIRE(cache.stats().misses == 1);
  REQUIRE(cache.stats().hits == 1);

  fs::copy_file(kFixtures / "nam" / "linear_identity.nam", t.dir / "m.nam", fs::copy_options::overwrite_existing);
  const auto b = cache.namModel(c, "x.file");
  REQUIRE(b != a);  // content changed (sha differs) -> reloaded
  REQUIRE(cache.stats().misses == 2);

  // A preset hash that matches the new content is accepted; the old one is rejected.
  c.sha256 = sha256File(t.dir / "m.nam");
  REQUIRE(cache.namModel(c, "x.file") == b);
  c.sha256 = sha256File(kFixtures / "nam" / "linear_05_025.nam");
  REQUIRE_THROWS_WITH(cache.namModel(c, "x.file"), ContainsSubstring("sha256 mismatch"));
}

TEST_CASE("CaptureCache: errors carry the JSON path of the capture", "[cache][errors]") {
  const AudioFile in = shortDi();
  CaptureCache cache;
  for (CaptureCache* c : {static_cast<CaptureCache*>(nullptr), &cache}) {
    {  // missing model
      json j = readJson(kPresetDir / "golden_shared.json");
      j["paths"]["b"]["blocks"][1]["model"]["file"] = "../nam/absent.nam";
      try {
        renderWith(parsePreset(j, kPresetDir), in, c);
        FAIL("expected an error");
      } catch (const RenderError& e) {
        REQUIRE(e.kind() == RenderErrorKind::Io);
        REQUIRE(e.jsonPath() == "paths.b.blocks[1].model.file");
      }
    }
    {  // wrong IR hash
      json j = readJson(kPresetDir / "golden_shared.json");
      j["cab"]["ir"]["sha256"] = std::string(64, '0');
      try {
        renderWith(parsePreset(j, kPresetDir), in, c);
        FAIL("expected an error");
      } catch (const RenderError& e) {
        REQUIRE(e.kind() == RenderErrorKind::Io);
        REQUIRE(e.jsonPath() == "cab.ir.file");
        REQUIRE_THAT(e.what(), ContainsSubstring("sha256 mismatch"));
      }
    }
    {  // semantic preset error found at load time (EQ band above Nyquist at the render rate)
      json j = readJson(kPresetDir / "golden_shared.json");
      j["postEq"][0]["freq"] = 40000.0;
      try {
        renderWith(parsePreset(j, kPresetDir), in, c);
        FAIL("expected an error");
      } catch (const RenderError& e) {
        REQUIRE(e.kind() == RenderErrorKind::Preset);
        REQUIRE(e.jsonPath() == "postEq");
      }
    }
  }
}

TEST_CASE("CaptureCache: concurrent renders share one cache and match the serial result", "[cache][threads]") {
  const AudioFile in = shortDi();
  const Preset shared = load("golden_shared.json");
  const Preset perPath = load("golden_perpath.json");
  const Preset sharedB = load("golden_shared.json", 0.3);
  const Preset* presets[4] = {&shared, &perPath, &sharedB, &shared};
  std::vector<float> expect[4];
  for (int i = 0; i < 4; ++i) expect[i] = renderWith(*presets[i], in, nullptr).samples;

  CaptureCache cache;  // cold: the four threads race to load the same files
  std::vector<float> got[4];
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i)
    threads.emplace_back([&, i] { got[i] = renderWith(*presets[i], in, &cache).samples; });
  for (auto& t : threads) t.join();
  for (int i = 0; i < 4; ++i) REQUIRE(got[i] == expect[i]);

  // golden_shared + golden_perpath use: wavenet, lstm, linear_identity, ir_a (+ ir_b for per-path),
  // each loaded exactly once however the threads interleaved.
  const auto s = cache.stats();
  REQUIRE(s.files == 5);
  REQUIRE(s.misses == 5);
}
