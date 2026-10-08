// Task H.1: the peak-detector floor (peakFloorDb, shared with the matcher) and the live follower's peak-envelope statistic.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "sawblade/gate.h"

using namespace sawblade;

namespace {

constexpr double kFs = 48000.0;

// Gaussian white noise at a set RMS (dBFS): the DI noise floor is Gaussian-like, its peak envelope sits ~10 dB above the RMS.
std::vector<float> gaussian(double rmsDb, std::size_t n, std::uint32_t seed) {
  std::uint32_t s = seed;
  const auto u = [&s] {
    s = s * 1664525u + 1013904223u;
    return (static_cast<double>(s >> 8) + 0.5) / 16777216.0;
  };
  const double amp = std::pow(10.0, rmsDb / 20.0);
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; i += 2) {
    const double r = std::sqrt(-2.0 * std::log(u())), t = 2.0 * 3.14159265358979323846 * u();
    x[i] = static_cast<float>(amp * r * std::cos(t));
    if (i + 1 < n) x[i + 1] = static_cast<float>(amp * r * std::sin(t));
  }
  return x;
}

// The gate's own peak envelope in dBFS (same detector as Gate / peakFloorDb).
std::vector<double> peakEnvDb(const std::vector<float>& x) {
  const double atk = std::exp(-1.0 / (Gate::kEnvAttackMs * 0.001 * kFs)), rel = std::exp(-1.0 / (Gate::kEnvReleaseMs * 0.001 * kFs));
  std::vector<double> e(x.size());
  double env = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double a = std::fabs(static_cast<double>(x[i]));
    env = a > env ? a + atk * (env - a) : a + rel * (env - a);
    e[i] = 20.0 * std::log10(std::max(env, 1e-12));
  }
  return e;
}

Gate makeGate(const GateParams& p) {
  Gate g;
  g.setParams(p);
  g.prepare({kFs, 4096});
  return g;
}

// Per-sample gain of the gate keyed on `key` (a constant 1.0 passes through).
std::vector<float> gains(Gate& g, const std::vector<float>& key) {
  std::vector<float> io(key.size(), 1.0f);
  g.processKeyed(key.data(), io.data(), static_cast<int>(key.size()));
  return io;
}
double gainDbAt(const std::vector<float>& g, std::size_t i) { return 20.0 * std::log10(std::max(1e-12, static_cast<double>(g[i]))); }

// Fraction of samples in [from, to) whose gain is <= rangeDb + 1.
double closedFraction(const std::vector<float>& g, std::size_t from, std::size_t to, double rangeDb) {
  std::size_t n = 0;
  for (std::size_t i = from; i < to; ++i)
    if (gainDbAt(g, i) <= rangeDb + 1.0) ++n;
  return static_cast<double>(n) / static_cast<double>(to - from);
}

// The record gate the matcher's default cell builds: open = peakFloor + 10, hysteresis 6 (close = peakFloor + 4).
GateParams recordCell(double peakFloor) {
  GateParams g;
  g.enabled = true;
  g.thresholdDb = peakFloor + 10.0;
  g.hysteresisDb = 6.0;
  return g;
}

// The live set core derives for a match preset (without the key high-pass, so key = audio in these tests).
GateParams liveGate(double ratio = 4.0, double rangeDb = -40.0) {
  GateParams g;
  g.enabled = true;
  g.thresholdMode = GateThresholdMode::FloorRelative;
  g.floorOffsetDb = 10.0;
  g.mode = GateMode::Expander;
  g.ratio = ratio;
  g.rangeDb = rangeDb;
  g.holdMs = 40.0;
  g.releaseMs = 120.0;
  return g;
}

}  // namespace

TEST_CASE("peakFloorDb: percentile of the gate's peak envelope over the masked samples", "[gate][peakfloor]") {
  const auto n = gaussian(-60.0, static_cast<std::size_t>(5 * kFs), 11);
  const double p = peakFloorDb(n, kFs);
  std::vector<double> sorted = peakEnvDb(n);
  std::sort(sorted.begin(), sorted.end());
  const double want = sorted[static_cast<std::size_t>(std::floor(0.925 * static_cast<double>(sorted.size() - 1) + 0.5))];
  CHECK(std::fabs(p - want) < 1e-9);
  CHECK(p > -60.0 + 5.0);  // a peak detector reads well above the RMS of noise

  // The mask selects: a loud first part is excluded when only the quiet second part is masked.
  std::vector<float> x = gaussian(-20.0, static_cast<std::size_t>(2 * kFs), 3);
  const auto quiet = gaussian(-60.0, static_cast<std::size_t>(2 * kFs), 4);
  x.insert(x.end(), quiet.begin(), quiet.end());
  std::vector<std::uint8_t> mask(x.size(), 0);
  std::fill(mask.begin() + static_cast<std::ptrdiff_t>(2.5 * kFs), mask.end(), 1);
  CHECK(peakFloorDb(x, kFs, 0.0, &mask) < -40.0);
  CHECK(peakFloorDb(x, kFs) > -30.0);
  std::fill(mask.begin(), mask.end(), 0);
  CHECK(std::isnan(peakFloorDb(x, kFs, 0.0, &mask)));

  // The key high-pass is applied: a 30 Hz tone is removed by an 80 Hz high-pass.
  std::vector<float> hum(static_cast<std::size_t>(2 * kFs));
  for (std::size_t i = 0; i < hum.size(); ++i)
    hum[i] = static_cast<float>(0.1 * std::sin(2.0 * 3.14159265358979 * 30.0 * static_cast<double>(i) / kFs));
  CHECK(peakFloorDb(hum, kFs, 80.0) < peakFloorDb(hum, kFs) - 10.0);
}

