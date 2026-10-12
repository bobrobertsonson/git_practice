#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/chain.h"
#include "sawblade/ir_mix.h"
#include "sawblade/render.h"
#include "sawblade/wav_io.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;
const fs::path kPresets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

struct TempDir {
  fs::path dir = fs::temp_directory_path() / ("sawblade_irmix_tests_" + std::to_string(std::random_device{}()));
  TempDir() { fs::create_directories(dir); }
  ~TempDir() { std::error_code ec; fs::remove_all(dir, ec); }
  std::string write(const std::string& name, const std::vector<float>& ir) const {
    const auto p = dir / name;
    writeWavFloat32(p, kFs, ir);
    return p.string();
  }
};

std::vector<float> decayingNoise(std::size_t n, unsigned seed) {
  auto v = noise(n, seed, 1.0f);
  for (std::size_t i = 0; i < n; ++i) v[i] *= static_cast<float>(std::exp(-6.0 * static_cast<double>(i) / static_cast<double>(n)));
  return v;
}

json nam(const std::string& id, const std::string& file) {
  return {{"id", id}, {"type", "nam"}, {"model", {{"file", "../nam/" + file}}}};
}

json basePreset() {
  return {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "t"},
          {"paths", {{"a", {{"blocks", json::array({nam("a1", "linear_05_025.nam")})}}},
                     {"b", {{"blocks", json::array({nam("b1", "linear_neg1_at_23.nam")})}}}}},
          {"align", {{"mode", "off"}}}, {"blend", 0.4},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}}};
}

json withCab(json p, const json& cab) {
  p["cab"] = cab;
  return p;
}

json sharedCab(const std::string& f) { return {{"mode", "shared"}, {"ir", {{"file", f}}}}; }
json mixCab(const std::string& a, const std::string& b, double mix) {
  return {{"mode", "irMix"}, {"irA", {{"file", a}}}, {"irB", {{"file", b}}}, {"mix", mix}};
}

std::unique_ptr<Chain> build(const json& j, int maxBlock = 256) {
  const Preset p = parsePreset(j, kPresets);
  auto c = std::make_unique<Chain>(p, loadResources(p, kFs));
  c->prepare({kFs, maxBlock});
  return c;
}

std::vector<float> render(Chain& c, const std::vector<float>& x, int block) {
  std::vector<float> y(x.size());
  for (std::size_t pos = 0; pos < x.size(); pos += static_cast<std::size_t>(block)) {
    const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos));
    c.process(x.data() + pos, y.data() + pos, n);
  }
  return y;
}

void requireErrorAt(const json& j, const std::string& path) {
  try {
    (void)parsePreset(j, "/base");
    FAIL("expected PresetError at " << path);
  } catch (const PresetError& e) {
    REQUIRE(e.jsonPath() == path);
  }
}

}  // namespace

TEST_CASE("mixIrs: weights, zero padding, no renormalisation, clamp", "[irmix]") {
  const std::vector<float> a{1.0f, 2.0f, 3.0f}, b{10.0f, 20.0f};
  const auto h = mixIrs(a, b, 0.25);
  REQUIRE(h.size() == 3);
  REQUIRE(h[0] == 0.75f * 1.0f + 0.25f * 10.0f);
  REQUIRE(h[1] == 0.75f * 2.0f + 0.25f * 20.0f);
  REQUIRE(h[2] == 0.75f * 3.0f);
  REQUIRE(mixIrs(a, b, 0.0)[1] == 2.0f);
  REQUIRE(mixIrs(a, b, 1.0)[2] == 0.0f);
  REQUIRE(mixIrs(a, b, -3.0) == mixIrs(a, b, 0.0));
  REQUIRE(mixIrs(a, b, 3.0) == mixIrs(a, b, 1.0));
}

TEST_CASE("irMix preset: round trip, defaults and strict keys", "[irmix][preset]") {
  json j = withCab(basePreset(), mixCab("a.wav", "b.wav", 0.3));
  const Preset p = parsePreset(j, "/base");
  REQUIRE(p.cab.mode == CabMode::IrMix);
  REQUIRE(p.cab.mix == 0.3);
  REQUIRE(p.cab.irA.file == "a.wav");
  REQUIRE(p.cab.irB.file == "b.wav");
  const json out = toJson(p);
  REQUIRE(out["cab"]["mode"] == "irMix");
  REQUIRE(out["cab"]["mix"] == 0.3);
  REQUIRE_FALSE(out["cab"].contains("ir"));
  const Preset p2 = parsePreset(out, "/base");
  REQUIRE(p2 == p);
  REQUIRE(toJson(p2) == out);

  SECTION("mix defaults to 0.5") {
    j["cab"].erase("mix");
    REQUIRE(parsePreset(j, "/base").cab.mix == 0.5);
  }
  SECTION("ir is rejected in irMix mode") {
    j["cab"]["ir"] = {{"file", "x.wav"}};
    requireErrorAt(j, "cab.ir");
  }
  SECTION("mix is rejected in shared and perPath modes") {
    json s = basePreset();
    s["cab"]["mix"] = 0.5;
    requireErrorAt(s, "cab.mix");
    json pp = withCab(basePreset(), {{"mode", "perPath"}, {"irA", {{"file", "a.wav"}}}, {"irB", {{"file", "b.wav"}}}, {"mix", 0.5}});
    requireErrorAt(pp, "cab.mix");
  }
  SECTION("irA and irB are required") {
    json m = j;
    m["cab"].erase("irB");
    requireErrorAt(m, "cab.irB");
    m = j;
    m["cab"].erase("irA");
    requireErrorAt(m, "cab.irA");
  }
  SECTION("mix out of range") {
    j["cab"]["mix"] = 1.5;
    requireErrorAt(j, "cab.mix");
    j["cab"]["mix"] = -0.1;
    requireErrorAt(j, "cab.mix");
  }
  SECTION("other modes round trip unchanged") {
    const Preset s = parsePreset(basePreset(), "/base");
    REQUIRE(parsePreset(toJson(s), "/base") == s);
    REQUIRE_FALSE(toJson(s)["cab"].contains("mix"));
  }
}

