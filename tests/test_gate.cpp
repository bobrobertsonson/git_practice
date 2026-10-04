#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/gate.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;

namespace {

constexpr double kFs = 48000.0;

std::size_t ms(double t) { return static_cast<std::size_t>(std::llround(t * 0.001 * kFs)); }
double dbfsToPeak(double db) { return std::pow(10.0, db / 20.0); }

// Sine with the given peak level in dBFS. 1 kHz.
std::vector<float> tone(double peakDb, std::size_t n) { return sine(1000.0, kFs, n, dbfsToPeak(peakDb)); }

// Per-sample gain of output relative to a self-keyed input, via sample-wise ratio at peaks is
// awkward; instead run the gate on a DC-like constant keyed signal: use a separate constant io.
// processKeyed lets us key on the tone while passing a constant 1.0 through, so out[i] == gain.
std::vector<float> gainTrace(Gate& g, const std::vector<float>& key) {
  std::vector<float> io(key.size(), 1.0f);
  g.processKeyed(key.data(), io.data(), static_cast<int>(key.size()));
  return io;
}

double dbOf(float lin) { return 20.0 * std::log10(std::max(1e-12, static_cast<double>(lin))); }

Gate makeGate(const GateParams& p) {
  Gate g;
  g.setParams(p);
  g.prepare({kFs, 4096});
  return g;
}

}  // namespace

TEST_CASE("Gate passes a loud signal at unity after attack", "[gate]") {
  GateParams p;  // -55 dB threshold, 0.5 ms attack
  Gate g = makeGate(p);
  const auto key = tone(-20.0, ms(300));
  const auto gain = gainTrace(g, key);
  // After the attack (allow 10 attack time constants of the dB-domain smoother, + env rise).
  for (std::size_t i = ms(50); i < gain.size(); ++i) REQUIRE(std::fabs(dbOf(gain[i])) < 0.1);

  // Self-keyed process on the actual signal: transparent after attack.
  Gate g2 = makeGate(p);
  std::vector<float> x = key, y = key;
  g2.process(y.data(), static_cast<int>(y.size()));
  for (std::size_t i = ms(50); i < x.size(); ++i) REQUIRE(y[i] == x[i]);
}

namespace {

// Time for the envelope (peak follower, release tau = kEnvReleaseMs) to fall from peakDb to
// the close threshold derived from the params.
double envFallMs(const GateParams& p, double peakDb) {
  const double peakLin = dbfsToPeak(peakDb);
  const double closeLin = dbfsToPeak(p.thresholdDb - p.hysteresisDb);
  return Gate::kEnvReleaseMs * std::log(peakLin / closeLin);
}

}  // namespace

TEST_CASE("Gate closes to rangeDb after the key drops", "[gate]") {
  for (const double preDb : {-45.0, -20.0}) {
    DYNAMIC_SECTION("pre-drop level " << preDb << " dBFS") {
      GateParams p;
      Gate g = makeGate(p);
      std::vector<float> key = tone(preDb, ms(300));
      const std::size_t dropAt = key.size();
      const auto low = tone(-80.0, ms(800));
      key.insert(key.end(), low.begin(), low.end());
      const auto gain = gainTrace(g, key);

      REQUIRE(std::fabs(dbOf(gain[dropAt - 1])) < 0.1);  // still open one sample before the drop
      // Envelope fall time + hold + 5 release time constants. The envelope is slightly below the
      // tone's peak at the drop, so it crosses the close threshold marginally early: the bound
      // is conservative and needs no extra margin.
      const double boundMs = envFallMs(p, preDb) + p.holdMs + 5.0 * p.releaseMs;
      const std::size_t deadline = dropAt + ms(boundMs);
      REQUIRE(std::fabs(dbOf(gain[deadline]) - p.rangeDb) <= 1.0);
      REQUIRE(std::fabs(dbOf(gain.back()) - p.rangeDb) <= 1.0);
    }
  }
}

