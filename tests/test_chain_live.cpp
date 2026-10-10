// Chain::setLiveParams: continuous controls change in place (no reload), smoothed, RT-safe.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/chain.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;
const fs::path kPresets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

json identityBlock(const std::string& id) {
  return {{"id", id}, {"type", "nam"}, {"model", {{"file", "../nam/linear_identity.nam"}}}};
}

// A: identity. B: identity at -6.0206 dB (x0.5). No alignment, impulse cab, optional gate/postEq.
json mk(double blend, bool gate = false) {
  json j = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "live"},
            {"paths", {{"a", {{"blocks", json::array({identityBlock("a1")})}}},
                       {"b", {{"blocks", json::array({identityBlock("b1")})}, {"levelDb", -6.020599913279624}}}}},
            {"align", {{"mode", "off"}}},
            {"blend", blend},
            {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}},
            {"postEq", json::array({{{"type", "peak"}, {"freq", 1000}, {"gainDb", 0.0}, {"q", 1.0}},
                                    {{"type", "highShelf"}, {"freq", 6000}, {"gainDb", 0.0}, {"q", 0.7}}})}};
  if (gate) j["gate"] = {{"enabled", true}, {"thresholdDb", -55.0}, {"holdMs", 5.0}, {"releaseMs", 5.0}};
  return j;
}

std::unique_ptr<Chain> build(const json& j, int maxBlock = 512) {
  const Preset p = parsePreset(j, kPresets);
  auto res = loadResources(p, kFs);
  auto c = std::make_unique<Chain>(p, std::move(res));
  c->prepare({kFs, maxBlock});
  return c;
}

std::vector<float> run(Chain& c, const std::vector<float>& x, const std::vector<int>& sizes) {
  std::vector<float> y(x.size());
  std::size_t pos = 0, k = 0;
  while (pos < x.size()) {
    const auto n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(sizes[k++ % sizes.size()]), x.size() - pos));
    c.process(x.data() + pos, y.data() + pos, n);
    pos += static_cast<std::size_t>(n);
  }
  return y;
}

// mk() plus a pre EQ and a path EQ on both paths (peak bands, then a low shelf in the path EQ).
json mkEqs(double blend) {
  json j = mk(blend);
  for (const char* k : {"a", "b"}) {
    j["paths"][k]["preEq"] = json::array({{{"type", "peak"}, {"freq", 300}, {"gainDb", 4.0}, {"q", 1.0}}});
    j["paths"][k]["eq"] = json::array({{{"type", "peak"}, {"freq", 2500}, {"gainDb", -5.0}, {"q", 0.8}},
                                       {{"type", "highPass"}, {"freq", 80}, {"gainDb", 0.0}, {"q", 0.7}}});
  }
  return j;
}

double maxDiffFrom(const std::vector<float>& a, const std::vector<float>& b, std::size_t from) {
  double m = 0.0;
  for (std::size_t i = from; i < a.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}

std::vector<float> ones(std::size_t n, float v = 0.25f) { return std::vector<float>(n, v); }

}  // namespace

TEST_CASE("Chain live params: unchanged values are a no-op", "[chain][live]") {
  const auto x = noise(8000, 4);
  auto a = build(mk(0.4));
  auto b = build(mk(0.4));
  b->setLiveParams(b->liveParams());
  b->setLiveParams(LiveParams::fromPreset(parsePreset(mk(0.4), kPresets)));
  CHECK(run(*a, x, {256}) == run(*b, x, {256}));
}

TEST_CASE("Chain live params: gains ramp smoothly and sample-accurately", "[chain][live]") {
  auto a = build(mk(0.0));  // blend 0 = path A only: y = x * in * out
  auto b = build(mk(0.0));
  LiveParams p = a->liveParams();
  p.outputGainDb = -12.0;
  a->setLiveParams(p);
  b->setLiveParams(p);
  const auto x = ones(6000);
  const auto ya = run(*a, x, {1});
  const auto yb = run(*b, x, {977, 3, 400});
  CHECK(ya == yb);  // the ramp is per-sample: independent of block size

  const double target = 0.25 * std::pow(10.0, -12.0 / 20.0);
  CHECK(ya.back() == Catch::Approx(target).epsilon(1e-5));
  CHECK(ya.front() > 0.24);  // starts at the old gain
  // Monotone, with small steps (zipper noise would be a jump): the ramp lasts ~20 ms = 960 samples.
  double maxStep = 0.0;
  for (std::size_t i = 1; i < ya.size(); ++i) {
    CHECK(ya[i] <= ya[i - 1] + 1e-7f);
    maxStep = std::max(maxStep, static_cast<double>(std::fabs(ya[i] - ya[i - 1])));
  }
  CHECK(maxStep < 0.002);
  CHECK(ya[1500] == Catch::Approx(target).epsilon(1e-5));  // settled after the ramp
}

