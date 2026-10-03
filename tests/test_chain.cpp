#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "alloc_guard.h"
#include "latency_stub.h"
#include "sawblade/chain.h"
#include "sawblade/nam_block.h"
#include "sawblade/sha256.h"
#include "sawblade/wav_io.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;
const fs::path kPresets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

// ---- helpers --------------------------------------------------------------------------------
json namBlock(const std::string& id, const std::string& file) {
  return {{"id", id}, {"type", "nam"}, {"model", {{"file", "../nam/" + file}}}};
}

// Two-path preset, auto align, 50/50, shared impulse cab.
json mk(const std::string& a, const std::string& b) {
  return {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "t"},
          {"paths", {{"a", {{"blocks", json::array({namBlock("a1", a)})}}}, {"b", {{"blocks", json::array({namBlock("b1", b)})}}}}},
          {"align", {{"mode", "auto"}}},
          {"blend", 0.5},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}}};
}

struct TempDir {
  fs::path dir = fs::temp_directory_path() / ("sawblade_chain_tests_" + std::to_string(std::random_device{}()));
  TempDir() { fs::create_directories(dir); }
  ~TempDir() { std::error_code ec; fs::remove_all(dir, ec); }
  std::string write(const std::string& name, const std::vector<float>& ir) const {
    const auto p = dir / name;
    writeWavFloat32(p, kFs, ir);
    return p.string();
  }
};

std::vector<float> impulseAt(std::size_t k, std::size_t len = 256) {
  std::vector<float> v(len, 0.0f);
  v[k] = 1.0f;
  return v;
}

std::vector<float> decayingNoise(std::size_t n, unsigned seed) {
  auto v = noise(n, seed, 1.0f);
  for (std::size_t i = 0; i < n; ++i) v[i] *= static_cast<float>(std::exp(-6.0 * static_cast<double>(i) / static_cast<double>(n)));
  return v;
}

std::unique_ptr<Chain> build(const json& j, int maxBlock = 256, const fs::path& base = kPresets) {
  const Preset p = parsePreset(j, base);
  auto res = loadResources(p, kFs);
  auto c = std::make_unique<Chain>(p, std::move(res));
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

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
  return m;
}

