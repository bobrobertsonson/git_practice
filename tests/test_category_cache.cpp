// Phase 9b core: preset `category` (UI metadata) and the TONE3000 capture-cache fallback.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdlib>
#include <filesystem>
#include <random>

#include "sawblade/capture_cache.h"
#include "sawblade/chain.h"
#include "sawblade/render.h"
#include "sawblade/sha256.h"
#include "test_util.h"

using namespace sawblade;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
const fs::path kFx = fs::path(SAWBLADE_FIXTURES_DIR);

struct TempDir {
  fs::path dir = fs::temp_directory_path() / ("sawblade_cat_tests_" + std::to_string(std::random_device{}()));
  TempDir() { fs::create_directories(dir); }
  ~TempDir() { std::error_code ec; fs::remove_all(dir, ec); }
};

struct EnvGuard {
  explicit EnvGuard(const fs::path& cache) { ::setenv("SAWBLADE_CACHE_DIR", cache.string().c_str(), 1); }
  ~EnvGuard() { ::unsetenv("SAWBLADE_CACHE_DIR"); }
};

json presetJson(const json& nam, const json& ir) {
  return {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "t"},
          {"paths", {{"a", {{"blocks", json::array({{{"id", "a1"}, {"type", "nam"}, {"model", nam}}})}}},
                     {"b", {{"blocks", json::array({{{"id", "b1"}, {"type", "nam"}, {"model", nam}}})}}}}},
          {"align", {{"mode", "off"}}}, {"cab", {{"mode", "shared"}, {"ir", ir}}}};
}
json src(const char* id, const char* model) { return {{"provider", "tone3000"}, {"id", id}, {"modelId", model}}; }
}  // namespace

TEST_CASE("category: optional, round-trips, written only when non-empty", "[preset][category]") {
  json j = presetJson({{"file", "a.nam"}}, {{"file", "b.wav"}});
  Preset p = parsePreset(j, "/base");
  CHECK(p.category.empty());
  CHECK_FALSE(toJson(p).contains("category"));
  j["category"] = "Grind";
  p = parsePreset(j, "/base");
  CHECK(p.category == "Grind");
  CHECK(toJson(p)["category"] == "Grind");
  CHECK(parsePreset(toJson(p), "/base") == p);
  j["category"] = 5;
  CHECK_THROWS_AS(parsePreset(j, "/base"), PresetError);
}

TEST_CASE("category is ignored by the render", "[preset][category]") {
  json j = presetJson({{"file", (kFx / "nam/linear_identity.nam").string()}}, {{"file", (kFx / "ir/impulse.wav").string()}});
  const AudioFile in{48000.0, 1, sawblade::test::noise(4000, 3, 0.3f)};
  const auto a = renderPreset(parsePreset(j, "/"), in).samples;
  j["category"] = "Thrash";
  const auto b = renderPreset(parsePreset(j, "/"), in).samples;
  CHECK(a == b);
}

TEST_CASE("every factory preset parses and has a category", "[preset][category]") {
  const fs::path root(SAWBLADE_PRESETS_DIR);
  int n = 0;
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (e.path().extension() != ".json" || e.path().filename().string().find(".resolved.") != std::string::npos) continue;
    INFO(e.path());
    const Preset p = loadPresetFile(e.path());
    CHECK_FALSE(p.category.empty());
    ++n;
  }
  CHECK(n >= 11);
}

