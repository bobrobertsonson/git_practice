// Phase 10.1: path level matching (probe, trims), constant-loudness blend law, 6 dB sum headroom.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/bus_comp.h"
#include "sawblade/chain.h"
#include "sawblade/loudness.h"
#include "sawblade/render.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Approx;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;
const fs::path kPresets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

json nam(const std::string& id, const std::string& file) {
  return {{"id", id}, {"type", "nam"}, {"model", {{"file", "../nam/" + file}}}};
}

// Two paths (aFile / bFile), B at `levelDbB`, align off, impulse cab, 50/50.
json mk(const std::string& aFile, const std::string& bFile, double levelDbB = 0.0) {
  return {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "lm"},
          {"paths", {{"a", {{"blocks", json::array({nam("a1", aFile)})}}},
                     {"b", {{"blocks", json::array({nam("b1", bFile)})}, {"levelDb", levelDbB}}}}},
          {"align", {{"mode", "off"}}},
          {"blend", 0.5},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}}};
}

json withAuto(json j, const char* law = "constantLoudness") {
  j["levelMatch"] = {{"mode", "auto"}};
  j["blendLaw"] = law;
  return j;
}

std::unique_ptr<Chain> build(const json& j, int maxBlock = 512) {
  const Preset p = parsePreset(j, kPresets);
  auto res = loadResources(p, kFs);
  auto c = std::make_unique<Chain>(p, std::move(res));
  c->prepare({kFs, maxBlock});
  return c;
}

std::vector<float> run(Chain& c, const std::vector<float>& x, int block) {
  std::vector<float> y(x.size());
  for (std::size_t pos = 0; pos < x.size(); pos += static_cast<std::size_t>(block)) {
    const auto n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos));
    c.process(x.data() + pos, y.data() + pos, n);
  }
  return y;
}

double lufsOf(const float* x, std::size_t n) {
  const std::vector<float> zeros(n, 0.0f);
  const auto l = integratedLoudnessLufs(x, zeros.data(), static_cast<std::int64_t>(n), kFs);
  REQUIRE(l.has_value());
  return *l;
}

}  // namespace

TEST_CASE("Level match: probe is deterministic across runs and block sizes", "[levelmatch]") {
  const json j = withAuto(mk("wavenet.nam", "linear_identity.nam", -3.0));
  const ChainInfo a = build(j, 512)->info();
  const ChainInfo b = build(j, 512)->info();
  const ChainInfo c = build(j, 64)->info();
  REQUIRE(a.lufs[0] > LevelMatchResult::kNoLufs);
  for (std::size_t k = 0; k < 2; ++k) {
    CHECK(a.trimDb[k] == b.trimDb[k]);  // same block size: bit-identical
    CHECK(a.lufs[k] == b.lufs[k]);
    CHECK(a.trimDb[k] == Approx(c.trimDb[k]).margin(1e-3));  // other block size: float tolerance
    CHECK(a.lufs[k] == Approx(c.lufs[k]).margin(1e-3));
  }
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(a.makeupDb[i] == b.makeupDb[i]);
    CHECK(a.makeupDb[i] == Approx(c.makeupDb[i]).margin(1e-3));
  }
  CHECK(a.sumLufs == b.sumLufs);
}

TEST_CASE("Level match: a 6 dB levelDb offset between copies is cancelled by the trims", "[levelmatch]") {
  const ChainInfo i = build(withAuto(mk("linear_identity.nam", "linear_identity.nam", -6.0)))->info();
  CHECK(i.levelMatchMode == LevelMatchMode::Auto);
  CHECK(i.trimDb[0] == Approx(0.0).margin(0.1));  // the louder path gets 0
  CHECK(i.trimDb[1] == Approx(6.0).margin(0.1));  // the quieter one the difference
  CHECK(i.lufs[0] - i.lufs[1] == Approx(6.0).margin(0.1));
  // Trimmed, the paths are identical in loudness: the sum is as loud as path A alone.
  CHECK(i.sumLufs == Approx(i.lufs[0]).margin(0.1));
}

