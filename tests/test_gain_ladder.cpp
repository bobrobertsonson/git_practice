// v0.2 Task B (core): gain ladders: schema, rung selection with hysteresis, the LadderBlock (warm-up + equal-power
// crossfade), the Chain's GAIN -> rung / residual-drive wiring, bit-identity and offline determinism.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "alloc_guard.h"
#include "latency_stub.h"
#include "sawblade/capture_cache.h"
#include "sawblade/chain.h"
#include "sawblade/gain_ladder.h"
#include "sawblade/nam_block.h"
#include "sawblade/render.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;
const fs::path kNam = fs::path(SAWBLADE_FIXTURES_DIR) / "nam";
const fs::path kPresets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

// A TONE3000 capture cache in a temp dir (the in-process override of captureCacheRoot()).
struct CacheDir {
  fs::path dir;
  CacheDir() {
    static int n = 0;
    dir = fs::temp_directory_path() / ("sawblade_ladder_cache_" + std::to_string(std::random_device{}()) + "_" + std::to_string(n++));
    fs::create_directories(dir / "T1");
    setCaptureCacheRootOverride(dir);
  }
  ~CacheDir() {
    setCaptureCacheRootOverride(std::nullopt);
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void put(const std::string& model, const char* fixture) const { fs::copy_file(kNam / fixture, dir / "T1" / (model + ".nam"), fs::copy_options::overwrite_existing); }
};

// Rungs: m1 gain 2 (identity; the block's own capture), m2 gain 5 (0.5 x[n] + 0.25 x[n-1]), m3 gain 8 (identity).
// Knob positions 0 / 5 / 10.
json ladderJson() {
  return json::array({{{"modelId", "m1"}, {"gain", 2.0}, {"name", "Gain 2"}},
                      {{"modelId", "m2"}, {"gain", 5.0}, {"name", "Gain 5"}},
                      {{"modelId", "m3"}, {"gain", 8.0}, {"name", "Gain 8"}}});
}

json namBlock(const std::string& id, bool withLadder, const char* file = "linear_identity.nam") {
  json model = {{"file", (kNam / file).string()}};
  if (withLadder) {
    model["source"] = {{"provider", "tone3000"}, {"id", "T1"}, {"modelId", "m1"}};
    model["ladder"] = ladderJson();
  }
  return {{"id", id}, {"type", "nam"}, {"slot", "amp"}, {"model", model}};
}

// Path A: the ladder block, path B: plain identity, blend 0 (only A is heard). No cab.
json mk(bool withLadder = true, const json& amp = nullptr, const char* ownFile = "linear_identity.nam") {
  json a = {{"blocks", json::array({namBlock("a1", withLadder, ownFile)})}};
  if (!amp.is_null()) a["ampControls"] = amp;
  return {{"schema", "sawblade.preset"}, {"version", 2}, {"name", "ladder"},
          {"paths", {{"a", a}, {"b", {{"blocks", json::array({namBlock("b1", false)})}}}}},
          {"align", {{"mode", "off"}}}, {"blend", 0.0},
          {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}};
}

std::unique_ptr<Chain> build(const json& j, int maxBlock = 512, CaptureCache* cache = nullptr) {
  const Preset p = parsePreset(j, kPresets);
  auto c = std::make_unique<Chain>(p, loadResources(p, kFs, cache));
  c->prepare({kFs, maxBlock});
  return c;
}

std::vector<float> run(Chain& c, const std::vector<float>& x, int block) {
  std::vector<float> y(x.size());
  for (std::size_t pos = 0; pos < x.size();) {
    const auto n = std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos);
    c.process(x.data() + pos, y.data() + pos, static_cast<int>(n));
    pos += n;
  }
  return y;
}

// Loads the rung models of path A's amp block and hands them to the chain.
void publishAll(Chain& c, const Preset& p, std::initializer_list<int> rungs) {
  const auto& nam = static_cast<const NamBlockParams&>(*p.a.blocks[0].params);
  std::vector<LadderBlock::Entry> es;
  for (int r : rungs) {
    std::string why;
    auto proc = buildRungProcessor(nam, r, {kFs, 512}, nullptr, &why);
    INFO(why);
    REQUIRE(proc != nullptr);
    es.push_back({r, std::move(proc)});
  }
  REQUIRE(c.ladderBlock(0) != nullptr);
  c.ladderBlock(0)->publishRungs(std::move(es));
}

}  // namespace