// out[n] = x[n - d] (zeros before the start)
std::vector<float> delayed(const std::vector<float>& x, int d) {
  std::vector<float> y(x.size(), 0.0f);
  for (std::size_t i = static_cast<std::size_t>(d); i < x.size(); ++i) y[i] = x[i - static_cast<std::size_t>(d)];
  return y;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
TEST_CASE("Alignment: B lags A by 23 and is inverted", "[chain][align]") {
  auto chain = build(mk("linear_identity.nam", "linear_neg1_at_23.nam"));
  const ChainInfo info = chain->info();
  REQUIRE(info.align.delaySamplesB == -23);  // A is delayed by 23 to meet B
  REQUIRE(info.align.invertB);
  REQUIRE(info.align.peakCorrelation > 0.95);
  REQUIRE(info.alignDelay[0] == 23);
  REQUIRE(info.alignDelay[1] == 0);
  REQUIRE(info.alignMode == AlignMode::Auto);
  REQUIRE(info.latencySamples == 0);  // alignment delay is not processing latency
  REQUIRE(chain->latencySamples() == 0);

  const auto x = noise(20000, 11, 0.3f);
  const auto y = render(*chain, x, 256);
  // Constructive sum: 0.5 * A(delayed 23) + 0.5 * (-B) = A delayed by 23.
  REQUIRE(maxAbsDiff(y, delayed(x, 23)) <= 1e-5);
}

TEST_CASE("Alignment: identical paths resolve to (0, false)", "[chain][align]") {
  auto chain = build(mk("linear_05_025.nam", "linear_05_025.nam"));
  const auto info = chain->info();
  REQUIRE(info.align.delaySamplesB == 0);
  REQUIRE_FALSE(info.align.invertB);
  REQUIRE(info.align.peakCorrelation > 0.999);
  REQUIRE(info.latencySamples == 0);
  const auto x = noise(5000, 12, 0.3f);
  const auto y = render(*chain, x, 100);
  for (std::size_t i = 1; i < x.size(); ++i) REQUIRE(std::fabs(y[i] - (0.5f * x[i] + 0.25f * x[i - 1])) <= 1e-6);
}

TEST_CASE("Alignment: lag beyond maxLagMs is not found; within a wider window it is", "[chain][align]") {
  json j = mk("linear_identity.nam", "linear_delay_300.nam");
  j["align"] = {{"mode", "auto"}, {"maxLagMs", 2.0}};  // 96 samples at 48 kHz
  {
    auto chain = build(j);
    const auto r = chain->info().align;
    REQUIRE(std::abs(r.delaySamplesB) <= 96);
    REQUIRE(r.delaySamplesB != -300);
  }
  j["align"] = {{"mode", "auto"}, {"maxLagMs", 10.0}};  // 480 samples
  {
    auto chain = build(j);
    const auto r = chain->info().align;
    REQUIRE(r.delaySamplesB == -300);
    REQUIRE_FALSE(r.invertB);
  }
  j["align"] = {{"mode", "auto"}, {"maxLagMs", 0.0}};  // only lag 0 is searched
  REQUIRE(build(j)->info().align.delaySamplesB == 0);
}

TEST_CASE("Alignment: perPath mode measures through each path's own IR", "[chain][align]") {
  TempDir tmp;
  json j = mk("linear_identity.nam", "linear_identity.nam");
  j["cab"] = {{"mode", "perPath"}, {"irA", {{"file", tmp.write("irA.wav", impulseAt(0))}}},
              {"irB", {{"file", tmp.write("irB.wav", impulseAt(10))}}}};
  auto chain = build(j);
  REQUIRE(chain->info().align.delaySamplesB == -10);  // B's IR lags 10 samples, so A is delayed
  REQUIRE_FALSE(chain->info().align.invertB);
  REQUIRE_FALSE(chain->info().liveCompatible);
  const auto x = noise(8000, 13, 0.3f);
  REQUIRE(maxAbsDiff(render(*chain, x, 64), delayed(x, 10)) <= 1e-5);

  // Shared mode with the same IR on both paths sees no offset.
  j["cab"] = {{"mode", "shared"}, {"ir", {{"file", tmp.write("irB2.wav", impulseAt(10))}}}};
  REQUIRE(build(j)->info().align.delaySamplesB == 0);
}

TEST_CASE("Alignment: auto, manual and off modes", "[chain][align]") {
  const auto x = noise(12000, 14, 0.3f);
  json j = mk("linear_identity.nam", "linear_neg1_at_23.nam");
  auto autoChain = build(j);
  const auto resolved = autoChain->info().align;

  // The resolved values replay identically as `manual` (and the probe left no state behind).
  json m = j;
  m["align"] = {{"mode", "manual"}, {"delaySamplesB", resolved.delaySamplesB}, {"invertB", resolved.invertB}};
  auto manualChain = build(m);
  REQUIRE(manualChain->info().alignMode == AlignMode::Manual);
  REQUIRE(manualChain->info().align.delaySamplesB == -23);
  REQUIRE(maxAbsDiff(render(*autoChain, x, 200), render(*manualChain, x, 200)) == 0.0);

  // Manual positive delay delays B.
  m["align"] = {{"mode", "manual"}, {"delaySamplesB", 5}, {"invertB", false}};
  auto delayB = build(m);
  REQUIRE(delayB->info().alignDelay[1] == 5);
  REQUIRE(delayB->info().alignDelay[0] == 0);
  REQUIRE(delayB->latencySamples() == 0);
  {
    // out = 0.5 * x + 0.5 * (-x delayed by 23+5)
    const auto y = render(*delayB, x, 128);
    const auto d = delayed(x, 28);
    for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(std::fabs(y[i] - (0.5f * x[i] - 0.5f * d[i])) <= 1e-6);
  }

  // Off: nothing beyond latency compensation.
  m["align"] = {{"mode", "off"}};
  auto off = build(m);
  REQUIRE(off->info().align.delaySamplesB == 0);
  REQUIRE(off->latencySamples() == 0);

  // resolveAlignment() is repeatable and does not change the audio.
  autoChain->reset();
  const auto y1 = render(*autoChain, x, 77);
  autoChain->reset();
  const auto r2 = autoChain->resolveAlignment();
  REQUIRE(r2.delaySamplesB == -23);
  REQUIRE(r2.invertB);
  REQUIRE(maxAbsDiff(render(*autoChain, x, 77), y1) == 0.0);
}

TEST_CASE("Alignment: a silent or disabled path skips alignment with a warning", "[chain][align]") {
  json j = mk("linear_identity.nam", "linear_neg1_at_23.nam");
  j["paths"]["b"]["enabled"] = false;
  auto chain = build(j);
  REQUIRE(chain->info().align.delaySamplesB == 0);
  bool found = false;
  for (const auto& w : chain->info().warnings) found = found || w.find("alignment skipped") != std::string::npos;
  REQUIRE(found);
}

TEST_CASE("Latency compensation re-aligns paths (test-only latency stub)", "[chain][latency]") {
  registerLatencyStub();
  const auto x = noise(10000, 15, 0.3f);
  json j = mk("linear_identity.nam", "linear_identity.nam");
  j["paths"]["a"]["blocks"] = json::array({{{"id", "s1"}, {"type", "test.latency"}, {"latency", 100}}});
  j["align"] = {{"mode", "off"}};
  auto chain = build(j);
  const auto info = chain->info();
  REQUIRE(info.pathLatency[0] == 100);
  REQUIRE(info.pathLatency[1] == 0);
  REQUIRE(info.compensationDelay[0] == 0);
  REQUIRE(info.compensationDelay[1] == 100);  // the shorter path (B) is delayed
  REQUIRE(info.latencySamples == 100);
  REQUIRE(chain->latencySamples() == 100);
  // Both paths now carry x delayed by 100, so the blend is x delayed by 100 (no comb filtering).
  REQUIRE(maxAbsDiff(render(*chain, x, 256), delayed(x, 100)) <= 1e-6);

  // Same with the stub on B.
  json k = j;
  k["paths"]["a"]["blocks"] = json::array({namBlock("a1", "linear_identity.nam")});
  k["paths"]["b"]["blocks"] = json::array({{{"id", "s1"}, {"type", "test.latency"}, {"latency", 37}}});
  auto c2 = build(k);
  REQUIRE(c2->info().compensationDelay[0] == 37);
  REQUIRE(c2->latencySamples() == 37);
  REQUIRE(maxAbsDiff(render(*c2, x, 50), delayed(x, 37)) <= 1e-6);

  // Auto-align measures after compensation, so a pure latency difference is not an offset.
  j["align"] = {{"mode", "auto"}};
  auto c3 = build(j);
  REQUIRE(c3->info().align.delaySamplesB == 0);
  REQUIRE(c3->latencySamples() == 100);
  REQUIRE(maxAbsDiff(render(*c3, x, 256), delayed(x, 100)) <= 1e-6);

  // Latency and alignment are separate: stub 100 on A plus B lagging 23 -> A (delayed 100, B comp 100).
  json l = mk("linear_identity.nam", "linear_neg1_at_23.nam");
  l["paths"]["a"]["blocks"] = json::array({{{"id", "s1"}, {"type", "test.latency"}, {"latency", 100}}});
  auto c4 = build(l);
  REQUIRE(c4->info().align.delaySamplesB == -23);
  REQUIRE(c4->latencySamples() == 100);  // stub latency only; the 23 is alignDelay
  REQUIRE(c4->info().alignDelay[0] == 23);
  REQUIRE(maxAbsDiff(render(*c4, x, 256), delayed(x, 123)) <= 1e-5);

  // A bypassed block contributes no latency and is not processed.
  json b = j;
  b["paths"]["a"]["blocks"][0]["bypass"] = true;
  b["align"] = {{"mode", "off"}};
  auto c5 = build(b);
  REQUIRE(c5->latencySamples() == 0);
  REQUIRE(maxAbsDiff(render(*c5, x, 256), x) <= 1e-6);

  // Latency + a per-path IR in perPath mode: still consistent.
  TempDir tmp;
  json p = j;
  p["align"] = {{"mode", "off"}};
  p["cab"] = {{"mode", "perPath"}, {"irA", {{"file", tmp.write("a.wav", impulseAt(0))}}}, {"irB", {{"file", tmp.write("b.wav", impulseAt(0))}}}};
  auto c6 = build(p);
  REQUIRE(c6->latencySamples() == 100);
  REQUIRE(maxAbsDiff(render(*c6, x, 256), delayed(x, 100)) <= 1e-5);
}

TEST_CASE("Warnings: non-NAM-trainable blocks and long bus-comp release", "[chain][info]") {
  registerLatencyStub();
  auto has = [](const ChainInfo& i, const std::string& s) {
    return std::any_of(i.warnings.begin(), i.warnings.end(), [&](const std::string& w) { return w.find(s) != std::string::npos; });
  };
  json j = mk("linear_identity.nam", "linear_identity.nam");
  REQUIRE(build(j)->info().warnings.empty());

  json k = j;
  k["paths"]["a"]["blocks"].push_back({{"id", "s1"}, {"type", "test.latency"}, {"latency", 3}});
  REQUIRE(has(build(k)->info(), "block 's1'"));
  REQUIRE(has(build(k)->info(), "not NAM-trainable"));
  k["paths"]["a"]["blocks"][1]["bypass"] = true;  // bypassed: not part of the export
  REQUIRE_FALSE(has(build(k)->info(), "not NAM-trainable"));

  json c = j;
  c["busComp"] = {{"enabled", true}, {"releaseMs", 200.0}};
  REQUIRE(has(build(c)->info(), "busComp"));
  REQUIRE(has(build(c)->info(), "not NAM-trainable"));
  c["busComp"] = {{"enabled", true}, {"releaseMs", 150.0}};
  REQUIRE_FALSE(has(build(c)->info(), "not NAM-trainable"));
  c["busComp"] = {{"enabled", false}, {"releaseMs", 400.0}};
  REQUIRE_FALSE(has(build(c)->info(), "not NAM-trainable"));
}

TEST_CASE("Blend 0 is exactly path A, blend 1 exactly path B", "[chain][blend]") {
  const auto x = noise(7000, 16, 0.4f);
  // Hand-rendered single-path references through the same components, same block size.
  auto single = [&](const std::string& file) {
    auto b = NamBlock::load(fs::path(SAWBLADE_FIXTURES_DIR) / "nam" / file, {});
    b->prepare({kFs, 256});
    std::vector<float> y = x;
    for (std::size_t pos = 0; pos < y.size(); pos += 256)
      b->process(y.data() + pos, static_cast<int>(std::min<std::size_t>(256, y.size() - pos)));
    return y;
  };
  json j = mk("linear_05_025.nam", "linear_neg1_at_23.nam");
  j["align"] = {{"mode", "off"}};
  j["cab"]["enabled"] = false;
  j["blend"] = 0.0;
  {
    auto c = build(j);
    REQUIRE(maxAbsDiff(render(*c, x, 256), single("linear_05_025.nam")) == 0.0);
  }
  j["blend"] = 1.0;
  {
    auto c = build(j);
    REQUIRE(maxAbsDiff(render(*c, x, 256), single("linear_neg1_at_23.nam")) == 0.0);
  }
  // Linear in between.
  j["blend"] = 0.25;
  {
    auto c = build(j);
    const auto y = render(*c, x, 256), a = single("linear_05_025.nam"), b = single("linear_neg1_at_23.nam");
    for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(std::fabs(y[i] - (0.75f * a[i] + 0.25f * b[i])) <= 1e-6);
  }
}

TEST_CASE("Path flags: disabled path is silent, invert flips, level trims", "[chain][path]") {
  const auto x = noise(4000, 17, 0.3f);
  json j = mk("linear_identity.nam", "linear_identity.nam");
  j["align"] = {{"mode", "off"}};
  j["cab"]["enabled"] = false;
  {
    json d = j;
    d["paths"]["b"]["enabled"] = false;
    const auto y = render(*build(d), x, 256);
    for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(y[i] == 0.5f * x[i]);
  }
  {
    json d = j;
    d["paths"]["b"]["invert"] = true;
    d["blend"] = 1.0;
    const auto y = render(*build(d), x, 256);
    for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(y[i] == -x[i]);
  }
  {
    json d = j;
    d["paths"]["a"]["levelDb"] = -6.0;
    d["blend"] = 0.0;
    d["input"] = {{"gainDb", 6.0}};
    d["output"] = {{"gainDb", -6.0}};
    const auto y = render(*build(d), x, 256);
    const float g = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(std::fabs(y[i] - x[i] * g) <= 1e-6);
  }
}

TEST_CASE("Blocks: bypass, and an eq block mid-chain equals the same bands in path eq", "[chain][registry]") {
  const auto x = noise(9000, 18, 0.3f);
  const json bands = json::array({{{"type", "peak"}, {"freq", 900.0}, {"gainDb", 6.0}, {"q", 1.2}},
                                  {{"type", "lowPass"}, {"freq", 6000.0}, {"q", 0.7071}}});
  json inChain = mk("linear_05_025.nam", "linear_identity.nam");
  inChain["align"] = {{"mode", "off"}};
  inChain["blend"] = 0.0;
  inChain["paths"]["a"]["blocks"] = json::array(
      {namBlock("a1", "linear_05_025.nam"), {{"id", "e1"}, {"type", "eq"}, {"bands", bands}}, namBlock("a2", "linear_identity.nam")});
  json inPath = inChain;
  inPath["paths"]["a"]["blocks"] = json::array({namBlock("a1", "linear_05_025.nam"), namBlock("a2", "linear_identity.nam")});
  inPath["paths"]["a"]["eq"] = bands;
  const auto y1 = render(*build(inChain), x, 256);
  const auto y2 = render(*build(inPath), x, 256);
  REQUIRE(maxAbsDiff(y1, y2) <= 1e-6);
  // ...and the EQ is audible.
  json none = inPath;
  none["paths"]["a"]["eq"] = json::array();
  REQUIRE(maxAbsDiff(render(*build(none), x, 256), y2) > 1e-3);

  // Bypassed NAM block is a pass-through.
  json byp = mk("linear_05_025.nam", "linear_identity.nam");
  byp["align"] = {{"mode", "off"}};
  byp["paths"]["a"]["blocks"][0]["bypass"] = true;
  byp["blend"] = 0.0;
  REQUIRE(maxAbsDiff(render(*build(byp), x, 256), x) == 0.0);

  // Invalid EQ frequency at the chain's rate is a load-time error naming the path.
  json bad = inChain;
  bad["paths"]["a"]["blocks"][1]["bands"][0]["freq"] = 40000.0;
  const Preset p = parsePreset(bad, kPresets);
  REQUIRE_THROWS_WITH(loadResources(p, kFs), ContainsSubstring("paths.a.blocks[1]"));
}

TEST_CASE("Cab: shared vs perPath with the same IR; disabled bypasses", "[chain][cab]") {
  TempDir tmp;
  const std::string ir = tmp.write("ir.wav", decayingNoise(3000, 5));
  const auto x = noise(15000, 19, 0.3f);
  json shared = mk("linear_05_025.nam", "linear_neg1_at_23.nam");
  shared["align"] = {{"mode", "off"}};
  shared["blend"] = 0.4;
  shared["cab"] = {{"mode", "shared"}, {"ir", {{"file", ir}}}};
  json per = shared;
  per["cab"] = {{"mode", "perPath"}, {"irA", {{"file", ir}}}, {"irB", {{"file", ir}}}};

  auto cs = build(shared), cp = build(per);
  REQUIRE(cs->info().liveCompatible);
  REQUIRE(cs->info().exportExactness.noCab);
  REQUIRE(cs->info().exportExactness.withCab);
  REQUIRE_FALSE(cp->info().liveCompatible);
  REQUIRE_FALSE(cp->info().exportExactness.noCab);
  REQUIRE(cp->info().exportExactness.withCab);
  const auto ys = render(*cs, x, 256), yp = render(*cp, x, 333);
  REQUIRE(maxAbsDiff(ys, yp) <= 1e-5);

  json off = shared;
  off["cab"]["enabled"] = false;
  const auto yo = render(*build(off), x, 256);
  REQUIRE(maxAbsDiff(yo, ys) > 1e-2);  // the IR matters
  json offPer = per;
  offPer["cab"]["enabled"] = false;
  REQUIRE(maxAbsDiff(render(*build(offPer), x, 256), yo) <= 1e-6);
}

TEST_CASE("Gate is keyed on the DI before the split", "[chain][gate]") {
  // Path B has +60 dB of gain, so its output is loud even for a quiet DI; a gate keyed on a
  // path output would open. Keyed on the DI it must stay closed.
  json j = mk("linear_identity.nam", "linear_identity.nam");
  j["align"] = {{"mode", "off"}};
  j["cab"]["enabled"] = false;
  j["blend"] = 1.0;
  j["paths"]["b"]["levelDb"] = 60.0;
  j["gate"] = {{"enabled", true}, {"thresholdDb", -55.0}};
  const auto quiet = noise(48000, 20, 1e-4f);  // about -80 dBFS
  const auto yq = render(*build(j), quiet, 256);
  REQUIRE(rms(yq.data(), yq.size()) < 1e-4 * 1000.0 * 1e-2);  // >= 40 dB below the ungated 1000x

  // A loud DI opens it: after the attack the output equals the ungated chain.
  const auto loud = noise(48000, 21, 0.2f);
  json ungated = j;
  ungated["gate"]["enabled"] = false;
  const auto a = render(*build(j), loud, 256), b = render(*build(ungated), loud, 256);
  double worst = 0.0;
  for (std::size_t i = 4800; i < loud.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - b[i]));
  REQUIRE(worst <= 1e-3 * 0.2 * 1000.0);  // within 0.1% of full scale
  const double ra = rms(a.data() + 4800, a.size() - 4800), rb = rms(b.data() + 4800, b.size() - 4800);
  REQUIRE(std::fabs(toDb(ra / rb)) < 0.1);
}