TEST_CASE("Level match: constant-loudness output is within +-0.3 LU across the blend", "[levelmatch]") {
  const auto x = noise(static_cast<std::size_t>(2.5 * kFs), 7, 0.2f);
  std::vector<double> lufs;
  for (const double b : {0.0, 0.25, 0.5, 0.75, 1.0}) {
    auto c = build(withAuto(mk("linear_identity.nam", "linear_identity.nam", -6.0)));
    LiveParams p = c->liveParams();
    p.blend = b;
    c->setLiveParams(p);
    const auto y = run(*c, x, 256);
    const auto skip = static_cast<std::size_t>(1.0 * kFs);  // the 20 ms ramp has long settled
    lufs.push_back(lufsOf(y.data() + skip, y.size() - skip));
  }
  const double ref = lufs[2];
  for (std::size_t i = 0; i < lufs.size(); ++i) CHECK(lufs[i] == Approx(ref).margin(0.3));
}

TEST_CASE("Level match: the live law toggle switches without a rebuild and without allocation", "[levelmatch][rt]") {
  auto c = build(withAuto(mk("linear_identity.nam", "linear_identity.nam", -6.0), "linear"), 512);
  const int latency = c->latencySamples();
  CHECK(build(withAuto(mk("linear_identity.nam", "linear_identity.nam", -6.0), "constantLoudness"))->latencySamples() == latency);
  const auto x = noise(512, 3, 0.2f);
  std::vector<float> y(512);
  LiveParams p = c->liveParams();
  AllocGuard guard;
  for (int i = 0; i < 400; ++i) {
    p.blend = 0.5 + 0.45 * std::sin(i * 0.05);  // the blend keeps ramping
    p.blendLaw = (i / 50) % 2 ? BlendLaw::Linear : BlendLaw::ConstantLoudness;
    c->setLiveParams(p);
    c->process(x.data(), y.data(), 512);
  }
  CHECK(guard.count() == 0);
  CHECK(c->latencySamples() == latency);
}

TEST_CASE("Level match: absent keys equal explicit off + linear, bit for bit", "[levelmatch]") {
  const json legacy = mk("linear_identity.nam", "wavenet.nam", -4.0);
  json explicitOff = legacy;
  explicitOff["levelMatch"] = {{"mode", "off"}};
  explicitOff["blendLaw"] = "linear";
  const Preset p = parsePreset(legacy, kPresets);
  CHECK(p.levelMatch.mode == LevelMatchMode::Off);
  CHECK(p.blendLaw == BlendLaw::Linear);
  const auto x = noise(20000, 5, 0.3f);
  auto a = build(legacy);
  auto b = build(explicitOff);
  CHECK(run(*a, x, 128) == run(*b, x, 128));
  CHECK(a->info().trimDb == std::array<double, 2>{0.0, 0.0});
}

TEST_CASE("Level match: modes (off measures only, manual uses the stored trims)", "[levelmatch]") {
  json off = mk("linear_identity.nam", "linear_identity.nam", -6.0);
  off["blendLaw"] = "constantLoudness";  // off + linear never probes
  const ChainInfo io = build(off)->info();
  CHECK(io.trimDb == std::array<double, 2>{0.0, 0.0});
  CHECK(io.lufs[0] - io.lufs[1] == Approx(6.0).margin(0.1));  // still measured, for a live law toggle

  json manual = off;
  manual["blendLaw"] = "linear";
  manual["levelMatch"] = {{"mode", "manual"}, {"trimADb", 1.5}, {"trimBDb", 7.0}};
  const ChainInfo im = build(manual)->info();
  CHECK(im.trimDb[0] == 1.5);
  CHECK(im.trimDb[1] == 7.0);
  // The manual trims show in the audio: A is 1.5 dB and B 7 dB up -> the sum is +1.0 dB over an untrimmed copy.
  json plainJ = off;
  plainJ["blendLaw"] = "linear";
  auto plain = build(plainJ);
  auto trimmed = build(manual);
  const auto x = noise(static_cast<std::size_t>(kFs), 11, 0.1f);
  const auto yp = run(*plain, x, 256), yt = run(*trimmed, x, 256);
  const double expected = std::pow(10.0, 1.5 / 20.0) * 0.5 + std::pow(10.0, 7.0 / 20.0) * std::pow(10.0, -6.0 / 20.0) * 0.5;
  const auto n = yp.size() / 2;
  const double gain = rms(yt.data() + n, yp.size() - n) / rms(yp.data() + n, yp.size() - n);
  CHECK(gain == Approx(expected / (0.5 + std::pow(10.0, -6.0 / 20.0) * 0.5)).epsilon(1e-3));
}

