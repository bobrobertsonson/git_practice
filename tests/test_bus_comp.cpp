#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "alloc_guard.h"
#include "sawblade/bus_comp.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;

namespace {

// Steady-state gain in dB for a sine of the given peak amplitude: RMS ratio over the last second.
double measureGainDb(const BusCompParams& p, double amp, double freq = 997.0, double fs = 48000.0) {
  BusCompressor c;
  c.setParams(p);
  c.prepare({fs, 512});
  auto x = sine(freq, fs, static_cast<std::size_t>(fs * 3), amp);
  const auto in = x;
  processChunked(c, x, {512});
  const auto n = static_cast<std::size_t>(fs);
  const std::size_t off = x.size() - n;
  return toDb(rms(x.data() + off, n) / rms(in.data() + off, n));
}

}  // namespace

TEST_CASE("BusCompressor static curve: hard knee", "[buscomp]") {
  BusCompParams p;
  p.thresholdDb = -20.0; p.ratio = 4.0; p.kneeDb = 0.0; p.attackMs = 1.0; p.releaseMs = 50.0;
  const double levelDb = -6.0;  // sine peak 0.5
  const double expected = (p.thresholdDb + (levelDb - p.thresholdDb) / p.ratio) - levelDb;  // -10.5 dB
  REQUIRE(expected == Catch::Approx(-10.5));
  REQUIRE(measureGainDb(p, std::pow(10.0, levelDb / 20.0)) == Catch::Approx(expected).margin(0.2));
  // Below the threshold: unity.
  REQUIRE(measureGainDb(p, 0.05) == Catch::Approx(0.0).margin(0.2));
}

TEST_CASE("BusCompressor static curve: soft knee and makeup", "[buscomp]") {
  BusCompParams p;
  p.thresholdDb = -12.0; p.ratio = 2.0; p.kneeDb = 6.0; p.attackMs = 1.0; p.releaseMs = 50.0;
  // Inside the knee (level -10 dB, over = +2, knee 6): slope * (over + knee/2)^2 / (2 knee).
  const double inKnee = (1.0 / 2.0 - 1.0) * 25.0 / 12.0;
  REQUIRE(BusCompressor::staticGainDb(p, -10.0) == Catch::Approx(inKnee));
  REQUIRE(BusCompressor::staticGainDb(p, -20.0) == 0.0);              // below the knee
  REQUIRE(BusCompressor::staticGainDb(p, 0.0) == Catch::Approx(-6.0));  // above: (1/R - 1) * over
  REQUIRE(measureGainDb(p, std::pow(10.0, -10.0 / 20.0)) == Catch::Approx(inKnee).margin(0.2));
  REQUIRE(measureGainDb(p, 1.0) == Catch::Approx(-6.0).margin(0.2));

  p.makeupDb = 6.0;
  REQUIRE(measureGainDb(p, 0.01) == Catch::Approx(6.0).margin(0.05));  // far below threshold: makeup only
  REQUIRE(measureGainDb(p, 1.0) == Catch::Approx(0.0).margin(0.2));    // -6 + 6
}

TEST_CASE("BusCompressor attack/release behave and block size does not matter", "[buscomp]") {
  BusCompParams p;
  p.thresholdDb = -30.0; p.ratio = 8.0; p.kneeDb = 0.0; p.attackMs = 5.0; p.releaseMs = 120.0;
  auto x = noise(48000, 3, 0.4f);
  auto y1 = x, y2 = x;
  BusCompressor a, b;
  a.setParams(p); b.setParams(p);
  a.prepare({48000.0, 1000}); b.prepare({48000.0, 1000});
  processChunked(a, y1, {1});
  processChunked(b, y2, {333, 64, 1000});
  for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(y1[i] == y2[i]);
}

TEST_CASE("BusCompressor does not allocate in process()", "[buscomp][alloc]") {
  BusCompressor c;
  c.prepare({48000.0, 512});
  auto x = noise(512, 9);
  AllocGuard g;
  for (int i = 0; i < 20; ++i) c.process(x.data(), 100 + 20 * (i % 20));
  REQUIRE(g.count() == 0);
}
