#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/delay.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;

TEST_CASE("DelayLine delays an impulse by exactly n samples", "[delay]") {
  constexpr int kMax = 100;
  for (const int n : {0, 1, 7, kMax}) {
    DYNAMIC_SECTION("n = " << n) {
      DelayLine d(kMax);
      d.prepare({48000.0, 64});
      d.setDelaySamples(n);
      std::vector<float> x(400, 0.0f);
      x[3] = 1.0f;
      processChunked(d, x, {64, 37, 1});  // also exercises ring wrap across block boundaries
      for (std::size_t i = 0; i < x.size(); ++i)
        REQUIRE(x[i] == (i == static_cast<std::size_t>(3 + n) ? 1.0f : 0.0f));
    }
  }
}

TEST_CASE("DelayLine clamps, resets and can change delay without allocating", "[delay][alloc]") {
  DelayLine d(32);
  d.prepare({48000.0, 128});
  d.setDelaySamples(1000);
  REQUIRE(d.delaySamples() == 32);
  d.setDelaySamples(-5);
  REQUIRE(d.delaySamples() == 0);

  d.setDelaySamples(10);
  std::vector<float> x = noise(256);
  d.process(x.data(), 128);
  d.reset();
  std::vector<float> imp(64, 0.0f);
  imp[0] = 1.0f;
  d.process(imp.data(), 64);
  REQUIRE(imp[10] == 1.0f);
  REQUIRE(imp[0] == 0.0f);  // reset cleared the earlier noise

  std::vector<float> buf = noise(1024);
  const int sizes[] = {1, 7, 64, 128, 2, 99};
  long allocs = -1;
  {
    AllocGuard g;
    int k = 0;
    for (int rep = 0; rep < 4; ++rep)
      for (const int n : sizes) {
        d.setDelaySamples((k++ * 5) % 33);
        d.process(buf.data(), n);
      }
    d.reset();
    allocs = g.count();
  }
  REQUIRE(allocs == 0);
}

TEST_CASE("Gain applies dB gain exactly", "[delay][gain]") {
  Gain g;
  g.setGainDb(6.0);
  g.prepare({48000.0, 64});
  std::vector<float> x = {1.0f, -0.5f, 0.25f};
  g.process(x.data(), 3);
  const float lin = std::pow(10.0f, 6.0f / 20.0f);
  REQUIRE(x[0] == Catch::Approx(lin).epsilon(1e-6));
  REQUIRE(x[1] == Catch::Approx(-0.5f * lin).epsilon(1e-6));
  g.setGainDb(0.0);
  std::vector<float> y = {0.3f, -0.7f};
  g.process(y.data(), 2);
  REQUIRE(y[0] == 0.3f);
  REQUIRE(y[1] == -0.7f);
}