TEST_CASE("Level match: a single enabled path measures nothing (trims and make-up are 0)", "[levelmatch]") {
  json j = withAuto(mk("linear_identity.nam", "linear_identity.nam", -6.0));
  j["paths"]["b"]["enabled"] = false;
  const ChainInfo i = build(j)->info();
  CHECK(i.trimDb == std::array<double, 2>{0.0, 0.0});
  CHECK(i.makeupDb == std::array<double, 5>{});
  CHECK(i.lufs[0] == LevelMatchResult::kNoLufs);
}

TEST_CASE("Level match: trims are clamped to +18 dB", "[levelmatch]") {
  const ChainInfo i = build(withAuto(mk("linear_identity.nam", "linear_identity.nam", -40.0)))->info();
  CHECK(i.trimDb[0] == Approx(0.0).margin(0.1));
  CHECK(i.trimDb[1] == Approx(18.0).margin(1e-9));
}

TEST_CASE("Level match: preset keys round-trip and are validated", "[levelmatch][preset]") {
  json j = mk("linear_identity.nam", "linear_identity.nam");
  j["levelMatch"] = {{"mode", "manual"}, {"trimADb", 0.0}, {"trimBDb", 4.25}};
  j["blendLaw"] = "constantLoudness";
  const Preset p = parsePreset(j, kPresets);
  CHECK(p.levelMatch.mode == LevelMatchMode::Manual);
  CHECK(p.levelMatch.trimBDb == 4.25);
  CHECK(p.blendLaw == BlendLaw::ConstantLoudness);
  const json out = toJson(p);
  CHECK(out["levelMatch"]["mode"] == "manual");
  CHECK(out["levelMatch"]["trimBDb"] == 4.25);
  CHECK(out["blendLaw"] == "constantLoudness");
  CHECK(parsePreset(out, kPresets) == p);
  // The writer always emits both keys, with the defaults for a legacy preset.
  const json legacy = toJson(parsePreset(mk("linear_identity.nam", "linear_identity.nam"), kPresets));
  CHECK(legacy["levelMatch"]["mode"] == "off");
  CHECK(legacy["blendLaw"] == "linear");
  json bad = j;
  bad["levelMatch"]["trimADb"] = 18.5;
  CHECK_THROWS_AS(parsePreset(bad, kPresets), PresetError);
  bad = j;
  bad["levelMatch"]["trimADb"] = -0.5;
  CHECK_THROWS_AS(parsePreset(bad, kPresets), PresetError);
  bad = j;
  bad["blendLaw"] = "cubic";
  CHECK_THROWS_AS(parsePreset(bad, kPresets), PresetError);
  bad = j;
  bad["levelMatch"]["mode"] = "sometimes";
  CHECK_THROWS_AS(parsePreset(bad, kPresets), PresetError);
}

TEST_CASE("Level match: the render report carries the measured values", "[levelmatch][report]") {
  const Preset p = parsePreset(withAuto(mk("linear_identity.nam", "linear_identity.nam", -6.0)), kPresets);
  AudioFile in{kFs, 1, noise(24000, 2, 0.1f)};
  RenderOptions o;
  o.renderRate = kFs;
  const json rep = reportJson(renderPreset(p, in, o));
  CHECK(rep["levelMatch"]["mode"] == "auto");
  CHECK(rep["levelMatch"]["trimBDb"].get<double>() == Approx(6.0).margin(0.1));
  CHECK(rep["levelMatch"]["lufsA"].is_number());
  CHECK(rep["levelMatch"]["sumLufs"].is_number());
  CHECK(rep["blend"]["value"] == 0.5);
  CHECK(rep["blend"]["law"] == "constantLoudness");
  CHECK(rep["blend"]["makeupDb"].size() == 5);
}