TEST_CASE("Gate hysteresis keeps it open between close and open thresholds", "[gate]") {
  // Open at -55, close below -61. Hold is ~0 so only hysteresis can keep the gate open through
  // 50 ms stretches at threshold - 3 dB (long enough to decay the 10 ms envelope well below
  // the open threshold, but not below the close threshold).
  auto run = [](double hysteresisDb) {
    GateParams p;
    p.holdMs = 0.0;
    p.hysteresisDb = hysteresisDb;
    Gate g = makeGate(p);
    std::vector<float> key = tone(-30.0, ms(100));  // open it
    for (int k = 0; k < 10; ++k) {
      const auto hi = tone(p.thresholdDb + 1.0, ms(10));
      const auto lo = tone(p.thresholdDb - 3.0, ms(50));
      key.insert(key.end(), hi.begin(), hi.end());
      key.insert(key.end(), lo.begin(), lo.end());
    }
    return gainTrace(g, key);
  };

  SECTION("hysteresis 6 dB: stays open") {
    const auto gain = run(6.0);
    for (std::size_t i = ms(50); i < gain.size(); ++i) REQUIRE(std::fabs(dbOf(gain[i])) < 0.1);
  }
  SECTION("negative control, hysteresis 0 dB: closes") {
    const auto gain = run(0.0);
    const auto minGain = *std::min_element(gain.begin() + static_cast<std::ptrdiff_t>(ms(50)), gain.end());
    REQUIRE(dbOf(minGain) < -3.0);
  }
}

TEST_CASE("Gate honours hold", "[gate]") {
  GateParams p;
  p.holdMs = 50.0;
  Gate g = makeGate(p);
  constexpr double kPreDb = -45.0;
  std::vector<float> key = tone(kPreDb, ms(300));
  const std::size_t dropAt = key.size();
  const auto low = tone(-90.0, ms(400));
  key.insert(key.end(), low.begin(), low.end());

  // Step through sample by sample so we can look at the open state itself.
  std::vector<float> io(key.size(), 1.0f);
  std::size_t closedAt = 0;
  for (std::size_t i = 0; i < key.size(); ++i) {
    g.processKeyed(&key[i], &io[i], 1);
    if (i >= dropAt && !g.isOpen() && closedAt == 0) closedAt = i;
  }
  REQUIRE(closedAt != 0);
  const double fallMs = envFallMs(p, kPreDb);
  // Open (unity) 1 ms before envelope fall + hold has elapsed; closed by a few ms after it.
  // (The envelope sits marginally below the tone peak at the drop, so closing can come a
  // fraction of a ms early; the 1 ms / 3 ms windows absorb that.)
  REQUIRE(closedAt - dropAt >= ms(fallMs + p.holdMs - 1.0));
  REQUIRE(std::fabs(dbOf(io[dropAt + ms(fallMs + p.holdMs - 1.0)])) < 0.1);
  REQUIRE(closedAt - dropAt <= ms(fallMs + p.holdMs + 3.0));
}

TEST_CASE("Gate disabled is bit-transparent; reset closes it", "[gate]") {
  GateParams p;
  p.enabled = false;
  Gate g = makeGate(p);
  std::vector<float> x = noise(1000, 3, 0.001f), y = x;
  g.process(y.data(), 1000);
  REQUIRE(x == y);

  p.enabled = true;
  g.setParams(p);
  const auto key = tone(-20.0, ms(50));
  gainTrace(g, key);
  REQUIRE(g.isOpen());
  g.reset();
  REQUIRE_FALSE(g.isOpen());
  REQUIRE(g.gainDb() == p.rangeDb);
}

TEST_CASE("Gate output is independent of block size", "[gate]") {
  Gate a = makeGate(GateParams{});
  Gate b = makeGate(GateParams{});
  std::vector<float> x = noise(ms(500), 9, 0.2f);
  for (std::size_t i = ms(250); i < x.size(); ++i) x[i] *= 1e-4f;  // second half quiet
  std::vector<float> y1 = x, y2 = x;
  a.process(y1.data(), static_cast<int>(y1.size()));
  processChunked(b, y2, {1, 64, 333, 7});
  REQUIRE(y1 == y2);
}