TEST_CASE("Chain live params: blend and per-path level", "[chain][live]") {
  auto c = build(mk(0.0));
  const auto x = ones(4000);
  CHECK(run(*c, x, {512}).back() == Catch::Approx(0.25).epsilon(1e-5));  // A only
  LiveParams p = c->liveParams();
  p.blend = 1.0;
  c->setLiveParams(p);
  CHECK(run(*c, x, {512}).back() == Catch::Approx(0.125).epsilon(1e-5));  // B only (x0.5)
  p.blend = 0.5;
  c->setLiveParams(p);
  CHECK(run(*c, x, {512}).back() == Catch::Approx(0.1875).epsilon(1e-5));  // 0.5*A + 0.5*B
  p.levelDbB = 0.0;  // B back to unity
  c->setLiveParams(p);
  CHECK(run(*c, x, {512}).back() == Catch::Approx(0.25).epsilon(1e-5));
  p.levelDbA = -200.0;  // A (almost) off
  c->setLiveParams(p);
  CHECK(run(*c, x, {512}).back() == Catch::Approx(0.125).epsilon(1e-5));
}

TEST_CASE("Chain live params: post-EQ band gain equals a chain built with that gain", "[chain][live]") {
  const double fs = kFs;
  const auto x = sine(1000.0, fs, 24000, 0.25);
  auto live = build(mk(0.5));
  LiveParams p = live->liveParams();
  p.postEq[0].gainDb = 9.0;  // peak 1 kHz, Q 1
  live->setLiveParams(p);
  const auto yl = run(*live, x, {100, 33, 512});

  json j = mk(0.5);
  j["postEq"][0]["gainDb"] = 9.0;
  auto ref = build(j);
  const auto yr = run(*ref, x, {512});
  const std::size_t i0 = 12000;  // after the 20 ms ramp
  double maxDiff = 0.0;
  for (std::size_t i = i0; i < x.size(); ++i) maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(yl[i] - yr[i])));
  CHECK(maxDiff < 1e-5);
  CHECK(rms(yl.data() + i0, x.size() - i0) / rms(x.data() + i0, x.size() - i0) > 1.5);  // it really boosts

  // The ramp itself is smooth: a block of constant input moves without discontinuities.
  auto c2 = build(mk(0.5));
  LiveParams q = c2->liveParams();
  q.postEq[1].gainDb = -12.0;  // high shelf; DC passes unchanged, so a DC input shows only transients
  c2->setLiveParams(q);
  const auto yd = run(*c2, ones(4000), {64});
  for (std::size_t i = 1; i < yd.size(); ++i) CHECK(std::fabs(yd[i] - yd[i - 1]) < 0.01f);
}

TEST_CASE("Chain live params: gate threshold moves without a reload", "[chain][live]") {
  auto c = build(mk(0.0, true));
  const auto x = sine(220.0, kFs, 48000, 0.03);  // about -33 dBFS peak
  const auto open = run(*c, x, {512});
  CHECK(rms(open.data() + 24000, 24000) > 0.015);
  LiveParams p = c->liveParams();
  p.gateThresholdDb = -20.0;
  c->setLiveParams(p);
  const auto closed = run(*c, x, {512});
  CHECK(rms(closed.data() + 24000, 24000) < 0.0005);
}

TEST_CASE("Chain live params: setLiveParams and process allocate nothing", "[chain][live][rt]") {
  auto c = build(mk(0.5, true), 512);
  const auto x = noise(512, 2);
  std::vector<float> y(512);
  LiveParams p = c->liveParams();
  AllocGuard guard;
  for (int i = 0; i < 400; ++i) {
    p.inputGainDb = std::sin(i * 0.1) * 6.0;
    p.outputGainDb = std::cos(i * 0.07) * 6.0;
    p.blend = 0.5 + 0.4 * std::sin(i * 0.05);
    p.levelDbA = std::sin(i * 0.03) * 3.0;
    p.levelDbB = std::cos(i * 0.09) * 3.0;
    p.gateThresholdDb = -60.0 + 20.0 * std::sin(i * 0.02);
    p.postEq[0].gainDb = 9.0 * std::sin(i * 0.11);
    p.postEq[1].gainDb = 9.0 * std::cos(i * 0.13);
    p.postEq[0].freq = 1000.0 * std::pow(2.0, std::sin(i * 0.04));
    p.postEq[0].q = std::exp(0.5 * std::cos(i * 0.06));
    for (std::size_t k = 0; k < 2; ++k) {
      p.preEq[k][0].freq = 300.0 * std::pow(2.0, std::sin(i * 0.05 + static_cast<double>(k)));
      p.preEq[k][0].gainDb = 6.0 * std::sin(i * 0.08);
      p.pathEq[k][0].q = 0.5 + 0.4 * std::cos(i * 0.07 + static_cast<double>(k));
      p.pathEq[k][0].freq = 2500.0 * std::pow(2.0, std::cos(i * 0.03));
      p.blocks[k][0].inputGainDb = 6.0 * std::sin(i * 0.1 + static_cast<double>(k));
      p.blocks[k][0].outputGainDb = 3.0 * std::cos(i * 0.12);
    }
    p.muteA = (i / 37) % 2 == 1;
    p.muteB = (i / 53) % 2 == 1;
    c->setLiveParams(p);
    c->process(x.data(), y.data(), 1 + (i * 53) % 512);
  }
  CHECK(guard.count() == 0);
}