TEST_CASE("Headroom: the bus compressor behaves as before the sum node ran 6 dB down", "[levelmatch][headroom]") {
  // Unit: a compressor fed x with threshold T equals 2 x compressor(0.5 x, T - 6.0206 dB).
  BusCompParams cp;
  cp.enabled = true;
  cp.thresholdDb = -18.0;
  cp.ratio = 4.0;
  cp.kneeDb = 6.0;
  cp.attackMs = 2.0;
  cp.releaseMs = 80.0;
  cp.makeupDb = 2.0;
  auto x = noise(48000, 4, 0.8f);
  BusCompressor ref;
  ref.setParams(cp);
  ref.prepare({kFs, 512});
  auto yRef = x;
  processChunked(ref, yRef, {512});
  BusCompParams down = cp;
  down.thresholdDb += Chain::kHeadroomDb;
  BusCompressor half;
  half.setParams(down);
  half.prepare({kFs, 512});
  auto yHalf = x;
  for (auto& v : yHalf) v *= 0.5f;
  processChunked(half, yHalf, {512});
  double maxDiff = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(2.0 * yHalf[i] - yRef[i]));
  CHECK(maxDiff < 1e-5);

  // Chain: identical identity paths, comp on, linear law: the chain output equals the plain compressor.
  json j = mk("linear_identity.nam", "linear_identity.nam");
  j["busComp"] = {{"enabled", true}, {"thresholdDb", cp.thresholdDb}, {"ratio", cp.ratio}, {"kneeDb", cp.kneeDb},
                  {"attackMs", cp.attackMs}, {"releaseMs", cp.releaseMs}, {"makeupDb", cp.makeupDb}};
  auto c = build(j);
  const auto y = run(*c, x, 256);
  const auto lat = static_cast<std::size_t>(c->latencySamples());
  REQUIRE(lat < 64);
  double chainDiff = 0.0;
  for (std::size_t i = 0; i + lat < x.size(); ++i) chainDiff = std::max(chainDiff, std::fabs(static_cast<double>(y[i + lat]) - yRef[i]));
  CHECK(chainDiff < 1e-5);
}

TEST_CASE("Headroom: without a compressor the sum node is transparent", "[levelmatch][headroom]") {
  // Identical identity paths at 50/50 through the impulse cab: output == input (x0.5 and x2 are exact).
  json j = mk("linear_identity.nam", "linear_identity.nam");
  auto c = build(j);
  const auto x = noise(8192, 6, 0.9f);
  const auto y = run(*c, x, 100);
  const auto lat = static_cast<std::size_t>(c->latencySamples());
  for (std::size_t i = 0; i + lat < x.size(); ++i) REQUIRE(y[i + lat] == Approx(x[i]).margin(2e-6));
}

TEST_CASE("Level match: a legacy-shaped preset never probes and renders the golden exactly", "[levelmatch][golden]") {
  const fs::path presets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";
  const fs::path di = fs::path(SAWBLADE_FIXTURES_DIR) / "di_riff.wav";
  const RenderResult r = renderFile(presets / "golden_perpath.json", di);  // LSTM, manual alignment, no new keys
  CHECK_FALSE(r.info.levelMeasured);
  CHECK(r.info.trimDb == std::array<double, 2>{0.0, 0.0});
  CHECK(r.info.makeupDb == std::array<double, 5>{});
  CHECK_FALSE(reportJson(r)["levelMatch"]["measured"].get<bool>());
  const AudioFile g = readWav(fs::path(SAWBLADE_GOLDEN_DIR) / "golden_perpath.wav");
  REQUIRE(g.interleaved.size() == r.samples.size());
  double maxDiff = 0.0;
  for (std::size_t i = 0; i < r.samples.size(); ++i)
    maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(g.interleaved[i]) - r.samples[i]));
  CHECK(maxDiff == 0.0);

  // The same preset with the constant-loudness law is measured and has a non-trivial curve.
  json j = json::parse(std::ifstream(presets / "golden_perpath.json"));
  j["blendLaw"] = "constantLoudness";
  const Preset p = parsePreset(j, presets);
  AudioFile in = readWav(di);
  in.interleaved.resize(48000);
  const RenderResult m = renderPreset(p, in, RenderOptions{});
  CHECK(m.info.levelMeasured);
  double span = 0.0;
  for (double v : m.info.makeupDb) span = std::max(span, std::fabs(v));
  CHECK(span > 0.1);
}