TEST_CASE("Chain: input/output gain, bus comp and post EQ run in the graph", "[chain][buscomp]") {
  const auto x = sine(997.0, kFs, 96000, 0.5);
  json j = mk("linear_identity.nam", "linear_identity.nam");
  j["align"] = {{"mode", "off"}};
  j["cab"]["enabled"] = false;
  j["busComp"] = {{"enabled", true}, {"thresholdDb", -20.0}, {"ratio", 4.0}, {"kneeDb", 0.0}, {"attackMs", 1.0}, {"releaseMs", 50.0}};
  auto y = render(*build(j), x, 256);
  const double g = toDb(rms(y.data() + 48000, 48000) / rms(x.data() + 48000, 48000));
  REQUIRE(g == Catch::Approx(-10.5).margin(0.2));  // analytic: -20 + 14/4 - (-6)

  j["busComp"]["makeupDb"] = 3.0;
  j["busComp"]["enabled"] = false;  // disabled: makeup must not apply either
  y = render(*build(j), x, 256);
  REQUIRE(maxAbsDiff(y, x) == 0.0);
  j["postEq"] = json::array({{{"type", "peak"}, {"freq", 997.0}, {"gainDb", 6.0}, {"q", 1.0}}});
  y = render(*build(j), x, 256);
  REQUIRE(toDb(rms(y.data() + 48000, 48000) / rms(x.data() + 48000, 48000)) == Catch::Approx(6.0).margin(0.1));
}