// ---- schema -----------------------------------------------------------------------------------------------
TEST_CASE("Ladder schema: round trip, ascending order, strings for ids", "[ladder][preset]") {
  json j = mk();
  j["paths"]["a"]["blocks"][0]["model"]["ladder"] = json::array({{{"modelId", "m9"}, {"gain", 8}}, {{"modelId", "m1"}, {"gain", 2}, {"name", "x"}}});
  j["paths"]["a"]["blocks"][0]["model"]["source"]["modelId"] = "m1";
  const Preset p = parsePreset(j, kPresets);
  const auto& m = static_cast<const NamBlockParams&>(*p.a.blocks[0].params).model;
  REQUIRE(m.ladder.size() == 2);
  CHECK(m.ladder[0].modelId == "m1");  // sorted by gain
  CHECK(m.ladder[1].modelId == "m9");
  CHECK(m.ladder[1].name.empty());
  const json out = toJson(p);
  CHECK(out["paths"]["a"]["blocks"][0]["model"]["ladder"][0]["name"] == "x");
  CHECK_FALSE(out["paths"]["a"]["blocks"][0]["model"]["ladder"][1].contains("name"));
  CHECK(parsePreset(out, kPresets) == p);
  CHECK(ownRungIndex(m) == 0);
  // No ladder: nothing is written.
  CHECK_FALSE(toJson(parsePreset(mk(false), kPresets))["paths"]["a"]["blocks"][0]["model"].contains("ladder"));
}

TEST_CASE("Ladder schema: invalid ladders are PresetErrors naming the field", "[ladder][preset]") {
  const auto err = [](json l) -> std::string {
    json j = mk();
    j["paths"]["a"]["blocks"][0]["model"]["ladder"] = std::move(l);
    try {
      parsePreset(j, kPresets);
    } catch (const PresetError& e) {
      return e.jsonPath();
    }
    return "(no error)";
  };
  const std::string base = "paths.a.blocks[0].model.ladder";
  CHECK(err(json::array({{{"modelId", "m1"}, {"gain", 2}}})) == base);  // one rung is not a ladder
  CHECK(err(json::object()) == base);
  CHECK(err(json::array({{{"modelId", "m1"}, {"gain", 2}}, {{"modelId", "m2"}}})) == base + "[1].gain");
  CHECK(err(json::array({{{"modelId", "m1"}, {"gain", 2}}, {{"gain", 3}}})) == base + "[1].modelId");
  CHECK(err(json::array({{{"modelId", "m1"}, {"gain", 2}}, {{"modelId", 7}, {"gain", 3}}})) == base + "[1].modelId");
  CHECK(err(json::array({{{"modelId", "m1"}, {"gain", 2}}, {{"modelId", "m1"}, {"gain", 3}}})) == base + "[1].modelId");
  CHECK(err(json::array({{{"modelId", "m1"}, {"gain", 2}}, {{"modelId", "m2"}, {"gain", 2}}})) == base + "[1].gain");
  CHECK(err(json::array({{{"modelId", "m1"}, {"gain", 2}}, {{"modelId", "m2"}, {"gain", 101}}})) == base + "[1].gain");
  CHECK(err(json::array({{{"modelId", "m1"}, {"gain", 2}}, {{"modelId", "m2"}, {"gain", 3}, {"x", 1}}})) == base + "[1].x");
  CHECK(err(ladderJson()) == "(no error)");
  // Only NAM model captures carry a ladder.
  json j = mk();
  j["cab"] = {{"mode", "shared"}, {"ir", {{"file", "x.wav"}, {"ladder", ladderJson()}}}};
  CHECK_THROWS_AS(parsePreset(j, kPresets), PresetError);
}