TEST_CASE("Chain live params: a post-EQ ramp is bit-identical for any block size", "[chain][live]") {
  const auto x = sine(900.0, kFs, 9000, 0.3);
  std::vector<float> ref;
  for (const int block : {1, 7, 64, 512, 100000}) {
    CAPTURE(block);
    auto c = build(mk(0.5), 512);
    LiveParams p = c->liveParams();
    p.postEq[0].gainDb = 12.0;
    p.postEq[1].gainDb = -9.0;
    c->setLiveParams(p);
    const auto y = run(*c, x, {block});
    if (ref.empty()) ref = y;
    CHECK(y == ref);
  }
  // A second ramp started mid-stream, at the same sample position, is also invariant.
  std::vector<float> ref2;
  for (const int block : {1, 7, 64, 512}) {
    CAPTURE(block);
    auto c = build(mk(0.5), 512);
    std::vector<float> y(x.size());
    LiveParams p = c->liveParams();
    p.postEq[0].gainDb = 6.0;
    c->setLiveParams(p);
    for (std::size_t pos = 0; pos < x.size();) {
      if (pos == 4032) {  // 4032 is a multiple of every block size used except 7: split there
        p.postEq[0].gainDb = -6.0;
        c->setLiveParams(p);
      }
      std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos);
      if (pos < 4032 && pos + n > 4032) n = 4032 - pos;
      c->process(x.data() + pos, y.data() + pos, static_cast<int>(n));
      pos += n;
    }
    if (ref2.empty()) ref2 = y;
    CHECK(y == ref2);
  }
}

TEST_CASE("Chain live params: a live freq/Q/gain change equals a chain built with those values",
          "[chain][live][eq]") {
  const auto x = noise(48000, 11);
  struct Case {
    const char* name;
    double blend;
    std::function<void(LiveParams&)> live;
    std::function<void(json&)> built;
  };
  const std::vector<Case> cases = {
      {"post", 0.5,
       [](LiveParams& p) { p.postEq[0].freq = 2200.0; p.postEq[0].q = 2.5; p.postEq[0].gainDb = 7.0; },
       [](json& j) { j["postEq"][0]["freq"] = 2200.0; j["postEq"][0]["q"] = 2.5; j["postEq"][0]["gainDb"] = 7.0; }},
      {"preA", 0.0,
       [](LiveParams& p) { p.preEq[0][0].freq = 700.0; p.preEq[0][0].q = 3.0; p.preEq[0][0].gainDb = -6.0; },
       [](json& j) {
         j["paths"]["a"]["preEq"][0]["freq"] = 700.0;
         j["paths"]["a"]["preEq"][0]["q"] = 3.0;
         j["paths"]["a"]["preEq"][0]["gainDb"] = -6.0;
       }},
      {"pathA", 0.0,
       [](LiveParams& p) { p.pathEq[0][0].freq = 4100.0; p.pathEq[0][0].q = 0.4; p.pathEq[0][1].freq = 140.0; p.pathEq[0][1].q = 1.5; },
       [](json& j) {
         j["paths"]["a"]["eq"][0]["freq"] = 4100.0;
         j["paths"]["a"]["eq"][0]["q"] = 0.4;
         j["paths"]["a"]["eq"][1]["freq"] = 140.0;
         j["paths"]["a"]["eq"][1]["q"] = 1.5;
       }},
      {"pathB", 1.0,
       [](LiveParams& p) { p.pathEq[1][0].freq = 1800.0; p.pathEq[1][0].gainDb = 8.0; },
       [](json& j) { j["paths"]["b"]["eq"][0]["freq"] = 1800.0; j["paths"]["b"]["eq"][0]["gainDb"] = 8.0; }},
  };
  for (const auto& cs : cases) {
    CAPTURE(cs.name);
    auto live = build(mkEqs(cs.blend));
    LiveParams p = live->liveParams();
    cs.live(p);
    live->setLiveParams(p);
    const auto yl = run(*live, x, {100, 33, 512});
    json j = mkEqs(cs.blend);
    cs.built(j);
    auto ref = build(j);
    const auto yr = run(*ref, x, {512});
    CHECK(maxDiffFrom(yl, yr, 24000) <= 1e-6);
    // and it does change the sound relative to the unmodified chain
    auto base = build(mkEqs(cs.blend));
    CHECK(maxDiffFrom(run(*base, x, {512}), yr, 24000) > 1e-3);
  }
}

