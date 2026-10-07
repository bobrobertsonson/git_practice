// Task G: live dynamics policy. Gate floor follower (thresholdMode floorRelative), the preset v4 fields
// (liveDynamics / dynamicsMode / origin), the derivation rule, the active-set resolver and the Chain's atomic set switch.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/chain.h"
#include "sawblade/gate.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;
const fs::path kPresets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

// Deterministic white noise with the given RMS in dBFS (uniform, so RMS = peak / sqrt(3)).
struct Noise {
  std::uint32_t s = 12345u;
  float next(double rmsDb) {
    s = s * 1664525u + 1013904223u;
    const double u = (static_cast<double>(s >> 8) / 8388608.0) - 1.0;  // [-1, 1)
    return static_cast<float>(u * std::sqrt(3.0) * std::pow(10.0, rmsDb / 20.0));
  }
};

// A riff over a floor: 250 ms of a -12 dBFS (peak) 200 Hz-ish sine on, 150 ms of gap, repeated. The floor noise is always there.
std::vector<float> riff(double floorRmsDb, double seconds, bool withNotes, std::uint32_t seed = 1) {
  Noise nz;
  nz.s = seed;
  const auto n = static_cast<std::size_t>(seconds * kFs);
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    float v = nz.next(floorRmsDb);
    const double t = static_cast<double>(i) / kFs;
    if (withNotes && std::fmod(t, 0.4) < 0.25) v += static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979 * 196.0 * t));
    x[i] = v;
  }
  return x;
}

GateParams floorGate(double offset = 8.0) {
  GateParams g;
  g.enabled = true;
  g.thresholdMode = GateThresholdMode::FloorRelative;
  g.floorOffsetDb = offset;
  g.mode = GateMode::Expander;
  g.ratio = 2.0;
  g.rangeDb = -24.0;
  g.holdMs = 40.0;
  g.releaseMs = 120.0;
  return g;
}

Gate makeGate(const GateParams& p) {
  Gate g;
  g.setParams(p);
  g.prepare({kFs, 4096});
  return g;
}

void feed(Gate& g, const std::vector<float>& key, int block) {
  std::vector<float> io(key.size(), 1.0f);
  for (std::size_t pos = 0; pos < key.size();) {
    const auto n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), key.size() - pos));
    g.processKeyed(key.data() + pos, io.data() + pos, n);
    pos += static_cast<std::size_t>(n);
  }
}

json identityBlock(const std::string& id) {
  return {{"id", id}, {"type", "nam"}, {"model", {{"file", "../nam/linear_identity.nam"}}}};
}

// A preset with a record gate (hold 10 / release 20) and a bus comp.
json mkDyn(int version = 3) {
  return {{"schema", "sawblade.preset"}, {"version", version}, {"name", "dyn"},
          {"paths", {{"a", {{"blocks", json::array({identityBlock("a1")})}}}, {"b", {{"enabled", false}, {"blocks", json::array()}}}}},
          {"blend", 0.0},
          {"align", {{"mode", "off"}}},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}},
          {"gate", {{"enabled", true}, {"thresholdDb", -50.0}, {"holdMs", 10.0}, {"releaseMs", 20.0}, {"attackMs", 1.0},
                    {"hysteresisDb", 5.0}, {"releaseCurve", "linear-db"}}},
          {"busComp", {{"enabled", true}, {"thresholdDb", -20.0}, {"ratio", 4.0}, {"releaseMs", 100.0}}}};
}

Preset parse(const json& j) { return parsePreset(j, kPresets); }

std::unique_ptr<Chain> build(const Preset& p, int maxBlock = 512) {
  auto c = std::make_unique<Chain>(p, loadResources(p, kFs));
  c->prepare({kFs, maxBlock});
  return c;
}

std::vector<float> render(const Preset& p, const std::vector<float>& x, int block = 512) {
  auto c = build(p, block);
  std::vector<float> y(x.size());
  for (std::size_t pos = 0; pos < x.size();) {
    const auto n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos));
    c->process(x.data() + pos, y.data() + pos, n);
    pos += static_cast<std::size_t>(n);
  }
  return y;
}

}  // namespace

// ---- G.2 floor follower ---------------------------------------------------------------------------------------------------
TEST_CASE("Floor follower: converges to a known noise floor within 3.5 s", "[gate][floor]") {
  Gate g = makeGate(floorGate());
  feed(g, riff(-80.0, 3.5, false), 256);
  CHECK(std::fabs(g.floorEstimateDb() - (-80.0)) < 1.5);
  CHECK(std::fabs(g.openThresholdDb() - (g.floorEstimateDb() + 8.0)) < 1e-9);
}