TEST_CASE("Chain: SHA-256 verification", "[chain][sha256]") {
  const fs::path f = fs::path(SAWBLADE_FIXTURES_DIR) / "nam" / "linear_identity.nam";
  json j = mk("linear_identity.nam", "linear_05_025.nam");
  j["paths"]["a"]["blocks"][0]["model"]["sha256"] = sha256File(f);
  REQUIRE_NOTHROW(build(j));
  j["paths"]["a"]["blocks"][0]["model"]["sha256"] = std::string(64, '0');
  const Preset p = parsePreset(j, kPresets);
  REQUIRE_THROWS_WITH(loadResources(p, kFs), ContainsSubstring("paths.a.blocks[0].model.file"));
  REQUIRE_THROWS_WITH(loadResources(p, kFs), ContainsSubstring("sha256 mismatch"));
  // IR hash too.
  j = mk("linear_identity.nam", "linear_identity.nam");
  j["cab"]["ir"]["sha256"] = std::string(64, 'f');
  REQUIRE_THROWS_WITH(loadResources(parsePreset(j, kPresets), kFs), ContainsSubstring("cab.ir.file"));
  // Missing file.
  j = mk("does_not_exist.nam", "linear_identity.nam");
  REQUIRE_THROWS_WITH(loadResources(parsePreset(j, kPresets), kFs), ContainsSubstring("paths.a.blocks[0].model.file"));
}

