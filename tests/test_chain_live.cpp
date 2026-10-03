// Chain::setLiveParams: continuous controls change in place (no reload), smoothed, RT-safe.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
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
  p.postEqGainDb[0] = 9.0;  // peak 1 kHz, Q 1
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
  q.postEqGainDb[1] = -12.0;  // high shelf; DC passes unchanged, so a DC input shows only transients
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
    p.postEqGainDb[0] = 9.0 * std::sin(i * 0.11);
    p.postEqGainDb[1] = 9.0 * std::cos(i * 0.13);
    c->setLiveParams(p);
    c->process(x.data(), y.data(), 1 + (i * 53) % 512);
  }
  CHECK(guard.count() == 0);
}