TEST_CASE("Noise alone: the matcher's default cell stays closed (> 95 % after 0.5 s) and the live gate attenuates", "[gate][peakfloor]") {
  const auto begin = static_cast<std::size_t>(0.5 * kFs);
  // Record gate, default cell, a -49.5 dBFS RMS floor (the user's L DI): old (RMS + 4) vs new (peak floor + 10) open / close.
  const auto noise = gaussian(-49.5, static_cast<std::size_t>(20 * kFs), 21);
  const double rmsDb = -49.5, pk = peakFloorDb(noise, kFs);
  GateParams oldCell;  // open = RMS floor + 4 dB, hysteresis 6 dB
  oldCell.enabled = true;
  oldCell.thresholdDb = rmsDb + 4.0;
  oldCell.hysteresisDb = 6.0;
  const GateParams newCell = recordCell(pk);
  Gate go = makeGate(oldCell), gn = makeGate(newCell);
  const double fracOld = closedFraction(gains(go, noise), begin, noise.size(), oldCell.rangeDb);
  const double fracNew = closedFraction(gains(gn, noise), begin, noise.size(), newCell.rangeDb);
  WARN("REPORT noise floor -49.5 dBFS RMS, peak floor " << pk << " dBFS (" << (pk - rmsDb) << " dB above the RMS)\n"
       << "  old cell: open " << oldCell.thresholdDb << " close " << oldCell.thresholdDb - oldCell.hysteresisDb << " dBFS, closed "
       << 100.0 * fracOld << " % of the time\n"
       << "  new cell: open " << newCell.thresholdDb << " close " << newCell.thresholdDb - newCell.hysteresisDb << " dBFS, closed "
       << 100.0 * fracNew << " % of the time");
  CHECK(fracNew > 0.95);

  // Live gate (floorRelative +10, expander 4:1, range -40): the follower seeds at -70 dBFS. A floor whose peak envelope is above
  // seed + 20 dB is only learned through the 10 s / +1 dB/s leak, so a -49.5 dBFS RMS floor is attenuated after ~25 s; quieter
  // ones at once. An expander does not "close": its gain on noise is -(ratio - 1) * (close - envelope), -(4-1) x (close - envelope), ~10 dB at the floor.
  const auto liveStats = [&](double db, double seconds, std::size_t from, const char* label) {
    const auto q = gaussian(db, static_cast<std::size_t>(seconds * kFs), 22);
    Gate g = makeGate(liveGate());
    const auto gl = gains(g, q);
    std::vector<double> gdb;
    for (std::size_t i = from; i < gl.size(); i += 16) gdb.push_back(gainDbAt(gl, i));
    std::sort(gdb.begin(), gdb.end());
    const double med = gdb[gdb.size() / 2], p95 = gdb[gdb.size() * 95 / 100];
    WARN("REPORT live gate on " << label << ": floor estimate " << g.floorEstimateDb() << " dBFS, open " << g.openThresholdDb()
         << " close " << g.openThresholdDb() - 6.0 << " dBFS; gain on the noise: median " << med << " dB, 95th percentile (loudest) " << p95 << " dB");
    return med;
  };
  CHECK(liveStats(-49.5, 60.0, static_cast<std::size_t>(40 * kFs), "a -49.5 dBFS RMS floor (steady state after 40 s)") <= -10.0);
  CHECK(liveStats(-75.0, 8.0, begin, "a -75 dBFS RMS floor") <= -10.0);
  CHECK(liveStats(-65.0, 8.0, begin, "a -65 dBFS RMS floor") <= -10.0);
}