TEST_CASE("Chain: lifecycle errors", "[chain]") {
  const Preset p = parsePreset(mk("linear_identity.nam", "linear_identity.nam"), kPresets);
  Chain c(p, loadResources(p, kFs));
  REQUIRE_THROWS(c.prepare({44100.0, 256}));  // resources were built for 48 kHz
  REQUIRE_THROWS_AS(c.resolveAlignment(), std::logic_error);
  float s = 0.25f, o = 0.0f;
  c.process(&s, &o, 1);  // not prepared: pass-through, no crash
  REQUIRE(o == 0.25f);
  c.prepare({kFs, 64});
  // Blocks larger than maxBlockSize are split internally.
  const auto x = noise(1000, 22, 0.3f);
  std::vector<float> y(x.size());
  c.process(x.data(), y.data(), static_cast<int>(x.size()));
  REQUIRE(maxAbsDiff(y, x) <= 1e-6);
  // In-place processing.
  auto z = x;
  c.reset();
  c.process(z.data(), z.data(), static_cast<int>(z.size()));
  REQUIRE(maxAbsDiff(z, x) <= 1e-6);
}

// ---- whole-chain: zero allocation + block-size invariance ------------------------------------
namespace {

json richPreset(const TempDir& tmp, bool perPath) {
  json j = mk("wavenet.nam", "linear_05_025.nam");
  j["name"] = "rich";
  j["gate"] = {{"enabled", true}, {"thresholdDb", -50.0}};
  j["input"] = {{"gainDb", 2.0}};
  j["paths"]["a"]["preEq"] = json::array({{{"type", "highPass"}, {"freq", 60.0}, {"q", 0.7071}}});
  j["paths"]["a"]["blocks"].push_back({{"id", "a2"}, {"type", "eq"}, {"bands", json::array({{{"type", "peak"}, {"freq", 1500.0}, {"gainDb", 3.0}, {"q", 1.0}}})}});
  j["paths"]["a"]["eq"] = json::array({{{"type", "lowPass"}, {"freq", 8000.0}, {"q", 0.7071}}});
  j["paths"]["b"]["blocks"].push_back(namBlock("b2", "lstm.nam"));
  j["paths"]["b"]["levelDb"] = -3.0;
  j["blend"] = 0.55;
  const std::string ir = tmp.write("rich_ir.wav", decayingNoise(3000, 8));
  const std::string ir2 = tmp.write("rich_ir2.wav", decayingNoise(2500, 9));
  j["cab"] = perPath ? json{{"mode", "perPath"}, {"irA", {{"file", ir}}}, {"irB", {{"file", ir2}}}}
                     : json{{"mode", "shared"}, {"ir", {{"file", ir}}}};
  j["postEq"] = json::array({{{"type", "peak"}, {"freq", 1500.0}, {"gainDb", 2.0}, {"q", 1.2}}});
  j["busComp"] = {{"enabled", true}, {"thresholdDb", -18.0}, {"ratio", 3.0}};
  j["output"] = {{"gainDb", -3.0}};
  return j;
}

std::vector<float> richInput() {
  std::vector<float> x = noise(72000, 31, 0.3f);
  std::fill(x.begin() + 30000, x.begin() + 42000, 0.0f);  // let the gate close
  return x;
}

}  // namespace