// ---- selection ----------------------------------------------------------------------------------------------
TEST_CASE("Ladder: knob positions, nearest rung with hysteresis, residual drive", "[ladder]") {
  const std::vector<LadderRung> l = {{"a", 2.0, ""}, {"b", 5.0, ""}, {"c", 8.0, ""}};
  const auto pos = ladderPositions(l);
  REQUIRE(pos.size() == 3);
  CHECK(pos[0] == 0.0);
  CHECK(pos[1] == Catch::Approx(5.0));
  CHECK(pos[2] == Catch::Approx(10.0));
  // Boundary between rung 0 and 1 is 2.5; the switch needs the knob 0.15 past it.
  CHECK(selectRung(pos, 0, 2.5) == 0);
  CHECK(selectRung(pos, 0, 2.649) == 0);
  CHECK(selectRung(pos, 0, 2.651) == 1);
  CHECK(selectRung(pos, 1, 2.5) == 1);   // resting on the boundary: no flip-flop
  CHECK(selectRung(pos, 1, 2.351) == 1);
  CHECK(selectRung(pos, 1, 2.349) == 0);
  CHECK(selectRung(pos, 1, 7.649) == 1);
  CHECK(selectRung(pos, 1, 7.651) == 2);
  CHECK(selectRung(pos, 2, 7.5) == 2);
  CHECK(selectRung(pos, 2, 7.349) == 1);
  CHECK(selectRung(pos, 0, 10.0) == 2);  // may jump several rungs
  CHECK(selectRung(pos, 2, 0.0) == 0);
  CHECK(selectRung(pos, 7, 0.0) == 0);   // out-of-range current is clamped
  CHECK(selectRung({}, 0, 5.0) == -1);
  // A knob dithering around a boundary never changes the rung.
  int cur = 1;
  for (int i = 0; i < 1000; ++i) cur = selectRung(pos, cur, 2.5 + 0.14 * std::sin(i * 0.7));
  CHECK(cur == 1);
  // Residual drive: (knob - position) * 2.4 dB, clamped to +-12.
  CHECK(ladderResidualDb(5.0, 5.0) == 0.0);
  CHECK(ladderResidualDb(6.0, 5.0) == Catch::Approx(2.4));
  CHECK(ladderResidualDb(4.0, 5.0) == Catch::Approx(-2.4));
  CHECK(ladderResidualDb(10.0, 0.0) == 12.0);
  CHECK(ladderResidualDb(0.0, 10.0) == -12.0);
  // Degenerate (all gains equal): every position is 0.
  CHECK(ladderPositions({{"a", 3.0, ""}, {"b", 3.0, ""}})[1] == 0.0);
}

// ---- the chain ------------------------------------------------------------------------------------------------
TEST_CASE("Ladder: with a ladder and no gainStep the render is bit-identical to the same preset without one", "[ladder][chain][neutral]") {
  const CacheDir cache;
  cache.put("m2", "linear_05_025.nam");
  cache.put("m3", "linear_identity.nam");
  const auto x = noise(30000, 3, 0.3f);
  for (const json& amp : {json(nullptr), json{{"mid", 7}, {"level", 6}}}) {
    auto plain = build(mk(false, amp));
    auto lad = build(mk(true, amp));
    CHECK(run(*lad, x, 100) == run(*plain, x, 333));
    const LadderState st = lad->ladderState(0);
    REQUIRE(st.has);
    CHECK(st.active == 0);
    CHECK(st.own == 0);
    CHECK_FALSE(st.pending);
  }
  CHECK_FALSE(build(mk(false))->ladderState(0).has);
  CHECK_FALSE(build(mk(true))->ladderState(1).has);
}

TEST_CASE("Ladder: gainStep selects the rung offline, with no crossfade", "[ladder][chain][render]") {
  const CacheDir cache;
  cache.put("m2", "linear_05_025.nam");
  cache.put("m3", "linear_identity_loud24.nam");
  const auto x = noise(20000, 5, 0.3f);
  // gainStep m2 with the knob on its position (5): it is the plain m2 chain.
  auto a = build(mk(true, json{{"gainStep", "m2"}}));
  auto plain = build(mk(false, nullptr, "linear_05_025.nam"));
  CHECK(a->ladderState(0).active == 1);
  CHECK(run(*a, x, 64) == run(*plain, x, 64));
  // Knob 7: residual (7 - 5) * 2.4 = +4.8 dB on top of m2.
  auto b = build(mk(true, json{{"gainStep", "m2"}, {"gain", 7.0}}));
  auto plain7 = build(mk(false, json{{"gain", 7.0}}, "linear_05_025.nam"));
  const auto yb = run(*b, x, 64), yp = run(*plain7, x, 64);
  for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(yb[i] == Catch::Approx(yp[i]).margin(1e-6));
  // The offline renderer (tonerender / export) does the same.
  AudioFile in;
  in.sampleRate = kFs;
  in.channels = 1;
  in.interleaved = x;
  const RenderResult r = renderPreset(parsePreset(mk(true, json{{"gainStep", "m2"}}), kPresets), in);
  const RenderResult rp = renderPreset(parsePreset(mk(false, nullptr, "linear_05_025.nam"), kPresets), in);
  CHECK(r.samples == rp.samples);
  // A rung that is not cached, or not in the ladder: the block's own capture, drive only, with a warning.
  auto miss = build(mk(true, json{{"gainStep", "m9"}}));
  CHECK(miss->ladderState(0).active == 0);
  bool warned = false;
  for (const auto& w : miss->info().warnings) warned = warned || w.find("m9") != std::string::npos;
  CHECK(warned);
  fs::remove(cache.dir / "T1" / "m3.nam");
  auto uncached = build(mk(true, json{{"gainStep", "m3"}}));
  CHECK(uncached->ladderState(0).active == 0);
}