TEST_CASE("Floor follower: 30 s of riffing at -12 dBFS over a -75 dB floor stays within 3 dB of the floor", "[gate][floor]") {
  Gate g = makeGate(floorGate());
  const auto x = riff(-75.0, 30.0, true);
  std::vector<float> io(x.size(), 1.0f);
  double worst = 0.0;
  const std::size_t step = 4800;
  for (std::size_t pos = 0; pos < x.size(); pos += step) {
    const auto n = static_cast<int>(std::min(step, x.size() - pos));
    g.processKeyed(x.data() + pos, io.data() + pos, n);
    if (pos > static_cast<std::size_t>(kFs)) worst = std::max(worst, std::fabs(g.floorEstimateDb() - (-75.0)));
  }
  CHECK(worst < 3.0);
}

TEST_CASE("Floor follower: a floor step -75 -> -60 dB is learned within 25 s", "[gate][floor]") {
  Gate g = makeGate(floorGate());
  feed(g, riff(-75.0, 8.0, true), 512);
  CHECK(std::fabs(g.floorEstimateDb() - (-75.0)) < 3.0);
  feed(g, riff(-60.0, 25.0, true, 99), 512);
  CHECK(std::fabs(g.floorEstimateDb() - (-60.0)) < 3.0);
}

TEST_CASE("Floor follower: when nothing qualifies for 10 s the estimate leaks up", "[gate][floor]") {
  Gate g = makeGate(floorGate());
  feed(g, riff(-85.0, 5.0, false), 512);
  const double before = g.floorEstimateDb();
  // A floor 40 dB above the estimate never qualifies (> estimate + 20 dB); after 10 s the estimate rises +1 dB/s and re-captures it.
  feed(g, riff(-40.0, 40.0, false, 7), 512);
  CHECK(g.floorEstimateDb() > before + 20.0);
}

TEST_CASE("Floor follower: follows a -12 dB input change without a re-match", "[gate][floor]") {
  Gate g = makeGate(floorGate());
  feed(g, riff(-70.0, 6.0, true), 512);
  const double t0 = g.openThresholdDb();
  // The whole input (notes and floor) scaled by -12 dB.
  std::vector<float> scaled = riff(-70.0, 6.0, true, 5);
  const auto k = static_cast<float>(std::pow(10.0, -12.0 / 20.0));
  for (float& s : scaled) s *= k;
  feed(g, scaled, 512);
  CHECK(std::fabs((g.openThresholdDb() - t0) - (-12.0)) < 1.0);
}

TEST_CASE("Floor follower: estimate is clamped to [-96, -40] dBFS", "[gate][floor]") {
  Gate g = makeGate(floorGate());
  feed(g, std::vector<float>(static_cast<std::size_t>(4 * kFs), 0.0f), 512);
  CHECK(g.floorEstimateDb() == Gate::kFloorMinDb);
}

TEST_CASE("Floor follower: output is independent of the block size (bit identical)", "[gate][floor]") {
  const auto key = riff(-72.0, 6.0, true, 3);
  std::vector<std::vector<float>> outs;
  for (int block : {1, 64, 512, 333}) {
    Gate g = makeGate(floorGate());
    std::vector<float> io = key;  // self-keyed through processKeyed with io aliasing key
    for (std::size_t pos = 0; pos < io.size();) {
      const auto n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), io.size() - pos));
      g.processKeyed(io.data() + pos, io.data() + pos, n);
      pos += static_cast<std::size_t>(n);
    }
    outs.push_back(io);
  }
  for (std::size_t k = 1; k < outs.size(); ++k) CHECK(outs[k] == outs[0]);
}

TEST_CASE("Floor follower: process() allocates nothing", "[gate][floor][alloc]") {
  Gate g = makeGate(floorGate());
  auto key = riff(-72.0, 2.0, true);
  std::vector<float> io(key.size(), 1.0f);
  AllocGuard guard;
  g.processKeyed(key.data(), io.data(), static_cast<int>(key.size()));
  CHECK(guard.count() == 0);
}

TEST_CASE("Gate absolute mode is unchanged by the floor follower (default params)", "[gate][floor]") {
  GateParams p;
  Gate g = makeGate(p);
  CHECK(g.params().thresholdMode == GateThresholdMode::Absolute);
  CHECK(g.openThresholdDb() == p.thresholdDb);
}