TEST_CASE("Chain: process() does not allocate", "[chain][alloc]") {
  TempDir tmp;
  for (bool perPath : {false, true}) {
    auto chain = build(richPreset(tmp, perPath), 512);
    auto x = richInput();
    std::vector<float> y(1000);
    const int sizes[] = {512, 1, 64, 480, 7, 128, 300, 33, 512, 256, 100, 2, 1000};
    AllocGuard g;
    std::size_t pos = 0;
    for (int n : sizes) {
      chain->process(x.data() + pos, y.data(), n);  // n = 1000 also exercises internal splitting
      pos += static_cast<std::size_t>(n);
    }
    REQUIRE(g.count() == 0);
    for (float v : y) REQUIRE(std::isfinite(v));
  }
}

TEST_CASE("Chain: process() does not allocate with latency compensation, a bypassed block and inverted align", "[chain][alloc]") {
  registerLatencyStub();
  json j = mk("linear_identity.nam", "linear_neg1_at_23.nam");
  // A: latency stub (100 samples) + identity.  B: a bypassed WaveNet, then the -1 @ 23 model.
  j["paths"]["a"]["blocks"] = json::array({{{"id", "s1"}, {"type", "test.latency"}, {"latency", 100}}, namBlock("a1", "linear_identity.nam")});
  json bypassed = namBlock("b0", "wavenet.nam");
  bypassed["bypass"] = true;
  j["paths"]["b"]["blocks"] = json::array({bypassed, namBlock("b1", "linear_neg1_at_23.nam")});
  auto chain = build(j, 512);
  const ChainInfo info = chain->info();
  REQUIRE(info.compensationDelay[1] == 100);  // nonzero compensation delay on the path without the stub
  REQUIRE(info.pathLatency[1] == 0);          // the bypassed block adds no latency
  REQUIRE(info.align.invertB);
  REQUIRE(info.align.delaySamplesB == -23);
  REQUIRE(chain->latencySamples() == 100);

  auto x = noise(6000, 41, 0.3f);
  std::vector<float> y(1000);
  const int sizes[] = {512, 1, 64, 480, 7, 128, 300, 33, 512, 256, 100, 2, 1000};
  {
    AllocGuard g;
    std::size_t pos = 0;
    for (int n : sizes) {
      chain->process(x.data() + pos, y.data(), n);
      pos += static_cast<std::size_t>(n);
    }
    REQUIRE(g.count() == 0);
  }
  for (float v : y) REQUIRE(std::isfinite(v));
  // Sanity: the chain still does what it should (constructive sum, delayed by 100 + 23).
  chain->reset();
  REQUIRE(maxAbsDiff(render(*chain, x, 256), delayed(x, 123)) <= 1e-5);
}