TEST_CASE("Decay tail: no attenuation while the note's envelope is > 12 dB above the peak floor", "[gate][peakfloor]") {
  const double floorRms = -70.0;
  const std::size_t pre = static_cast<std::size_t>(4 * kFs), len = static_cast<std::size_t>(5 * kFs);
  const auto floorNoise = gaussian(floorRms, pre + len, 31);
  const double pk = peakFloorDb(std::vector<float>(floorNoise.begin(), floorNoise.begin() + static_cast<std::ptrdiff_t>(pre)), kFs);
  // A plucked note: -12 dBFS peak, exponential decay of 60 dB in 1.5 s, starting after the floor has been learned.
  std::vector<float> x = floorNoise;
  const double tau = 1.5 / (60.0 / 8.685889638);
  for (std::size_t i = 0; i < len; ++i) {
    const double t = static_cast<double>(i) / kFs;
    x[pre + i] += static_cast<float>(std::pow(10.0, -12.0 / 20.0) * std::exp(-t / tau) * std::sin(2.0 * 3.14159265358979 * 196.0 * t));
  }
  const auto env = peakEnvDb(x);

  struct Case {
    const char* name;
    GateParams p;
  };
  const Case cases[] = {{"record cell (peak floor + 10, hysteresis 6)", recordCell(pk)}, {"live floorRelative +10", liveGate()}};
  for (const Case& c : cases) {
    Gate g = makeGate(c.p);
    const auto gain = gains(g, x);
    double onset = std::numeric_limits<double>::quiet_NaN();
    double worstAbove12 = 0.0;
    for (std::size_t i = pre + static_cast<std::size_t>(0.02 * kFs); i < pre + len; ++i) {
      const double gdb = gainDbAt(gain, i), above = env[i] - pk;
      if (above > 12.0) worstAbove12 = std::min(worstAbove12, gdb);
      if (std::isnan(onset) && gdb < -1.0) onset = above;
    }
    WARN("REPORT decay tail, " << c.name << ": peak floor " << pk << " dBFS; attenuation (> 1 dB) starts when the envelope is "
                               << onset << " dB above the peak floor; worst gain while > 12 dB above: " << worstAbove12 << " dB");
    INFO(c.name);
    CHECK(worstAbove12 >= -1.0);
  }
}

// Sustain loss of the live gate: a steady tone whose peak envelope sits at floor + 4 / + 2 / + 0 dB (close = floor + 4). Printed for
// the old derivation (ratio 2, range -24) and the new one (ratio 4, range -40); the gate must not touch a tone at the close threshold.
TEST_CASE("Live gate sustain loss at a steady envelope near the floor (printed table)", "[gate][peakfloor]") {
  const std::size_t learn = static_cast<std::size_t>(4 * kFs), tone = static_cast<std::size_t>(1 * kFs);
  const auto noise = gaussian(-75.0, learn, 41);
  struct Variant {
    const char* name;
    double ratio, range;
  };
  const Variant variants[] = {{"OLD ratio 2, range -24", 2.0, -24.0}, {"NEW ratio 4, range -40", 4.0, -40.0}};
  double lossNewAtClose = 0.0;
  std::string table = "REPORT live gate sustain loss (dB of attenuation at a steady envelope re the follower's floor estimate):\n";
  for (const Variant& v : variants) {
    table += std::string("  ") + v.name + ":";
    for (double delta : {4.0, 2.0, 0.0}) {
      Gate g = makeGate(liveGate(v.ratio, v.range));
      (void)gains(g, noise);
      const double est = g.floorEstimateDb();
      // A 196 Hz tone whose measured mean peak envelope is est + delta (the amplitude is calibrated on the detector itself).
      const auto makeTone = [&](double amp) {
        std::vector<float> t(tone);
        for (std::size_t i = 0; i < tone; ++i) t[i] = static_cast<float>(amp * std::sin(2.0 * 3.14159265358979 * 196.0 * static_cast<double>(i) / kFs));
        return t;
      };
      const auto meanEnv = [&](const std::vector<float>& t) {
        const auto e = peakEnvDb(t);
        double sum = 0.0;
        for (std::size_t i = tone / 2; i < tone; ++i) sum += e[i];
        return sum / static_cast<double>(tone - tone / 2);
      };
      const double a0 = std::pow(10.0, (est + delta) / 20.0);
      const double a1 = a0 * std::pow(10.0, ((est + delta) - meanEnv(makeTone(a0))) / 20.0);
      const auto t = makeTone(a1);
      const auto gl = gains(g, t);
      double sum = 0.0;
      for (std::size_t i = tone * 3 / 4; i < tone; ++i) sum += gainDbAt(gl, i);
      const double loss = -sum / static_cast<double>(tone - tone * 3 / 4);
      char buf[64];
      std::snprintf(buf, sizeof buf, "  floor+%.0f: %.1f dB", delta, loss);
      table += buf;
      if (v.ratio == 4.0 && delta == 4.0) lossNewAtClose = loss;
    }
    table += "\n";
  }
  WARN(table);
  CHECK(lossNewAtClose < 1.0);  // at the close threshold itself nothing is taken
}
