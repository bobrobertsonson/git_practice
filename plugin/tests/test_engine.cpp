// Engine: Chain + real-time rate conversion + exact latency (JUCE-free).
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include "Engine.h"
#include "alloc_guard.h"
#include "latency_stub.h"
#include "lock_guard.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::plugin;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kPresetDir = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

json identityBlock(const std::string& id) {
  return {{"id", id}, {"type", "nam"}, {"model", {{"file", "../nam/linear_identity.nam"}}}};
}

// Both paths: identity NAM (+ a real N-sample delay that reports N when stubLatency > 0).
// Blend 0.5 of two identical paths is the identity, so the whole engine is a pure delay.
Preset identityPreset(int stubLatency = 0) {
  registerLatencyStub();
  json a = json::array({identityBlock("a1")}), b = json::array({identityBlock("b1")});
  if (stubLatency > 0) {
    a.push_back({{"id", "as"}, {"type", "test.latency"}, {"latency", stubLatency}});
    b.push_back({{"id", "bs"}, {"type", "test.latency"}, {"latency", stubLatency}});
  }
  json j = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "identity"},
            {"paths", {{"a", {{"blocks", a}}}, {"b", {{"blocks", b}}}}},
            {"align", {{"mode", "off"}}},
            {"blend", 0.5},
            {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}},
            {"postEq", json::array({{{"type", "peak"}, {"freq", 1000}, {"gainDb", 0.0}, {"q", 1.0}}})}};
  return parsePreset(j, kPresetDir);
}

void runEngine(Engine& e, const std::vector<float>& x, std::vector<float>& y, const std::vector<int>& sizes) {
  y.assign(x.size(), 0.0f);
  std::size_t pos = 0, k = 0;
  while (pos < x.size()) {
    const auto n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(sizes[k++ % sizes.size()]), x.size() - pos));
    e.process(x.data() + pos, y.data() + pos, n);
    pos += static_cast<std::size_t>(n);
  }
}

std::vector<int> randomSizes(unsigned seed, int count = 97, int maxSize = 4096) {
  std::mt19937 g(seed);
  std::vector<int> s;
  for (int i = 0; i < count; ++i) s.push_back(1 + static_cast<int>(g() % static_cast<unsigned>(maxSize)));
  return s;
}

ParamValues defaults() {
  ParamValues v{};
  for (int i = 0; i < kNumParams; ++i) v[static_cast<std::size_t>(i)] = paramSpec(i).def;
  return v;
}

const double kHostRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 192000.0};

}  // namespace

TEST_CASE("Engine: reported latency equals the measured impulse delay", "[engine][latency]") {
  for (const int stub : {0, 100, 301}) {
    for (const double host : kHostRates) {
      CAPTURE(stub, host);
      auto e = Engine::build(identityPreset(stub), host, 512);
      REQUIRE(e->modelRate() == 48000.0);
      const int T = e->latencySamples();
      CHECK(e->latency().chainModelSamples == stub);
      if (host == 48000.0) CHECK(T == stub);  // no converters
      else CHECK(T > stub / 2);

      for (const int k0 : {700, 701, 1234}) {  // different input phases: the delay must not move
        e = Engine::build(identityPreset(stub), host, 512);
        std::vector<float> x(8000, 0.0f), y;
        x[static_cast<std::size_t>(k0)] = 1.0f;
        runEngine(*e, x, y, randomSizes(static_cast<unsigned>(k0)));
        const auto peak = std::max_element(y.begin(), y.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) - y.begin();
        CHECK(peak - k0 == T);
        CHECK(e->underruns() == 0);
      }
    }
  }
}

TEST_CASE("Engine: 44.1 kHz host with 48 kHz models - latency breakdown", "[engine][latency]") {
  auto e = Engine::build(identityPreset(0), 44100.0, 512);
  const auto& l = e->latency();
  CHECK(l.resampling);
  CHECK(l.chainModelSamples == 0);
  // Two converters of ~46 samples each at their own input rate: about 1 ms + 1 ms.
  CHECK(l.total > 80);
  CHECK(l.total < 100);
  WARN("44.1 kHz host / 48 kHz models: total latency " << l.total << " host samples ("
                                                      << 1000.0 * l.total / 44100.0 << " ms)");
  auto e2 = Engine::build(identityPreset(0), 96000.0, 512);
  WARN("96 kHz host / 48 kHz models: total latency " << e2->latency().total << " host samples");
}