TEST_CASE("irMix render equals the mix of the two shared renders", "[irmix][chain]") {
  TempDir tmp;
  const std::string fa = tmp.write("a.wav", decayingNoise(3000, 5));
  const std::string fb = tmp.write("b.wav", decayingNoise(1100, 9));
  const auto x = noise(20000, 19, 0.3f);
  const auto ya = render(*build(withCab(basePreset(), sharedCab(fa))), x, 256);
  const auto yb = render(*build(withCab(basePreset(), sharedCab(fb))), x, 256);
  auto chain = build(withCab(basePreset(), mixCab(fa, fb, 0.3)));
  REQUIRE(chain->info().liveCompatible);
  REQUIRE(chain->info().exportExactness.noCab);
  REQUIRE(chain->info().cabMode == "irMix");
  const auto ym = render(*chain, x, 256);
  double worst = 0.0, peak = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    worst = std::max(worst, std::fabs(static_cast<double>(ym[i]) - (0.7 * ya[i] + 0.3 * yb[i])));
    peak = std::max(peak, static_cast<double>(std::fabs(ym[i])));
  }
  REQUIRE(peak > 0.05);  // not a vacuous comparison
  REQUIRE(worst <= 1e-6);
  // Latency matches shared.
  REQUIRE(chain->latencySamples() == build(withCab(basePreset(), sharedCab(fa)))->latencySamples());
}

TEST_CASE("irMix with mix = 0 is bit-identical to shared irA", "[irmix][chain]") {
  TempDir tmp;
  for (const auto& [la, lb] : {std::pair<std::size_t, std::size_t>{3000, 1100}, {900, 4000}}) {
    const std::string fa = tmp.write("a.wav", decayingNoise(la, 5));
    const std::string fb = tmp.write("b.wav", decayingNoise(lb, 9));
    const auto x = noise(20000, 21, 0.3f);
    const auto ys = render(*build(withCab(basePreset(), sharedCab(fa))), x, 256);
    const auto ym = render(*build(withCab(basePreset(), mixCab(fa, fb, 0.0))), x, 256);
    REQUIRE(ys == ym);
  }
}

TEST_CASE("irMix is block-size invariant", "[irmix][chain]") {
  TempDir tmp;
  const std::string fa = tmp.write("a.wav", decayingNoise(3000, 5));
  const std::string fb = tmp.write("b.wav", decayingNoise(1100, 9));
  const json j = withCab(basePreset(), mixCab(fa, fb, 0.3));
  const auto x = noise(20000, 23, 0.3f);
  const auto y1 = render(*build(j, 512), x, 512);
  for (int block : {1, 7, 64, 333}) {
    const auto y2 = render(*build(j, 512), x, block);
    REQUIRE(y1 == y2);
  }
}

TEST_CASE("irMix: process() does not allocate", "[irmix][alloc]") {
  TempDir tmp;
  const std::string fa = tmp.write("a.wav", decayingNoise(3000, 5));
  const std::string fb = tmp.write("b.wav", decayingNoise(1100, 9));
  auto chain = build(withCab(basePreset(), mixCab(fa, fb, 0.3)), 512);
  auto x = noise(4000, 3, 0.3f);
  std::vector<float> y(1000);
  const int sizes[] = {512, 1, 64, 480, 7, 128, 300, 33, 512, 256, 100, 2, 1000};
  AllocGuard g;
  std::size_t pos = 0;
  for (int n : sizes) {
    chain->process(x.data() + pos, y.data(), n);
    pos += static_cast<std::size_t>(n);
    if (pos + 1000 > x.size()) pos = 0;
  }
  REQUIRE(g.count() == 0);
  for (float v : y) REQUIRE(std::isfinite(v));
}

TEST_CASE("irMix: render report names the mode and both captures", "[irmix][render]") {
  TempDir tmp;
  json cab = mixCab(tmp.write("a.wav", decayingNoise(800, 5)), tmp.write("b.wav", decayingNoise(600, 9)), 0.5);
  cab["irA"]["source"] = {{"provider", "tone3000"}, {"id", "1"}, {"modelId", "10"}};
  cab["irB"]["source"] = {{"provider", "tone3000"}, {"id", "1"}, {"modelId", "11"}};
  const RenderResult r = renderPreset(parsePreset(withCab(basePreset(), cab), kPresets), AudioFile{48000.0, 1, noise(3000, 6, 0.3f)});
  const json rep = reportJson(r);
  REQUIRE(rep["cabMode"] == "irMix");
  REQUIRE(rep["liveCompatible"] == true);
  REQUIRE(rep["exportExactness"]["noCab"] == true);
  std::vector<std::string> where;
  for (const auto& c : rep["captures"]) where.push_back(c["where"]);
  REQUIRE(std::count(where.begin(), where.end(), "cab.irA") == 1);
  REQUIRE(std::count(where.begin(), where.end(), "cab.irB") == 1);
}