TEST_CASE("Gate process performs no allocations", "[gate][alloc]") {
  Gate g = makeGate(GateParams{});
  std::vector<float> buf = noise(1024, 5, 0.1f);
  std::vector<float> key = noise(1024, 6, 0.1f);
  const int sizes[] = {1, 7, 64, 333, 1024, 128, 2, 512, 17, 1000, 256};
  long allocs = -1;
  {
    AllocGuard guard;
    for (const int n : sizes) {
      g.process(buf.data(), n);
      g.processKeyed(key.data(), buf.data(), n);
    }
    // reset() is checked here as an implementation property, not an interface contract.
    g.reset();
    allocs = guard.count();
  }
  REQUIRE(allocs == 0);
}

// ---- Phase 3.5: expander mode, key high-pass, linear-dB release ----

namespace {

GateParams expanderParams() {
  GateParams p;
  p.mode = GateMode::Expander;
  p.ratio = 4.0;
  p.thresholdDb = -50.0;
  p.hysteresisDb = 6.0;  // close threshold -56 dBFS
  p.holdMs = 0.0;
  p.rangeDb = -60.0;
  p.releaseMs = 5.0;
  p.attackMs = 0.5;
  return p;
}

}  // namespace

TEST_CASE("Expander static curve matches the analytic law", "[gate][expander]") {
  const GateParams p = expanderParams();
  const double closeDb = p.thresholdDb - p.hysteresisDb;
  // Constant (DC) key levels below the close threshold give a constant envelope. Start open so
  // the state machine passes through the close path.
  for (const double envDb : {-58.0, -62.0, -70.0, -80.0, -90.0}) {
    Gate g = makeGate(p);
    std::vector<float> key(ms(100), 0.1f);
    key.insert(key.end(), ms(1500), static_cast<float>(dbfsToPeak(envDb)));
    const auto gain = gainTrace(g, key);
    const double expected = std::max(p.rangeDb, -(p.ratio - 1.0) * (closeDb - envDb));
    INFO("env " << envDb << " dB");
    REQUIRE(std::fabs(dbOf(gain.back()) - expected) <= 0.1);
  }
  SECTION("above the close threshold: unity") {
    Gate g = makeGate(p);
    std::vector<float> key(ms(100), 0.1f);
    key.insert(key.end(), ms(500), static_cast<float>(dbfsToPeak(closeDb + 1.0)));
    REQUIRE(std::fabs(dbOf(gainTrace(g, key).back())) < 0.1);
  }
  SECTION("ratio 8 expands harder") {
    GateParams q = p;
    q.ratio = 8.0;
    Gate g = makeGate(q);
    std::vector<float> key(ms(100), 0.1f);
    key.insert(key.end(), ms(1500), static_cast<float>(dbfsToPeak(-58.0)));
    REQUIRE(std::fabs(dbOf(gainTrace(g, key).back()) - (-14.0)) <= 0.1);
  }
}

namespace {

// Mean (over the second half) of the expander gain in dB for a given key, ratio 2 and a close
// threshold of 0 dBFS, so gainDb = envDb exactly (never limited by the range).
double meanExpanderGainDb(const std::vector<float>& key, double hpHz) {
  GateParams p;
  p.mode = GateMode::Expander;
  p.ratio = 2.0;
  p.thresholdDb = 0.0;
  p.hysteresisDb = 0.0;
  p.holdMs = 0.0;
  p.rangeDb = -120.0;
  p.releaseMs = 0.05;  // follows the target nearly instantly
  p.attackMs = 0.01;
  p.keyHighPassHz = hpHz;
  Gate g = makeGate(p);
  std::vector<float> io(key.size(), 1.0f);
  g.processKeyed(key.data(), io.data(), static_cast<int>(key.size()));
  double sum = 0.0;
  std::size_t n = 0;
  for (std::size_t i = key.size() / 2; i < io.size(); ++i, ++n) sum += dbOf(io[i]);
  return sum / static_cast<double>(n);
}

}  // namespace