TEST_CASE("Chain live params: the EQ freq/Q/gain ramps are bit-identical for any block size",
          "[chain][live][eq]") {
  const auto x = noise(12000, 5);
  std::vector<float> ref;
  for (const int block : {1, 7, 64, 1024}) {
    CAPTURE(block);
    auto c = build(mkEqs(0.5), 1024);
    LiveParams p = c->liveParams();
    p.postEq[0].freq = 3000.0;
    p.postEq[0].q = 2.0;
    p.preEq[0][0].freq = 900.0;
    p.preEq[1][0].q = 3.0;
    p.pathEq[0][0].freq = 500.0;
    p.pathEq[0][0].gainDb = 6.0;
    p.pathEq[1][1].freq = 200.0;
    c->setLiveParams(p);
    std::vector<float> y(x.size());
    for (std::size_t pos = 0; pos < x.size();) {
      if (pos == 4032) {  // second ramp mid-stream, at a position every block size reaches
        p.postEq[0].freq = 1200.0;
        p.pathEq[0][0].q = 2.0;
        c->setLiveParams(p);
      }
      std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos);
      if (pos < 4032 && pos + n > 4032) n = 4032 - pos;
      c->process(x.data() + pos, y.data() + pos, static_cast<int>(n));
      pos += n;
    }
    if (ref.empty()) ref = y;
    CHECK(y == ref);
  }
}

TEST_CASE("Chain live params: invalid EQ values are ignored", "[chain][live][eq]") {
  const auto x = noise(8000, 6);
  auto a = build(mkEqs(0.5));
  auto b = build(mkEqs(0.5));
  LiveParams p = b->liveParams();
  p.postEq[0].freq = std::nan("");
  p.postEq[0].q = -1.0;
  p.preEq[0][0].freq = 5.0;      // below 10 Hz
  p.pathEq[0][0].freq = 30000.0;  // above 0.49 fs
  p.pathEq[0][0].gainDb = std::numeric_limits<double>::infinity();
  b->setLiveParams(p);
  CHECK(b->liveParams() == a->liveParams());
  CHECK(run(*a, x, {256}) == run(*b, x, {256}));
}

TEST_CASE("Chain live params: live block gains equal a chain built with them", "[chain][live][blocks]") {
  const auto x = noise(12000, 8);
  json j = mk(0.0);
  auto live = build(j);
  LiveParams p = live->liveParams();
  p.blocks[0][0].inputGainDb = 6.0;
  p.blocks[0][0].outputGainDb = -3.0;
  live->setLiveParams(p);
  const auto yl = run(*live, x, {64, 1, 300});
  j["paths"]["a"]["blocks"][0]["inputGainDb"] = 6.0;
  j["paths"]["a"]["blocks"][0]["outputGainDb"] = -3.0;
  auto ref = build(j);
  CHECK(maxDiffFrom(yl, run(*ref, x, {512}), 2000) <= 1e-6);
  // the ramp is block-size independent
  auto live2 = build(mk(0.0));
  live2->setLiveParams(p);
  CHECK(run(*live2, x, {977}) == yl);
}

TEST_CASE("Chain live params: mute ramps to exact silence and back", "[chain][live][mute]") {
  auto c = build(mk(0.0));
  const auto x = ones(6000);
  CHECK(run(*c, x, {512}).back() == Catch::Approx(0.25).epsilon(1e-5));
  LiveParams p = c->liveParams();
  p.muteA = true;
  c->setLiveParams(p);
  const auto ym = run(*c, x, {64});
  for (std::size_t i = 1; i < ym.size(); ++i) CHECK(ym[i] <= ym[i - 1] + 1e-7f);
  for (std::size_t i = 1100; i < ym.size(); ++i) CHECK(ym[i] == 0.0f);
  p.levelDbA = -6.0;  // a level change while muted only changes the stored target
  c->setLiveParams(p);
  for (const float v : run(*c, x, {512})) CHECK(v == 0.0f);
  p.muteA = false;
  c->setLiveParams(p);
  const auto yu = run(*c, x, {512});
  CHECK(yu.back() == Catch::Approx(0.25 * std::pow(10.0, -6.0 / 20.0)).epsilon(1e-5));
  CHECK(yu.front() < 0.01f);
}