// ---- G.1 preset -----------------------------------------------------------------------------------------------------------
TEST_CASE("Derivation: origin match derives the live set; any other origin keeps the stored set", "[dynamics][preset]") {
  json jm = mkDyn(4);
  jm["origin"] = "match";
  const Preset m = parse(jm);
  const DynamicsSet live = liveDynamicsOf(m);
  CHECK(live.gate.enabled);
  CHECK(live.gate.mode == GateMode::Expander);
  CHECK(live.gate.ratio == 2.0);
  CHECK(live.gate.rangeDb == -24.0);
  CHECK(live.gate.keyHighPassHz == 80.0);
  CHECK(live.gate.thresholdMode == GateThresholdMode::FloorRelative);
  CHECK(live.gate.floorOffsetDb == 8.0);
  CHECK(live.gate.holdMs == 40.0);
  CHECK(live.gate.releaseMs == 120.0);
  CHECK(live.gate.attackMs == m.gate.attackMs);
  CHECK(live.gate.hysteresisDb == m.gate.hysteresisDb);
  CHECK(live.gate.releaseCurve == GateReleaseCurve::LinearDb);
  CHECK_FALSE(live.busComp.enabled);

  // Longer record hold / release are kept.
  json jl = jm;
  jl["gate"]["holdMs"] = 90.0;
  jl["gate"]["releaseMs"] = 300.0;
  const DynamicsSet live2 = liveDynamicsOf(parse(jl));
  CHECK(live2.gate.holdMs == 90.0);
  CHECK(live2.gate.releaseMs == 300.0);

  // A disabled / absent record gate -> disabled live gate.
  json jd = jm;
  jd.erase("gate");
  CHECK_FALSE(liveDynamicsOf(parse(jd)).gate.enabled);

  for (const char* origin : {"user", "official"}) {
    json j = mkDyn(4);
    j["origin"] = origin;
    const Preset p = parse(j);
    CHECK(liveDynamicsOf(p) == recordDynamicsOf(p));
  }
  const Preset absent = parse(mkDyn(4));  // origin absent = user
  CHECK(absent.origin == PresetOrigin::User);
  CHECK(liveDynamicsOf(absent) == recordDynamicsOf(absent));
  const Preset v3 = parse(mkDyn(3));
  CHECK(liveDynamicsOf(v3) == recordDynamicsOf(v3));
  CHECK(v3.gate.holdMs == 10.0);
}

TEST_CASE("Derivation: an explicit liveDynamics wins; dynamicsMode selects the active set", "[dynamics][preset]") {
  json j = mkDyn(4);
  j["origin"] = "match";
  j["liveDynamics"] = {{"gate", {{"enabled", true}, {"thresholdDb", -61.0}}}, {"busComp", {{"enabled", false}}}};
  Preset p = parse(j);
  REQUIRE(p.liveDynamics.has_value());
  CHECK(liveDynamicsOf(p).gate.thresholdDb == -61.0);
  CHECK(effectiveDynamicsMode(p) == DynamicsMode::Record);  // absent = record
  CHECK(activeDynamics(p) == recordDynamicsOf(p));
  p.dynamicsMode = DynamicsMode::Live;
  CHECK(activeDynamics(p) == liveDynamicsOf(p));
}

TEST_CASE("Preset v4: round trip, version 5 rejected, v1-3 files read as record", "[dynamics][preset]") {
  json j = mkDyn(4);
  j["origin"] = "match";
  j["dynamicsMode"] = "live";
  j["liveDynamics"] = {{"gate", {{"enabled", true}, {"thresholdMode", "floorRelative"}, {"floorOffsetDb", 6.0}}},
                       {"busComp", {{"enabled", false}}}};
  const Preset p = parse(j);
  CHECK(p.origin == PresetOrigin::Match);
  CHECK(p.dynamicsMode == DynamicsMode::Live);
  CHECK(p.liveDynamics->gate.thresholdMode == GateThresholdMode::FloorRelative);
  CHECK(p.liveDynamics->gate.floorOffsetDb == 6.0);
  const json out = toJson(p);
  CHECK(out["version"] == 4);
  CHECK(out["dynamicsMode"] == "live");
  CHECK(out["origin"] == "match");
  CHECK(parse(out) == p);

  j["version"] = 5;
  CHECK_THROWS_AS(parse(j), PresetError);  // a reader that supports up to 4 rejects 5 (and a v3 reader rejects 4 the same way)

  for (int ver : {1, 2, 3}) {
    const Preset q = parse(mkDyn(ver));
    CHECK_FALSE(q.dynamicsMode.has_value());
    CHECK_FALSE(q.liveDynamics.has_value());
    const json o = toJson(q);
    CHECK_FALSE(o.contains("dynamicsMode"));
    CHECK_FALSE(o.contains("liveDynamics"));
    CHECK_FALSE(o.contains("origin"));
    CHECK_FALSE(o["gate"].contains("thresholdMode"));  // the record gate serialises exactly as before
    CHECK(parse(o) == q);
  }
  json bad = mkDyn(4);
  bad["origin"] = "robot";
  CHECK_THROWS_AS(parse(bad), PresetError);
  bad = mkDyn(4);
  bad["dynamicsMode"] = "maybe";
  CHECK_THROWS_AS(parse(bad), PresetError);
}