TEST_CASE("Engine: output is the input delayed by the latency (in-band signal)", "[engine]") {
  for (const double host : kHostRates) {
    CAPTURE(host);
    auto e = Engine::build(identityPreset(128), host, 256);
    const std::size_t n = static_cast<std::size_t>(host * 0.4);
    std::vector<float> x(n, 0.0f);
    const double fs = host;
    const double freqs[] = {110.0, 1000.0, 5000.0, 12000.0, 18000.0};
    for (std::size_t i = 0; i < n; ++i)
      for (double f : freqs) x[i] += static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * f * static_cast<double>(i) / fs));
    std::vector<float> y;
    runEngine(*e, x, y, randomSizes(11, 61, 2048));
    const auto T = static_cast<std::size_t>(e->latencySamples());
    double err = 0.0, sig = 0.0;
    for (std::size_t j = T + 3000; j < n; ++j) {
      const double d = static_cast<double>(y[j]) - static_cast<double>(x[j - T]);
      err += d * d;
      sig += static_cast<double>(x[j - T]) * static_cast<double>(x[j - T]);
    }
    CHECK(10.0 * std::log10(err / sig) < -60.0);  // < -60 dB error: the delay is exact, the passband flat
    CHECK(e->underruns() == 0);
  }
}

TEST_CASE("Engine: output does not depend on the host block sizes", "[engine]") {
  for (const double host : {44100.0, 96000.0}) {
    CAPTURE(host);
    const auto x = noise(30000, 3, 0.4f);
    auto e1 = Engine::build(identityPreset(40), host, 512);
    auto e2 = Engine::build(identityPreset(40), host, 512);
    auto e3 = Engine::build(identityPreset(40), host, 4096);
    std::vector<float> a, b, c;
    runEngine(*e1, x, a, {1});
    runEngine(*e2, x, b, randomSizes(5, 40, 3000));
    runEngine(*e3, x, c, {4096, 17});
    double d1 = 0, d2 = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      d1 = std::max(d1, static_cast<double>(std::fabs(a[i] - b[i])));
      d2 = std::max(d2, static_cast<double>(std::fabs(a[i] - c[i])));
    }
    CHECK(d1 < 1e-5);
    CHECK(d2 < 1e-5);
    CHECK(e1->underruns() + e2->underruns() + e3->underruns() == 0);
  }
}

TEST_CASE("Engine: process() and setParams() allocate nothing and take no locks", "[engine][rt]") {
  for (const double host : {44100.0, 48000.0, 96000.0}) {
    CAPTURE(host);
    auto e = Engine::build(identityPreset(64), host, 512);
    const auto x = noise(4096, 1, 0.5f);
    std::vector<float> y(4096);
    std::mt19937 g(7);
    ParamValues v = defaults();
    e->process(x.data(), y.data(), 512);  // warm up
    long allocs = 0, locks = 0;
    for (int i = 0; i < 300; ++i) {
      const int n = 1 + static_cast<int>(g() % 4096);  // beyond the prepared 512: chunked internally
      v[kInputGain] = std::sin(i * 0.1) * 6.0;
      v[kOutputGain] = std::cos(i * 0.07) * 6.0;
      v[kBlend] = 0.5 + 0.4 * std::sin(i * 0.05);
      v[kPostEqFirst] = 9.0 * std::sin(i * 0.11);
      AllocGuard ag;
      LockGuard lg;
      e->setParams(v);
      e->process(x.data(), y.data(), n);
      allocs += ag.count();
      locks += lg.count();
    }
    CHECK(allocs == 0);
    if (LockGuard::enabled()) CHECK(locks == 0);
    CHECK(e->underruns() == 0);
  }
}

TEST_CASE("Engine: parameters map onto the preset", "[engine][params]") {
  // Post-EQ slots follow the gain-bearing bands in order, skipping filters.
  Preset p = identityPreset(0);
  p.postEq = {EqBand{EqType::HighPass, 70.0, 0.0, 0.7, true}, EqBand{EqType::Peak, 1500.0, 2.0, 1.2, true},
              EqBand{EqType::LowPass, 9000.0, 0.0, 0.7, true}, EqBand{EqType::HighShelf, 6000.0, -3.0, 0.7, true}};
  const SlotBands s = postEqSlotBands(p);
  CHECK(s[0] == 1);
  CHECK(s[1] == 3);
  CHECK(s[2] == -1);
  ParamValues v = paramsFromPreset(p);
  CHECK(v[kPostEqFirst] == 2.0);
  CHECK(v[kPostEqFirst + 1] == -3.0);
  v[kPostEqFirst + 1] = 4.5;
  v[kBlend] = 0.25;
  applyParams(p, v);
  CHECK(p.postEq[3].gainDb == 4.5);
  CHECK(p.blend == 0.25);
  // Out-of-range preset values are clamped consistently.
  p.inputGainDb = 40.0;
  CHECK(clampedToParams(p).inputGainDb == 24.0);
}