TEST_CASE("Ladder: moving GAIN picks the rung, pending until its model is loaded, drive only meanwhile", "[ladder][chain]") {
  const CacheDir cache;
  cache.put("m2", "linear_05_025.nam");
  cache.put("m3", "linear_identity.nam");
  const Preset p = parsePreset(mk(true), kPresets);
  auto c = build(mk(true));
  const auto x = sine(300.0, kFs, 48000, 0.1);
  LiveParams lp = c->liveParams();
  lp.amp[0].gain = 10.0;  // the top rung m3 (identity) at position 10
  c->setLiveParams(lp);
  auto y = run(*c, x, 128);
  LadderState st = c->ladderState(0);
  CHECK(st.target == 2);
  CHECK(st.pending);          // m3 is not loaded: stay on m1, drive only
  CHECK(st.committed == 0);
  // Drive only: knob 10 on rung m1 (position 0) = +12 dB (clamped), the identity model.
  CHECK(toDb(rms(y.data() + 24000, 24000) / rms(x.data() + 24000, 24000)) == Catch::Approx(12.0).margin(0.05));
  // Hand over the models: the block switches, and the drive residual goes to 0 (knob 10 is rung m3's position).
  publishAll(*c, p, {1, 2});
  y = run(*c, x, 128);
  y = run(*c, x, 128);
  st = c->ladderState(0);
  CHECK(st.active == 2);
  CHECK(st.committed == 2);
  CHECK_FALSE(st.pending);
  CHECK(toDb(rms(y.data() + 24000, 24000) / rms(x.data() + 24000, 24000)) == Catch::Approx(0.0).margin(0.05));
  // Back to the bottom: m1 is still loaded (its own model), so no wait.
  lp.amp[0].gain = 0.0;
  c->setLiveParams(lp);
  y = run(*c, x, 128);
  y = run(*c, x, 128);
  CHECK(c->ladderState(0).active == 0);
  CHECK(toDb(rms(y.data() + 24000, 24000) / rms(x.data() + 24000, 24000)) == Catch::Approx(0.0).margin(0.05));
}

TEST_CASE("Ladder: hysteresis through the chain (a knob resting on a boundary does not flip-flop)", "[ladder][chain]") {
  const CacheDir cache;
  cache.put("m2", "linear_05_025.nam");
  cache.put("m3", "linear_identity.nam");
  const Preset p = parsePreset(mk(true), kPresets);
  auto c = build(mk(true));
  publishAll(*c, p, {1, 2});
  const auto x = noise(2000, 1, 0.1f);
  LiveParams lp = c->liveParams();
  int changes = 0, last = c->ladderState(0).target;
  for (int i = 0; i < 400; ++i) {
    lp.amp[0].gain = 2.5 + 0.14 * std::sin(i * 0.9);  // dithers around the 0 / 1 boundary at 2.5
    c->setLiveParams(lp);
    std::vector<float> y(512);
    c->process(x.data(), y.data(), 512);
    const int t = c->ladderState(0).target;
    changes += t != last;
    last = t;
  }
  CHECK(changes == 0);
  lp.amp[0].gain = 2.7;  // past the boundary + 0.15
  c->setLiveParams(lp);
  CHECK(c->ladderState(0).target == 1);
}