TEST_CASE("Chain: reset() before prepare() is a harmless no-op", "[chain]") {
  const Preset p = parsePreset(mk("wavenet.nam", "lstm.nam"), kPresets);
  Chain c(p, loadResources(p, kFs));
  REQUIRE_NOTHROW(c.reset());
  c.reset();
  c.prepare({kFs, 128});
  const auto x = noise(4000, 42, 0.3f);
  const auto y = render(c, x, 128);
  Chain fresh(p, loadResources(p, kFs));
  fresh.prepare({kFs, 128});
  REQUIRE(maxAbsDiff(render(fresh, x, 128), y) == 0.0);  // identical to a chain that was never reset
}

TEST_CASE("Chain: output is independent of block size", "[chain][blocksize]") {
  TempDir tmp;
  const auto x = richInput();
  for (bool perPath : {false, true}) {
    const json j = richPreset(tmp, perPath);
    auto ref = build(j, 1000);
    const auto yref = render(*ref, x, 1000);
    const auto refAlign = ref->info().align;
    REQUIRE(rms(yref.data(), yref.size()) > 1e-3);  // not a vacuous comparison
    for (int block : {1, 64, 256, 1000}) {
      INFO("block " << block << (perPath ? " perPath" : " shared"));
      auto c = build(j, block);
      REQUIRE(c->info().align.delaySamplesB == refAlign.delaySamplesB);
      REQUIRE(c->info().align.invertB == refAlign.invertB);
      REQUIRE(c->latencySamples() == ref->latencySamples());
      REQUIRE(maxAbsDiff(render(*c, x, block), yref) <= 1e-5);
    }
  }
}