// ---- render path ----------------------------------------------------------------------------------------------------------
TEST_CASE("Render: record mode is bit-identical to the v3 file; live mode runs the live set", "[dynamics][chain]") {
  const auto x = riff(-70.0, 3.0, true);
  json j3 = mkDyn(3);
  const std::vector<float> y3 = render(parse(j3), x);

  json jr = mkDyn(4);
  jr["dynamicsMode"] = "record";
  jr["origin"] = "match";
  CHECK(render(parse(jr), x) == y3);

  json jl = mkDyn(4);
  jl["dynamicsMode"] = "live";
  jl["origin"] = "match";  // live gate: expander floor-relative, comp off
  const auto yl = render(parse(jl), x);
  CHECK(yl != y3);

  // origin user + live: the stored set plays as set -> identical to record.
  json ju = mkDyn(4);
  ju["dynamicsMode"] = "live";
  CHECK(render(parse(ju), x) == y3);
}

TEST_CASE("Render: output is independent of the block size with a floorRelative live gate", "[dynamics][chain]") {
  json j = mkDyn(4);
  j["origin"] = "match";
  j["dynamicsMode"] = "live";
  const Preset p = parse(j);
  const auto x = riff(-72.0, 4.0, true, 11);
  const auto ref = render(p, x, 512);
  for (int block : {1, 64, 333}) {
    const auto y = render(p, x, block);
    double worst = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(y[i] - ref[i])));
    CHECK(worst < 1e-5);
  }
}

TEST_CASE("Chain: a dynamics set change through LiveParams is applied whole, between blocks, with no allocation", "[dynamics][chain][alloc]") {
  json j = mkDyn(4);
  j["origin"] = "match";
  const Preset rec = parse(j);  // record mode
  auto c = build(rec);
  LiveParams a = LiveParams::fromPreset(rec);
  CHECK(a.dynamics == recordDynamicsOf(rec));
  Preset livePreset = rec;
  livePreset.dynamicsMode = DynamicsMode::Live;
  const LiveParams b = LiveParams::fromPreset(livePreset);
  CHECK(b.dynamics == liveDynamicsOf(rec));
  CHECK(!(a.dynamics == b.dynamics));

  const auto x = riff(-70.0, 1.0, true);
  std::vector<float> y(x.size());
  AllocGuard guard;
  c->setLiveParams(b);
  CHECK(c->liveParams().dynamics == b.dynamics);
  c->process(x.data(), y.data(), static_cast<int>(x.size()));
  c->setLiveParams(a);
  CHECK(c->liveParams().dynamics == a.dynamics);
  CHECK(guard.count() == 0);
}

TEST_CASE("resolveDynamics: the record slots hold the active set (what the exporter and the notes read)", "[dynamics][preset]") {
  json j = mkDyn(4);
  j["origin"] = "match";
  Preset p = parse(j);
  CHECK(resolveDynamics(p).gate == p.gate);  // record mode: unchanged
  CHECK(resolveDynamics(p).busComp == p.busComp);
  p.dynamicsMode = DynamicsMode::Live;
  const Preset r = resolveDynamics(p);
  CHECK(r.gate == liveDynamicsOf(p).gate);
  CHECK_FALSE(r.busComp.enabled);
  CHECK_FALSE(r.liveDynamics.has_value());
  CHECK(r.origin == PresetOrigin::User);
  CHECK(r.dynamicsMode == DynamicsMode::Live);  // the label survives
  CHECK(activeDynamics(r) == activeDynamics(p));  // what a reader of the resolved preset plays is what the rig plays
  CHECK(resolveDynamics(r) == r);
}