TEST_CASE("Ladder: a crossfade between two linear models has no click versus the ideal equal-power fade", "[ladder][rt]") {
  // Rung 0 = identity (active), rung 1 = 0.5 x[n] + 0.25 x[n-1]. A 440 Hz sine; the target moves to rung 1 at sample S.
  // The incoming model warms up for kLadderWarmMs, then the fade runs kLadderFadeMs. The ideal output is
  // cos(w) * old + sin(w) * new, with old / new the two models run over the whole sine (so both are warm).
  const CacheDir cache;
  cache.put("m2", "linear_05_025.nam");
  const std::size_t N = 48000, S = 9600;
  const auto x = sine(440.0, kFs, N, 0.5);
  const auto model = [&](const char* f) {
    auto m = NamBlock::load(kNam / f, NamBlockConfig{});
    m->prepare({kFs, 512});
    auto y = x;
    m->process(y.data(), static_cast<int>(y.size()));
    return y;
  };
  const auto yOld = model("linear_identity.nam"), yNew = model("linear_05_025.nam");
  const int warm = static_cast<int>(std::llround(kLadderWarmMs * 0.001 * kFs));
  const int fade = static_cast<int>(std::llround(kLadderFadeMs * 0.001 * kFs));
  std::vector<float> ideal(N);
  for (std::size_t i = 0; i < N; ++i) {
    const long k = static_cast<long>(i) - static_cast<long>(S) - warm;
    const double t = std::clamp(static_cast<double>(k) / fade, 0.0, 1.0) * std::numbers::pi * 0.5;
    ideal[i] = static_cast<float>(std::cos(t) * yOld[i] + std::sin(t) * yNew[i]);
  }
  for (const int block : {1, 7, 64, 480, 4096}) {
    CAPTURE(block);
    auto lb = std::make_unique<LadderBlock>(2, 0, [&] { auto m = NamBlock::load(kNam / "linear_identity.nam", NamBlockConfig{}); return std::unique_ptr<Processor>(std::move(m)); }());
    lb->prepare({kFs, 4096});
    std::vector<LadderBlock::Entry> es;
    {
      auto m = NamBlock::load(kNam / "linear_05_025.nam", NamBlockConfig{});
      m->prepare({kFs, 4096});
      es.push_back({1, std::move(m)});
    }
    lb->publishRungs(std::move(es));
    std::vector<float> y = x;
    for (std::size_t pos = 0; pos < N;) {
      if (pos == S) lb->setTargetRung(1);
      auto n = std::min<std::size_t>(static_cast<std::size_t>(block), N - pos);
      if (pos < S && pos + n > S) n = S - pos;
      lb->process(y.data() + pos, static_cast<int>(n));
      pos += n;
    }
    // Largest deviation of the sample-to-sample step from the ideal's, and of the signal itself.
    double stepDev = 0.0, valDev = 0.0;
    for (std::size_t i = 1; i < N; ++i) {
      stepDev = std::max(stepDev, std::fabs(static_cast<double>(y[i]) - y[i - 1] - (static_cast<double>(ideal[i]) - ideal[i - 1])));
      valDev = std::max(valDev, std::fabs(static_cast<double>(y[i]) - ideal[i]));
    }
    CHECK(toDb(stepDev) < -60.0);
    CHECK(toDb(valDev) < -60.0);
    CHECK(lb->activeRung() == 1);
    CHECK(lb->committedRung() == 1);
    CHECK_FALSE(lb->pending());
    // The fade itself is equal power: the settled output is the new model exactly.
    for (std::size_t i = S + static_cast<std::size_t>(warm + fade) + 8; i < N; ++i) REQUIRE(y[i] == Catch::Approx(yNew[i]).margin(1e-6));
  }
}

TEST_CASE("Ladder: the output is bit-identical for any block size across a swap", "[ladder]") {
  const CacheDir cache;
  cache.put("m2", "linear_05_025.nam");
  const Preset p = parsePreset(mk(true), kPresets);
  const auto x = noise(30000, 8, 0.3f);
  std::vector<float> ref;
  for (const int block : {1, 33, 512, 100000}) {
    CAPTURE(block);
    auto c = build(mk(true));
    publishAll(*c, p, {1});
    std::vector<float> y(x.size());
    LiveParams lp = c->liveParams();
    for (std::size_t pos = 0; pos < x.size();) {
      if (pos == 6000) {
        lp.amp[0].gain = 6.0;  // nearest rung m2 (position 5)
        c->setLiveParams(lp);
      }
      auto n = std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos);
      if (pos < 6000 && pos + n > 6000) n = 6000 - pos;
      c->process(x.data() + pos, y.data() + pos, static_cast<int>(n));
      pos += n;
    }
    CHECK(c->ladderState(0).active == 1);
    if (ref.empty()) ref = y;
    CHECK(y == ref);
  }
}