TEST_CASE("Key high-pass: 12 dB/oct magnitude at 3 points, audio untouched", "[gate][expander]") {
  const double fc = 120.0;
  // The reference runs the same peak-follower on a pure sine of the analytic filtered amplitude
  // (no filter), so envelope ripple cancels and only the filter magnitude is under test.
  for (const double f : {60.0, 120.0, 480.0}) {
    DYNAMIC_SECTION("tone at " << f << " Hz") {
      const double peak = 0.2;
      const double mag = 1.0 / std::sqrt(1.0 + std::pow(fc / f, 4.0));  // 2nd-order Butterworth
      const double measured = meanExpanderGainDb(sine(f, kFs, ms(3000), peak), fc);
      const double reference = meanExpanderGainDb(sine(f, kFs, ms(3000), peak * mag), 0.0);
      REQUIRE(std::fabs(measured - reference) <= 0.15);
    }
  }
  SECTION("audio path is not filtered") {
    GateParams q;
    q.keyHighPassHz = 400.0;
    q.thresholdDb = -120.0;  // always open
    Gate g = makeGate(q);
    const std::vector<float> x = sine(50.0, kFs, ms(300), 0.5);
    std::vector<float> y = x;
    g.process(y.data(), static_cast<int>(y.size()));
    for (std::size_t i = ms(50); i < x.size(); ++i) REQUIRE(y[i] == x[i]);
  }
}

TEST_CASE("Linear-dB release falls at constant dB per ms", "[gate][expander]") {
  GateParams p;
  p.releaseCurve = GateReleaseCurve::LinearDb;
  p.releaseMs = 150.0;
  p.rangeDb = -50.0;
  p.holdMs = 0.0;
  Gate g = makeGate(p);
  std::vector<float> key = tone(-20.0, ms(200));
  const std::size_t dropAt = key.size();
  const auto low = tone(-90.0, ms(600));
  key.insert(key.end(), low.begin(), low.end());
  const auto gain = gainTrace(g, key);
  std::size_t start = dropAt;
  while (start < gain.size() && dbOf(gain[start]) > -0.5) ++start;
  const double expectedSlope = -p.rangeDb / p.releaseMs;  // dB per ms (magnitude)
  const std::size_t a = start + ms(30), b = start + ms(110);
  const double slope = (dbOf(gain[a]) - dbOf(gain[b])) / (static_cast<double>(b - a) / (kFs * 0.001));
  REQUIRE(std::fabs(slope - expectedSlope) <= 0.05 * expectedSlope);
  REQUIRE(std::fabs(dbOf(gain.back()) - p.rangeDb) <= 0.01);
  std::size_t end = start;
  while (end < gain.size() && dbOf(gain[end]) > p.rangeDb + 0.05) ++end;
  REQUIRE(std::fabs(static_cast<double>(end - start) / (kFs * 0.001) - p.releaseMs) <= 0.05 * p.releaseMs + 1.0);
}

TEST_CASE("Expander output is independent of block size", "[gate][expander]") {
  GateParams p = expanderParams();
  p.keyHighPassHz = 120.0;
  p.releaseCurve = GateReleaseCurve::LinearDb;
  Gate a = makeGate(p);
  Gate b = makeGate(p);
  std::vector<float> x = noise(ms(500), 11, 0.2f);
  for (std::size_t i = ms(250); i < x.size(); ++i) x[i] *= 1e-4f;
  std::vector<float> y1 = x, y2 = x;
  a.process(y1.data(), static_cast<int>(y1.size()));
  processChunked(b, y2, {1, 64, 333, 7});
  REQUIRE(y1 == y2);
}

TEST_CASE("Expander process performs no allocations", "[gate][alloc][expander]") {
  GateParams p = expanderParams();
  p.keyHighPassHz = 120.0;
  p.releaseCurve = GateReleaseCurve::LinearDb;
  Gate g = makeGate(p);
  std::vector<float> buf = noise(1024, 5, 0.1f);
  std::vector<float> key = noise(1024, 6, 0.01f);
  const int sizes[] = {1, 7, 64, 333, 1024, 128, 2, 512};
  long allocs = -1;
  {
    AllocGuard guard;
    for (const int n : sizes) {
      g.process(buf.data(), n);
      g.processKeyed(key.data(), buf.data(), n);
    }
    g.setParams(p);
    g.reset();
    allocs = guard.count();
  }
  REQUIRE(allocs == 0);
}