TEST_CASE("capture cache fallback: a missing local file is found in the TONE3000 cache", "[capture][cache]") {
  TempDir tmp;
  EnvGuard env(tmp.dir);
  REQUIRE(captureCacheRoot() == tmp.dir);
  fs::create_directories(tmp.dir / "58569");
  fs::copy_file(kFx / "nam/linear_identity.nam", tmp.dir / "58569" / "496942.nam");
  fs::copy_file(kFx / "ir/ir_a.wav", tmp.dir / "58569" / "7.wav");
  const json nam = {{"file", "captures/58569_496942.nam"}, {"source", src("58569", "496942")}};
  const json ir = {{"file", "captures/ir.wav"}, {"source", src("58569", "7")}};
  const Preset p = parsePreset(presetJson(nam, ir), "/nowhere");
  CHECK(locateCapture(p.cab.ir) == tmp.dir / "58569" / "7.wav");
  CHECK_NOTHROW(verifyCapture(p.cab.ir, "cab.ir.file"));
  auto res = loadResources(p, 48000.0);  // plain loaders
  CHECK(res.cabShared != nullptr);
  CaptureCache cache;
  CHECK_NOTHROW(loadResources(p, 48000.0, &cache));  // the shared cache and the NAM probe too
  CHECK_NOTHROW(probeNamRates(p, &cache));
  CHECK_NOTHROW(probeNamRates(p, nullptr));

  SECTION("a local file wins over the cache") {
    json local = nam;
    local["file"] = (kFx / "nam/linear_05_025.nam").string();
    const Preset q = parsePreset(presetJson(local, ir), "/");
    CHECK(locateCapture(static_cast<const NamBlockParams&>(*q.a.blocks[0].params).model) == kFx / "nam/linear_05_025.nam");
  }
  SECTION("a sha256 mismatch is rejected") {
    json bad = nam;
    bad["sha256"] = std::string(64, '0');
    const Preset q = parsePreset(presetJson(bad, ir), "/nowhere");
    CHECK_THROWS_WITH(verifyCapture(static_cast<const NamBlockParams&>(*q.a.blocks[0].params).model, "x.file"), ContainsSubstring("sha256 mismatch"));
    CHECK_THROWS_WITH(loadResources(q, 48000.0), ContainsSubstring("sha256 mismatch"));
    json good = nam;
    good["sha256"] = sha256File(tmp.dir / "58569" / "496942.nam");
    CHECK_NOTHROW(loadResources(parsePreset(presetJson(good, ir), "/nowhere"), 48000.0));
  }
  SECTION("not cached either: the path and the resolve hint") {
    json other = nam;
    other["source"] = src("1", "2");
    const Preset q = parsePreset(presetJson(other, ir), "/nowhere");
    try {
      loadResources(q, 48000.0);
      FAIL("expected CaptureError");
    } catch (const CaptureError& e) {
      CHECK(e.jsonPath() == "paths.a.blocks[0].model.file");
      CHECK_THAT(std::string(e.what()), ContainsSubstring("paths.a.blocks[0].model.file"));
      CHECK_THAT(std::string(e.what()), ContainsSubstring("file not found"));
      CHECK_THAT(std::string(e.what()), ContainsSubstring("not in the capture cache either; run: sawblade-t3k resolve <preset file>"));
    }
  }
  SECTION("a capture without TONE3000 ids has no fallback and no hint") {
    const Preset q = parsePreset(presetJson({{"file", "gone.nam"}}, ir), "/nowhere");
    try {
      loadResources(q, 48000.0);
      FAIL("expected CaptureError");
    } catch (const CaptureError& e) {
      CHECK_THAT(std::string(e.what()), !ContainsSubstring("capture cache"));
    }
  }
}

TEST_CASE("cache root defaults to ~/.cache/sawblade/captures", "[capture][cache]") {
  ::unsetenv("SAWBLADE_CACHE_DIR");
  const char* home = std::getenv("HOME");
  REQUIRE(home != nullptr);
  CHECK(captureCacheRoot() == fs::path(home) / ".cache" / "sawblade" / "captures");
}

TEST_CASE("capture cache: ids that are not plain tokens never build a path", "[capture][cache]") {
  TempDir tmp;
  fs::create_directories(tmp.dir / "cache");
  EnvGuard env(tmp.dir / "cache");
  // a file outside the cache root that a traversal id would reach
  fs::copy_file(kFx / "nam/linear_identity.nam", tmp.dir / "secret.nam");
  const json ir = {{"file", (kFx / "ir/impulse.wav").string()}};
  for (const char* bad : {"../secret", "..", "a/b", "a\\b", "x..y", "", "/abs", "a b"}) {
    INFO("id: " << bad);
    json nam = {{"file", "gone.nam"}, {"source", src(bad, "secret")}};
    const Preset p = parsePreset(presetJson(nam, ir), "/nowhere");
    const auto& model = static_cast<const NamBlockParams&>(*p.a.blocks[0].params).model;
    CHECK(locateCapture(model) == model.resolvedPath);  // never a cache path
  }
  json nam = {{"file", "gone.nam"}, {"source", src("../", "secret")}};
  const Preset p = parsePreset(presetJson(nam, ir), "/nowhere");
  try {
    loadResources(p, 48000.0);
    FAIL("expected CaptureError");
  } catch (const CaptureError& e) {
    CHECK_THAT(std::string(e.what()), ContainsSubstring("file not found"));
    CHECK_THAT(std::string(e.what()), ContainsSubstring("plain letters, digits"));
  }
  json nam2 = {{"file", "gone.nam"}, {"source", src("1", "../../secret")}};
  CHECK_THROWS_AS(loadResources(parsePreset(presetJson(nam2, ir), "/nowhere"), 48000.0), CaptureError);
  CaptureCache cache;
  CHECK_THROWS_AS(loadResources(parsePreset(presetJson(nam2, ir), "/nowhere"), 48000.0, &cache), CaptureError);
}