TEST_CASE("Ladder: a swap, the staged hand-over and knob moves allocate nothing on the audio thread", "[ladder][rt]") {
  const CacheDir cache;
  cache.put("m2", "linear_05_025.nam");
  cache.put("m3", "linear_identity.nam");
  const Preset p = parsePreset(mk(true), kPresets);
  auto c = build(mk(true));
  publishAll(*c, p, {1, 2});  // staged + published here (producer), consumed inside the guard
  const auto x = noise(512, 2);
  std::vector<float> y(512);
  LiveParams lp = c->liveParams();
  {
    AllocGuard g;
    for (int i = 0; i < 600; ++i) {
      lp.amp[0].gain = 5.0 + 5.0 * std::sin(i * 0.05);  // sweeps across all rungs
      lp.amp[0].bass = 5.0 + 3.0 * std::sin(i * 0.11);
      c->setLiveParams(lp);
      c->process(x.data(), y.data(), 1 + (i * 37) % 512);
    }
    CHECK(g.count() == 0);
  }
  CHECK(c->ladderState(0).loadedMask == 0b111);
  for (float v : y) REQUIRE(std::isfinite(v));
}

TEST_CASE("Ladder: latency never changes across swaps; a rung with another latency is rejected", "[ladder][latency]") {
  registerLatencyStub();
  const CacheDir cache;
  cache.put("m2", "linear_05_025.nam");
  const Preset p = parsePreset(mk(true), kPresets);
  auto c = build(mk(true));
  const int lat = c->latencySamples();
  publishAll(*c, p, {1});
  // A rung whose model reports another latency (a delaying processor): rejected, never runs.
  std::vector<LadderBlock::Entry> es;
  auto bad = std::make_unique<LatencyStub>(37);
  bad->prepare({kFs, 512});
  es.push_back({2, std::move(bad)});
  const auto x = noise(4096, 4);
  LiveParams lp = c->liveParams();
  std::vector<float> y(512);
  c->process(x.data(), y.data(), 512);                    // the audio thread takes the first batch,
  CHECK(c->ladderBlock(0)->publishRungs(std::move(es)));  // the previous batch was taken: handed over at once
  c->process(x.data(), y.data(), 512);
  for (double g : {5.0, 10.0, 0.0, 5.0}) {
    lp.amp[0].gain = g;
    c->setLiveParams(lp);
    for (int i = 0; i < 8; ++i) c->process(x.data() + i * 512, y.data(), 512);
    CHECK(c->latencySamples() == lat);
  }
  const LadderState st = c->ladderState(0);
  CHECK((st.rejectedMask & 0b100) != 0);
  CHECK((st.loadedMask & 0b100) == 0);
  CHECK((c->ladderBlock(0)->knownMask() & 0b100) == 0);  // the producer no longer counts a model the audio thread dropped
  CHECK(st.committed != 2);
  CHECK(c->info().latencySamples == lat);
  CHECK(c->info().pathLatency[0] == c->info().pathLatency[1]);
}

TEST_CASE("Ladder: the gainStep rung only needs its model in the cache; models are shared via the CaptureCache", "[ladder]") {
  const CacheDir cache;
  cache.put("m2", "linear_05_025.nam");
  const Preset p = parsePreset(mk(true), kPresets);
  const auto& nam = static_cast<const NamBlockParams&>(*p.a.blocks[0].params);
  CHECK(locateRungFile(nam.model, nam.model.ladder[1]).has_value());
  CHECK_FALSE(locateRungFile(nam.model, nam.model.ladder[2]).has_value());
  std::string why;
  CHECK(buildRungProcessor(nam, 2, {kFs, 512}, nullptr, &why) == nullptr);
  CHECK_THAT(why, ContainsSubstring("m3"));
  CaptureCache cc;
  CHECK(buildRungProcessor(nam, 1, {kFs, 512}, &cc, &why) != nullptr);
  CHECK(buildRungProcessor(nam, 1, {kFs, 512}, &cc, &why) != nullptr);
  CHECK(cc.stats().hits >= 1);
  CHECK(buildRungProcessor(nam, 9, {kFs, 512}, nullptr, &why) == nullptr);
}

TEST_CASE("Ladder: rungs staged while the audio thread has not taken the previous batch are not lost", "[ladder]") {
  const CacheDir cache;
  cache.put("m2", "linear_05_025.nam");
  cache.put("m3", "linear_identity.nam");
  const Preset p = parsePreset(mk(true), kPresets);
  auto c = build(mk(true));
  publishAll(*c, p, {1});  // batch 1, not taken yet (no audio has run)
  const auto& nam = static_cast<const NamBlockParams&>(*p.a.blocks[0].params);
  std::vector<LadderBlock::Entry> es;
  es.push_back({2, buildRungProcessor(nam, 2, {kFs, 512}, nullptr)});
  CHECK_FALSE(c->ladderBlock(0)->publishRungs(std::move(es)));  // staged
  CHECK((c->ladderBlock(0)->knownMask() & 0b110) == 0b110);
  const auto x = noise(512, 1);
  std::vector<float> y(512);
  c->process(x.data(), y.data(), 512);  // takes batch 1
  CHECK(c->ladderState(0).loadedMask == 0b011);
  CHECK(c->ladderBlock(0)->flushRungs());
  c->process(x.data(), y.data(), 512);  // takes batch 2
  CHECK(c->ladderState(0).loadedMask == 0b111);
  // Evict a model that is not sounding: the producer frees it.
  std::vector<LadderBlock::Entry> ev;
  ev.push_back({2, nullptr});
  CHECK(c->ladderBlock(0)->publishRungs(std::move(ev)));
  c->process(x.data(), y.data(), 512);
  CHECK(c->ladderState(0).loadedMask == 0b011);
  CHECK((c->ladderBlock(0)->knownMask() & 0b100) == 0);
  // The sounding rung is never evicted, and the producer still sees it as known.
  std::vector<LadderBlock::Entry> ev2;
  ev2.push_back({0, nullptr});
  c->ladderBlock(0)->publishRungs(std::move(ev2));
  c->process(x.data(), y.data(), 512);
  CHECK(c->ladderState(0).loadedMask == 0b011);
  CHECK((c->ladderBlock(0)->knownMask() & 0b001) != 0);
}

TEST_CASE("Ladder: hand-overs, flushes of a staged batch and evictions racing the audio thread allocate nothing there", "[ladder][rt]") {
  const CacheDir cache;
  cache.put("m2", "linear_05_025.nam");
  cache.put("m3", "linear_identity.nam");
  const Preset p = parsePreset(mk(true), kPresets);
  const auto& nam = static_cast<const NamBlockParams&>(*p.a.blocks[0].params);
  auto c = build(mk(true));
  publishAll(*c, p, {1, 2});
  LadderBlock* lb = c->ladderBlock(0);
  std::atomic<bool> stop{false};
  std::atomic<int> handOvers{0};
  // The producer: evicts and re-publishes rungs 1 and 2 continuously (some batches are staged behind an untaken one and flushed
  // later). Its own allocations are not counted: the guard is per thread.
  std::thread producer([&] {
    int i = 0;
    while (!stop.load()) {
      std::vector<LadderBlock::Entry> es;
      const int r = 1 + (i % 2);
      if ((i / 2) % 2 == 0) es.push_back({r, nullptr});  // eviction
      else es.push_back({r, buildRungProcessor(nam, r, {kFs, 512}, nullptr)});
      lb->publishRungs(std::move(es));
      lb->flushRungs();
      ++i;
      handOvers.store(i);
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
  });
  const auto x = noise(512, 2);
  std::vector<float> y(512);
  LiveParams lp = c->liveParams();
  // Run audio until the producer has really raced it: at least 3000 blocks AND at least 20 hand-overs observed (the producer thread may
  // not even be scheduled while a fast machine runs 3000 blocks), bounded by a generous wall clock; only that bound fails the count.
  long allocs = 0;
  int blocks = 0;
  const auto t0 = std::chrono::steady_clock::now();
  {
    AllocGuard g;
    for (int i = 0; i < 3000 || handOvers.load() < 20; ++i, ++blocks) {
      if ((i & 63) == 0 && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10)) break;
      lp.amp[0].gain = 5.0 + 5.0 * std::sin(i * 0.02);
      c->setLiveParams(lp);
      c->process(x.data(), y.data(), 1 + (i * 37) % 512);
    }
    allocs = g.count();
  }
  stop.store(true);
  producer.join();
  INFO("audio blocks " << blocks << ", producer hand-overs " << handOvers.load() << ", audio-thread allocations " << allocs);
  CHECK(allocs == 0);
  CHECK(handOvers.load() >= 20);
  for (float v : y) REQUIRE(std::isfinite(v));
}
