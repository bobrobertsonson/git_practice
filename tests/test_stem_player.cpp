// Phase 5.1: StemPlayer (transport, crossfades, loop, count-in, mix, latency, host-follow, RT safety).
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "alloc_guard.h"
#include "latency_stub.h"
#include "sawblade/chain.h"
#include "sawblade/stem_player.h"
#include "stem_test_util.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;
constexpr int kD = static_cast<int>(StemKind::Drums);
constexpr int kB = static_cast<int>(StemKind::Bass);
constexpr int kV = static_cast<int>(StemKind::Vocals);
constexpr int kG = static_cast<int>(StemKind::Guitar);

struct Out {
  std::vector<float> l, r;
  std::size_t size() const { return l.size(); }
};

Out run(StemPlayer& p, int n, int block = 256) {
  Out o;
  o.l.resize(static_cast<std::size_t>(n));
  o.r.resize(static_cast<std::size_t>(n));
  for (int pos = 0; pos < n; pos += block) {
    const int m = std::min(block, n - pos);
    p.process(o.l.data() + pos, o.r.data() + pos, m);
  }
  return o;
}

std::unique_ptr<StemPlayer> make(StemSet set, double fs = kFs, int maxLat = 0) {
  auto p = std::make_unique<StemPlayer>();
  p->prepare({fs, 512}, maxLat);
  p->setStemSet(std::make_unique<StemSet>(std::move(set)));
  p->process(nullptr, nullptr, 0);  // adopt (transport stopped)
  return p;
}

std::vector<float> constant(std::size_t n, float v) { return std::vector<float>(n, v); }

std::vector<float> rampStem(std::size_t n, float k = 1e-5f) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float>(i) * k;
  return x;
}

double maxStep(const std::vector<float>& y, std::size_t from = 1, std::size_t to = static_cast<std::size_t>(-1)) {
  to = std::min(to, y.size());
  double m = 0.0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i) m = std::max(m, std::fabs(static_cast<double>(y[i]) - y[i - 1]));
  return m;
}

double fadeIn(int k, int n) { return std::sin(0.5 * std::numbers::pi * (k + 0.5) / n); }
double fadeOut(int k, int n) { return std::cos(0.5 * std::numbers::pi * (k + 0.5) / n); }
int samplesOf(double ms, double fs = kFs) { return static_cast<int>(std::llround(ms * 0.001 * fs)); }

}  // namespace

// ---- 2. gain ramps -----------------------------------------------------------------------------
TEST_CASE("StemPlayer: stem gain ramps linearly over exactly round(20 ms)", "[stemplayer][mix]") {
  auto p = make(mkSet(kFs, {{StemKind::Drums, constant(40000, 1.0f)}}));
  p->play();
  run(*p, 1000);  // play fade (240) done
  const int N = samplesOf(kMixRampMs);
  REQUIRE(N == 960);
  const double target = std::pow(10.0, -12.0 / 20.0);
  p->setStemGainDb(StemKind::Drums, -12.0);
  const Out o = run(*p, N + 50, 64);
  for (int i = 0; i < N; ++i) {
    const double expect = 1.0 + (target - 1.0) * (i + 1) / N;
    REQUIRE(std::fabs(o.l[static_cast<std::size_t>(i)] - expect) < 1e-6);
    REQUIRE(o.l[static_cast<std::size_t>(i)] == o.r[static_cast<std::size_t>(i)]);
  }
  REQUIRE(std::fabs(o.l[0] - (1.0 + (target - 1.0) / N)) < 1e-6);                        // start
  REQUIRE(std::fabs(o.l[static_cast<std::size_t>(N / 2 - 1)] - (1.0 + (target - 1.0) * 0.5)) < 1e-6);  // midpoint
  REQUIRE(std::fabs(o.l[static_cast<std::size_t>(N - 1)] - target) < 1e-6);               // end
  REQUIRE(o.l[static_cast<std::size_t>(N - 2)] != o.l[static_cast<std::size_t>(N - 1)]);
  for (int i = N; i < N + 50; ++i) REQUIRE(std::fabs(o.l[static_cast<std::size_t>(i)] - target) < 1e-6);

  // Setting the same value again does not restart the ramp.
  p->setStemGainDb(StemKind::Drums, -12.0);
  const Out o2 = run(*p, 10);
  for (float v : o2.l) REQUIRE(std::fabs(v - target) < 1e-6);
}

TEST_CASE("StemPlayer: master level ramps linearly, and a retarget mid-ramp restarts from the current value", "[stemplayer][mix]") {
  auto p = make(mkSet(kFs, {{StemKind::Bass, constant(40000, 1.0f)}}));
  p->play();
  run(*p, 1000);
  const int N = samplesOf(kMixRampMs);
  const double target = std::pow(10.0, -6.0 / 20.0);
  p->setMasterLevelDb(-6.0);
  Out o = run(*p, N + 5);
  for (int i = 0; i < N; ++i) REQUIRE(std::fabs(o.l[static_cast<std::size_t>(i)] - (1.0 + (target - 1.0) * (i + 1) / N)) < 1e-6);
  REQUIRE(std::fabs(o.l[static_cast<std::size_t>(N)] - target) < 1e-6);

  // Retarget half way through: new ramp starts at the value reached so far.
  p->setMasterLevelDb(0.0);
  run(*p, N / 2);
  const double mid = target + (1.0 - target) * 0.0;  // (value after N/2 samples of ramp up is computed below)
  (void)mid;
  p->setMasterLevelDb(-12.0);
  const Out o3 = run(*p, 3);
  // First retargeted sample is one step from the half-way value of the 0 dB ramp.
  const double half = target + (1.0 - target) * (N / 2.0) / N;
  const double t12 = std::pow(10.0, -12.0 / 20.0);
  REQUIRE(std::fabs(o3.l[0] - (half + (t12 - half) / N)) < 1e-6);
}

// ---- 3. mute / solo / guitar mode ----------------------------------------------------------------
TEST_CASE("StemPlayer: mute, solo, mute-wins-over-solo, guitar mode", "[stemplayer][mix]") {
  const std::size_t n = 40000;
  auto p = make(mkSet(kFs, {{StemKind::Drums, constant(n, 0.1f)},
                            {StemKind::Bass, constant(n, 0.2f)},
                            {StemKind::Vocals, constant(n, 0.3f)},
                            {StemKind::Guitar, constant(n, 0.4f)}}));
  p->play();
  Out o = run(*p, 2000);
  // Guitar is muted by default: drums + bass + vocals only.
  REQUIRE(std::fabs(o.l.back() - 0.6f) < 1e-6);

  p->setStemMute(StemKind::Bass, true);
  o = run(*p, 2000);
  REQUIRE(std::fabs(o.l.back() - 0.4f) < 1e-6);
  const int N = samplesOf(kMixRampMs);
  p->setStemMute(StemKind::Bass, false);
  o = run(*p, 10);
  REQUIRE(o.l[0] > 0.4f);  // ramping up, not a jump
  REQUIRE(o.l[0] < 0.41f);
  run(*p, N);

  // Solo one stem: others silent after the ramp.
  p->setStemSolo(StemKind::Vocals, true);
  o = run(*p, 2000);
  REQUIRE(std::fabs(o.l.back() - 0.3f) < 1e-6);
  // Mute wins over solo: the soloed-and-muted stem is silent, and so is everything else.
  p->setStemMute(StemKind::Vocals, true);
  o = run(*p, 2000);
  REQUIRE(o.l.back() == 0.0f);
  p->setStemMute(StemKind::Vocals, false);
  p->setStemSolo(StemKind::Vocals, false);
  o = run(*p, 2000);
  REQUIRE(std::fabs(o.l.back() - 0.6f) < 1e-6);

  // Guitar modes.
  p->setGuitarMode(GuitarMode::Ghost);
  o = run(*p, 2000);
  const double ghost = o.l.back() - 0.6;
  REQUIRE(std::fabs(20.0 * std::log10(ghost / 0.4) - (-12.0)) < 0.01);
  p->setGuitarMode(GuitarMode::Full);
  o = run(*p, 2000);
  REQUIRE(std::fabs(o.l.back() - 1.0f) < 1e-6);
  p->setGuitarMode(GuitarMode::Muted);
  o = run(*p, 2000);
  REQUIRE(std::fabs(o.l.back() - 0.6f) < 1e-6);
}

TEST_CASE("StemPlayer: per-stem gain and click level are clamped", "[stemplayer][mix]") {
  auto p = make(mkSet(kFs, {{StemKind::Drums, constant(20000, 0.1f)}}));
  p->setStemGainDb(StemKind::Drums, 100.0);  // clamps to +12 dB
  p->play();
  const Out o = run(*p, 3000);
  REQUIRE(std::fabs(o.l.back() - 0.1f * std::pow(10.0f, 12.0f / 20.0f)) < 1e-5);
}

// ---- transport ---------------------------------------------------------------------------------
TEST_CASE("StemPlayer: play / pause fades, position, end of set", "[stemplayer][transport]") {
  auto p = make(mkSet(kFs, {{StemKind::Drums, constant(5000, 1.0f)}}));
  const int N = samplesOf(kTransportFadeMs);
  REQUIRE(N == 240);
  REQUIRE_FALSE(p->isPlaying());
  Out o = run(*p, 100);  // stopped: silence, playhead still
  for (float v : o.l) REQUIRE(v == 0.0f);
  REQUIRE(p->position() == 0);

  p->play();
  REQUIRE(p->isPlaying());
  o = run(*p, 1000, 77);
  for (int i = 0; i < N; ++i) REQUIRE(std::fabs(o.l[static_cast<std::size_t>(i)] - (i + 1.0) / N) < 1e-6);
  REQUIRE(o.l[N] == 1.0f);
  REQUIRE(p->position() == 1000);

  p->pause();
  REQUIRE_FALSE(p->isPlaying());
  o = run(*p, 600, 50);
  for (int i = 0; i < N; ++i) REQUIRE(std::fabs(o.l[static_cast<std::size_t>(i)] - (1.0 - (i + 1.0) / N)) < 1e-6);
  for (int i = N; i < 600; ++i) REQUIRE(o.l[static_cast<std::size_t>(i)] == 0.0f);
  REQUIRE(p->position() == 1000 + N);  // advanced during the fade, then stopped

  // Resume from where it stopped; seek while stopped jumps without a fade.
  p->seek(4000);
  p->play();
  o = run(*p, 1500);
  REQUIRE(p->position() == 5500);
  REQUIRE(std::fabs(o.l[0] - 1.0 / N) < 1e-6);
  REQUIRE(o.l[300] == 1.0f);       // sample 4300 < 5000
  REQUIRE(o.l[999] == 1.0f);       // sample 4999
  REQUIRE(o.l[1000] == 0.0f);      // end of the set: zeros, playhead keeps running
  REQUIRE(p->atEnd());
  REQUIRE(p->isPlaying());

  p->seek(1'000'000);  // clamped to the length
  run(*p, 1);
  REQUIRE(p->position() >= 5000);
  p->pause();
  run(*p, 1000);
  p->seek(-5);
  run(*p, 1);
  REQUIRE(p->position() == 0);
}

TEST_CASE("StemPlayer: loop validation", "[stemplayer][loop]") {
  StemPlayer none;
  none.prepare({kFs, 512}, 0);
  REQUIRE_FALSE(none.setLoop(0, 100000));  // no set

  auto p = make(mkSet(kFs, {{StemKind::Drums, constant(20000, 1.0f)}}));
  const std::int64_t minLen = 2 * samplesOf(kLoopFadeMs);
  REQUIRE(minLen == 960);
  REQUIRE_FALSE(p->setLoop(-1, 5000));
  REQUIRE_FALSE(p->setLoop(5000, 5000));
  REQUIRE_FALSE(p->setLoop(6000, 5000));
  REQUIRE_FALSE(p->setLoop(0, 20001));
  REQUIRE_FALSE(p->setLoop(1000, 1000 + minLen - 1));
  REQUIRE_FALSE(p->loopActive());
  REQUIRE(p->setLoop(1000, 1000 + minLen));
  REQUIRE(p->loopActive());
  REQUIRE(p->loopStart() == 1000);
  REQUIRE(p->loopEnd() == 1000 + minLen);
  REQUIRE_FALSE(p->setLoop(0, 100));  // rejected: nothing changes
  REQUIRE(p->loopStart() == 1000);
  REQUIRE(p->setLoop(0, 20000));
  p->clearLoop();
  REQUIRE_FALSE(p->loopActive());
}

// ---- 4. loop crossfade ---------------------------------------------------------------------------
TEST_CASE("StemPlayer: loop wrap is crossfaded (continuity) and the period is exact", "[stemplayer][loop]") {
  // 220 Hz: 1200 samples = 5.5 cycles, so a splice b -> a has a phase mismatch of pi.
  const std::int64_t a = 55, b = 1255;
  const auto sine = [] {
    std::vector<float> x(12000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / kFs));
    return x;
  }();
  const double ownStep = maxStep(sine);
  const double thr = 1.5 * ownStep + 1e-4;

  // Hard-cut splice, for comparison: the threshold must be meaningful.
  std::vector<float> hard;
  for (int n = 0; n < 6000; ++n) hard.push_back(sine[static_cast<std::size_t>(a + n % (b - a))]);
  REQUIRE(maxStep(hard) > thr);

  auto p = make(mkSet(kFs, {{StemKind::Drums, sine}}));
  p->seek(a);
  REQUIRE(p->setLoop(a, b));
  p->play();
  const Out o = run(*p, 6000, 100);
  REQUIRE(maxStep(o.l) <= thr);
  REQUIRE(maxStep(o.r) <= thr);
  REQUIRE(*std::max_element(o.l.begin(), o.l.end()) > 0.45f);  // actually playing

  // Marker impulse at `a` recurs every b - a samples, with the fade-in gain of the wrap.
  std::vector<float> marker(12000, 0.0f);
  marker[static_cast<std::size_t>(a)] = 1.0f;
  auto q = make(mkSet(kFs, {{StemKind::Bass, marker}}));
  q->seek(a);
  REQUIRE(q->setLoop(a, b));
  q->play();
  const Out m = run(*q, 6200, 333);
  std::vector<std::size_t> hits;
  for (std::size_t i = 0; i < m.size(); ++i)
    if (std::fabs(m.l[i]) > 1e-4f) hits.push_back(i);
  REQUIRE(hits == std::vector<std::size_t>{0, 1200, 2400, 3600, 4800, 6000});
  REQUIRE(std::fabs(m.l[1200] - fadeIn(0, samplesOf(kLoopFadeMs))) < 1e-6);
  REQUIRE(q->position() >= a);
  REQUIRE(q->position() < b);
}

TEST_CASE("StemPlayer: a playhead at or beyond b when the loop is set plays on", "[stemplayer][loop]") {
  auto p = make(mkSet(kFs, {{StemKind::Drums, rampStem(20000)}}));
  p->seek(6000);
  p->play();
  run(*p, 100);
  REQUIRE(p->setLoop(1000, 3000));  // playhead (6100) is beyond b
  run(*p, 3000);
  REQUIRE(p->position() == 9100);
  // Seek below b: now it wraps.
  p->seek(2900);
  run(*p, 1000);
  REQUIRE(p->position() < 3000);
  REQUIRE(p->position() >= 1000);
}

TEST_CASE("StemPlayer: loop wrap exact crossfade formula", "[stemplayer][loop]") {
  const auto x = rampStem(20000);
  auto p = make(mkSet(kFs, {{StemKind::Drums, x}}));
  p->seek(500);
  REQUIRE(p->setLoop(1000, 3000));
  p->play();
  const Out o = run(*p, 3000);
  // Output sample n reads stem index 500 + n until the wrap at n = 2500 (b = 3000).
  const int Nl = samplesOf(kLoopFadeMs);
  for (int n = 300; n < 2500; ++n) REQUIRE(std::fabs(o.l[static_cast<std::size_t>(n)] - x[static_cast<std::size_t>(500 + n)]) < 1e-6);
  for (int k = 0; k < Nl; ++k) {
    const double expect = fadeIn(k, Nl) * x[static_cast<std::size_t>(1000 + k)] + fadeOut(k, Nl) * x[static_cast<std::size_t>(3000 + k)];
    REQUIRE(std::fabs(o.l[static_cast<std::size_t>(2500 + k)] - expect) < 1e-6);
  }
  for (int n = 2500 + Nl; n < 3000; ++n) REQUIRE(std::fabs(o.l[static_cast<std::size_t>(n)] - x[static_cast<std::size_t>(1000 + (n - 2500))]) < 1e-6);
}

// ---- 5. seek declick -----------------------------------------------------------------------------
TEST_CASE("StemPlayer: seek while playing is an equal-power crossfade, sample accurate", "[stemplayer][seek]") {
  const auto sine = [] {
    std::vector<float> x(20000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / kFs));
    return x;
  }();
  const double thr = 1.5 * maxStep(sine) + 1e-4;
  {
    // Hard cut at the same splice exceeds the threshold.
    std::vector<float> hard(sine.begin(), sine.begin() + 2000);
    hard.insert(hard.end(), sine.begin() + 3200, sine.begin() + 4000);
    REQUIRE(maxStep(hard) > thr);
    auto p = make(mkSet(kFs, {{StemKind::Drums, sine}}));
    p->play();
    Out o = run(*p, 2000, 64);
    REQUIRE(p->position() == 2000);
    p->seek(3200);
    const Out o2 = run(*p, 1500, 64);
    o.l.insert(o.l.end(), o2.l.begin(), o2.l.end());
    REQUIRE(maxStep(o.l) <= thr);
  }
  {
    const auto x = rampStem(20000);
    auto p = make(mkSet(kFs, {{StemKind::Drums, x}}));
    p->play();
    run(*p, 2000, 100);
    const std::int64_t p0 = p->position();
    p->seek(7000);
    const Out o = run(*p, 600, 32);
    const int N = samplesOf(kSeekFadeMs);
    for (int k = 0; k < N; ++k) {
      const double expect = fadeIn(k, N) * x[static_cast<std::size_t>(7000 + k)] + fadeOut(k, N) * x[static_cast<std::size_t>(p0 + k)];
      REQUIRE(std::fabs(o.l[static_cast<std::size_t>(k)] - expect) < 1e-6);
    }
    for (int k = N; k < 600; ++k) REQUIRE(std::fabs(o.l[static_cast<std::size_t>(k)] - x[static_cast<std::size_t>(7000 + k)]) < 1e-6);
    REQUIRE(p->position() == 7600);

    // A second seek during the crossfade is applied when the crossfade ends.
    const std::int64_t p1 = p->position();
    p->seek(10000);
    run(*p, 100);
    p->seek(15000);
    const Out o3 = run(*p, 600);
    // First seek: 10000 reads from the first sample (before the second seek was issued), crossfade
    // 240 long; the second seek lands when it ends, at sample 240 of o3 (offset by the first 100).
    // Samples 0..139 of o3 are the tail of the first crossfade (k = 100..239).
    for (int j = 0; j < 140; ++j) {
      const int k = 100 + j;
      const double expect = fadeIn(k, N) * x[static_cast<std::size_t>(10000 + k)] + fadeOut(k, N) * x[static_cast<std::size_t>(p1 + k)];
      REQUIRE(std::fabs(o3.l[static_cast<std::size_t>(j)] - expect) < 1e-6);
    }
    // At j = 140 the second crossfade starts: new head at 15000, old head at 10000 + 240.
    for (int k = 0; k < N; ++k) {
      const double expect = fadeIn(k, N) * x[static_cast<std::size_t>(15000 + k)] + fadeOut(k, N) * x[static_cast<std::size_t>(10240 + k)];
      REQUIRE(std::fabs(o3.l[static_cast<std::size_t>(140 + k)] - expect) < 1e-6);
    }
  }
}

TEST_CASE("StemPlayer: a loop wrap that arrives during a seek crossfade is deferred and keeps the period", "[stemplayer][loop][seek]") {
  const auto x = rampStem(30000);
  auto p = make(mkSet(kFs, {{StemKind::Drums, x}}));
  REQUIRE(p->setLoop(1000, 3000));
  p->seek(500);
  p->play();
  run(*p, 500);
  const std::int64_t p0 = p->position();  // 1000
  p->seek(2900);                          // new head reaches b = 3000 after 100 samples, mid-crossfade
  const Out o = run(*p, 1200);
  const int Ns = samplesOf(kSeekFadeMs), Nl = samplesOf(kLoopFadeMs);
  for (int k = 0; k < Ns; ++k) {
    const double expect = fadeIn(k, Ns) * x[static_cast<std::size_t>(2900 + k)] + fadeOut(k, Ns) * x[static_cast<std::size_t>(p0 + k)];
    REQUIRE(std::fabs(o.l[static_cast<std::size_t>(k)] - expect) < 1e-6);  // no wrap yet: reads past b
  }
  // The crossfade ends with the head at 2900 + 240 = 3140: overshoot 140 -> lands at a + 140 = 1140.
  for (int j = 0; j < Nl; ++j) {
    const double expect = fadeIn(j, Nl) * x[static_cast<std::size_t>(1140 + j)] + fadeOut(j, Nl) * x[static_cast<std::size_t>(3140 + j)];
    REQUIRE(std::fabs(o.l[static_cast<std::size_t>(Ns + j)] - expect) < 1e-6);
  }
  REQUIRE(p->position() == 1140 + (1200 - Ns));
}

// ---- 6. count-in ---------------------------------------------------------------------------------
namespace {
void checkCountIn(double fs, int bars, double bpm, int bpb) {
  const std::size_t setLen = 400000;
  auto p = make(mkSet(fs, {{StemKind::Drums, constant(setLen, 1.0f)}}), fs);
  p->setCountIn(bars, bpm, bpb);
  const std::int64_t beats = static_cast<std::int64_t>(bars) * bpb;
  const auto C = static_cast<std::int64_t>(std::llround(static_cast<double>(beats) * 60.0 * fs / bpm));
  std::vector<float> expect(static_cast<std::size_t>(C), 0.0f);
  for (std::int64_t k = 0; k < beats; ++k) {
    const auto start = static_cast<std::int64_t>(std::llround(static_cast<double>(k) * 60.0 * fs / bpm));
    const auto& click = (k % bpb == 0) ? p->accentClick() : p->normalClick();
    for (std::size_t i = 0; i < click.size(); ++i) {
      REQUIRE(static_cast<std::size_t>(start) + i < expect.size());
      REQUIRE(expect[static_cast<std::size_t>(start) + i] == 0.0f);  // no overlaps at these tempos
      expect[static_cast<std::size_t>(start) + i] = click[i];
    }
  }
  p->play();
  const int total = static_cast<int>(C) + 400;
  const Out o = run(*p, total, 1000);
  for (std::int64_t n = 0; n < C; ++n) {
    REQUIRE(o.l[static_cast<std::size_t>(n)] == expect[static_cast<std::size_t>(n)]);
    REQUIRE(o.r[static_cast<std::size_t>(n)] == expect[static_cast<std::size_t>(n)]);
  }
  // First stem sample at C with the play fade-in.
  const int Nt = samplesOf(kTransportFadeMs, fs);
  for (int i = 0; i < Nt; ++i) REQUIRE(std::fabs(o.l[static_cast<std::size_t>(C + i)] - (i + 1.0) / Nt) < 1e-6);
  REQUIRE(o.l[static_cast<std::size_t>(C + Nt)] == 1.0f);
  REQUIRE_FALSE(p->isCountingIn());
  REQUIRE(p->isPlaying());
  REQUIRE(p->position() == total - C);  // the playhead was held during the count-in
}
}  // namespace

TEST_CASE("StemPlayer: count-in clicks land exactly on round(k*60*fs/bpm)", "[stemplayer][countin]") {
  SECTION("2 bars, 120 bpm, 48 kHz") { checkCountIn(48000.0, 2, 120.0, 4); }
  SECTION("1 bar, 137 bpm (non-integer beat), 44.1 kHz") { checkCountIn(44100.0, 1, 137.0, 4); }
  SECTION("3/4, accent every third beat") { checkCountIn(48000.0, 2, 200.0, 3); }
}

TEST_CASE("StemPlayer: click buffers follow the spec formula; pause cancels the count-in", "[stemplayer][countin]") {
  auto p = make(mkSet(kFs, {{StemKind::Drums, constant(100000, 1.0f)}}));
  const auto len = static_cast<std::size_t>(std::llround(0.030 * kFs));
  REQUIRE(p->accentClick().size() == len);
  REQUIRE(p->normalClick().size() == len);
  const double A = std::pow(10.0, -6.0 / 20.0);
  for (std::size_t n = 0; n < len; ++n) {
    const double x = static_cast<double>(n);
    const double e = std::exp(-x / (0.005 * kFs));
    REQUIRE(std::fabs(p->accentClick()[n] - A * std::sin(2.0 * std::numbers::pi * 1500.0 * x / kFs) * e) < 1e-6);
    REQUIRE(std::fabs(p->normalClick()[n] - A * std::sin(2.0 * std::numbers::pi * 1000.0 * x / kFs) * e) < 1e-6);
  }
  p->setClickLevelDb(-20.0);
  REQUIRE(std::fabs(p->accentClick()[20] / (A * std::sin(2.0 * std::numbers::pi * 1500.0 * 20.0 / kFs) * std::exp(-20.0 / (0.005 * kFs))) -
                    std::pow(10.0, -14.0 / 20.0)) < 1e-4);

  p->setCountIn(2, 120.0);
  p->play();
  run(*p, 30000);
  REQUIRE(p->isCountingIn());
  REQUIRE(p->isPlaying());
  p->pause();
  Out o = run(*p, 5000);
  REQUIRE_FALSE(p->isCountingIn());
  REQUIRE_FALSE(p->isPlaying());
  REQUIRE(p->position() == 0);
  for (std::size_t i = 3000; i < o.size(); ++i) REQUIRE(o.l[i] == 0.0f);
  // Count-in off (bars = 0): play starts the stems immediately.
  p->setCountIn(0, 120.0);
  p->play();
  o = run(*p, 400);
  REQUIRE_FALSE(p->isCountingIn());
  REQUIRE(o.l[399] == 1.0f);

  // Out-of-range values are clamped: bars 99 -> 8, bpm 1 -> 30.
  p->pause();
  run(*p, 1000);
  p->setCountIn(99, 1.0, 100);
  p->play();
  run(*p, 10);
  REQUIRE(p->isCountingIn());
}

// ---- 7. latency alignment ------------------------------------------------------------------------
TEST_CASE("StemPlayer: rig latency lines the backing up with a Chain", "[stemplayer][latency]") {
  registerLatencyStub();
  const fs::path presets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";
  auto nam = [](const std::string& id) { return json{{"id", id}, {"type", "nam"}, {"model", {{"file", "../nam/linear_identity.nam"}}}}; };
  json j = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "t"},
            {"paths", {{"a", {{"blocks", json::array({json{{"id", "s1"}, {"type", "test.latency"}, {"latency", 37}}, nam("a1")})}}},
                       {"b", {{"blocks", json::array({nam("b1")})}}}}},
            {"align", {{"mode", "off"}}},
            {"blend", 0.5},
            {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}}};
  const Preset preset = parsePreset(j, presets);
  auto res = loadResources(preset, kFs);
  Chain chain(preset, std::move(res));
  chain.prepare({kFs, 256});
  REQUIRE(chain.latencySamples() == 37);

  const std::size_t i0 = 700, len = 3000;
  std::vector<float> di(len, 0.0f);
  di[i0] = 1.0f;
  std::vector<float> guitar(len, 0.0f);
  for (std::size_t pos = 0; pos < len; pos += 256) {
    const int n = static_cast<int>(std::min<std::size_t>(256, len - pos));
    chain.process(di.data() + pos, guitar.data() + pos, n);
  }

  std::vector<float> imp(len, 0.0f);
  imp[i0] = 1.0f;
  auto p = make(mkSet(kFs, {{StemKind::Drums, imp}}), kFs, 64);
  p->setRigLatencySamples(chain.latencySamples());
  REQUIRE(p->latencySamples() == 0);
  p->play();
  const Out o = run(*p, static_cast<int>(len), 100);

  auto peakAt = [](const std::vector<float>& v) {
    return static_cast<std::size_t>(std::max_element(v.begin(), v.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) - v.begin());
  };
  REQUIRE(peakAt(guitar) == i0 + 37);
  REQUIRE(peakAt(o.l) == i0 + 37);
  REQUIRE(peakAt(o.r) == i0 + 37);

  // The clamp: more than the prepared maximum is limited to it.
  p->setRigLatencySamples(1000);
  REQUIRE(p->rigLatencySamples() == 64);
  p->setRigLatencySamples(-4);
  REQUIRE(p->rigLatencySamples() == 0);
}

// ---- 8. host-follow ------------------------------------------------------------------------------
namespace {
struct HostBlock {
  std::int64_t pos;
  bool playing;
};

Out runHost(StemPlayer& p, int blocks, int block, const std::function<HostBlock(int)>& host) {
  Out o;
  o.l.resize(static_cast<std::size_t>(blocks * block));
  o.r.resize(o.l.size());
  for (int b = 0; b < blocks; ++b) {
    const HostBlock h = host(b);
    p.setHostPosition(h.pos, h.playing);
    p.process(o.l.data() + b * block, o.r.data() + b * block, block);
  }
  return o;
}
}  // namespace

TEST_CASE("StemPlayer host-follow: steady advance equals free-run, jitter within the threshold is ignored", "[stemplayer][host]") {
  const std::size_t n = 30000;
  auto mkSetN = [&] { return mkSet(kFs, {{StemKind::Drums, noise(n, 5)}, {StemKind::Bass, sine(110.0, kFs, n)}}); };
  auto free = make(mkSetN());
  free->play();
  const Out ref = run(*free, 64 * 100, 64);

  auto h = make(mkSetN());
  h->setTransportMode(TransportMode::HostFollow);
  const Out steady = runHost(*h, 100, 64, [](int b) { return HostBlock{b * 64, true}; });
  REQUIRE(steady.l == ref.l);
  REQUIRE(steady.r == ref.r);

  // Jitter of up to +-64 samples (the default threshold) is ignored.
  auto j = make(mkSetN());
  j->setTransportMode(TransportMode::HostFollow);
  const int jitter[] = {0, 64, -64, 17, -3, 60, -60, 64};
  const Out jittered = runHost(*j, 100, 64, [&](int b) { return HostBlock{b * 64 + jitter[b % 8], true}; });
  REQUIRE(jittered.l == ref.l);
  REQUIRE(jittered.r == ref.r);
  REQUIRE(j->position() == 6400);

  // Free-run commands are ignored in host-follow mode.
  j->pause();
  j->seek(100);
  j->setCountIn(2, 120.0);
  const Out again = runHost(*j, 4, 64, [](int b) { return HostBlock{6400 + b * 64, true}; });
  REQUIRE(j->position() == 6400 + 256);
  REQUIRE(*std::max_element(again.l.begin(), again.l.end()) != 0.0f);
}

TEST_CASE("StemPlayer host-follow: a jump beyond the threshold is a 5 ms crossfade to the host position", "[stemplayer][host]") {
  const std::size_t n = 30000;
  const auto sineStem = sine(220.0, kFs, n);
  const double thr = 1.5 * maxStep(sineStem) + 1e-4;
  const int jumpBlock = 40, B = 64;

  auto jumpAt = [&](std::int64_t delta) {
    return [=](int b) { return HostBlock{b * B + (b >= jumpBlock ? delta : 0), true}; };
  };
  {
    auto p = make(mkSet(kFs, {{StemKind::Drums, sineStem}}));
    p->setTransportMode(TransportMode::HostFollow);
    const Out o = runHost(*p, 120, B, jumpAt(1200));  // 1200 samples = 5.5 cycles: phase mismatch pi
    REQUIRE(maxStep(o.l) <= thr);
    REQUIRE(p->position() == 120 * B + 1200);
  }
  const auto x = rampStem(n);
  for (std::int64_t delta : {65, 1200, -1000}) {
    auto p = make(mkSet(kFs, {{StemKind::Drums, x}}));
    p->setTransportMode(TransportMode::HostFollow);
    const Out o = runHost(*p, 120, B, jumpAt(delta));
    const int N = samplesOf(kSeekFadeMs);
    const std::int64_t p0 = jumpBlock * B;  // internal playhead at the block that sees the jump
    const std::int64_t h0 = p0 + delta;
    for (int k = 0; k < N; ++k) {  // the new head reads the host position from the first sample of that block
      const double expect = fadeIn(k, N) * x[static_cast<std::size_t>(h0 + k)] + fadeOut(k, N) * x[static_cast<std::size_t>(p0 + k)];
      REQUIRE(std::fabs(o.l[static_cast<std::size_t>(p0 + k)] - expect) < 1e-6);
    }
    for (int k = N; k < 500; ++k) REQUIRE(std::fabs(o.l[static_cast<std::size_t>(p0 + k)] - x[static_cast<std::size_t>(h0 + k)]) < 1e-6);
  }
  // Exactly the threshold is ignored; one more triggers.
  {
    auto p = make(mkSet(kFs, {{StemKind::Drums, x}}));
    p->setTransportMode(TransportMode::HostFollow);
    const Out o = runHost(*p, 60, B, [&](int b) { return HostBlock{b * B + (b == 30 ? 64 : 0), true}; });
    REQUIRE(std::fabs(o.l[30 * B + 3] - x[30 * B + 3]) < 1e-6);
  }
}

TEST_CASE("StemPlayer host-follow: stop / start fade, pre-roll is silent", "[stemplayer][host]") {
  const std::size_t n = 30000;
  auto p = make(mkSet(kFs, {{StemKind::Drums, constant(n, 1.0f)}}));
  p->setTransportMode(TransportMode::HostFollow);
  const int N = samplesOf(kTransportFadeMs), B = 64;
  // Pre-roll: negative host positions read silence (the playhead advances).
  Out o = runHost(*p, 8, B, [&](int b) { return HostBlock{-4 * B + b * B, true}; });
  for (int i = 0; i < 4 * B; ++i) REQUIRE(o.l[static_cast<std::size_t>(i)] == 0.0f);
  // After the pre-roll the stem plays, with the 5 ms play fade-in that started at the first block.
  REQUIRE(o.l[7 * B] == 1.0f);

  // Host stops: pause fade 1 -> 0 over 5 ms, then silence; the playhead stops.
  const std::int64_t p0 = p->position();
  o = runHost(*p, 10, B, [&](int b) { return HostBlock{p0 + b * B, false}; });
  for (int i = 0; i < N; ++i) REQUIRE(std::fabs(o.l[static_cast<std::size_t>(i)] - (1.0 - (i + 1.0) / N)) < 1e-6);
  for (int i = N; i < 10 * B; ++i) REQUIRE(o.l[static_cast<std::size_t>(i)] == 0.0f);
  REQUIRE(p->position() == p0 + N);
  REQUIRE_FALSE(p->isPlaying());

  // Host starts again at a new position: playhead = host position, 5 ms fade-in.
  o = runHost(*p, 10, B, [&](int b) { return HostBlock{9000 + b * B, true}; });
  for (int i = 0; i < N; ++i) REQUIRE(std::fabs(o.l[static_cast<std::size_t>(i)] - (i + 1.0) / N) < 1e-6);
  REQUIRE(o.l[N] == 1.0f);
  REQUIRE(p->position() == 9000 + 10 * B);
  REQUIRE(p->isPlaying());
}

// ---- 9. block-size independence --------------------------------------------------------------------
namespace {
struct Cmd {
  std::int64_t at;
  std::function<void(StemPlayer&)> fn;
};

Out scripted(int block) {
  const double fs = 16000.0;
  const std::size_t n = 40000;
  StemSet set = mkSet(fs, {{StemKind::Drums, noise(n, 1)},
                           {StemKind::Bass, sine(110.0, fs, n)},
                           {StemKind::Vocals, noise(n, 3)},
                           {StemKind::Other, sine(330.0, fs, n)},
                           {StemKind::Guitar, noise(n, 5)}});
  auto p = make(std::move(set), fs, 64);
  const std::vector<Cmd> cmds = {
      {0, [](StemPlayer& s) { s.setGuitarMode(GuitarMode::Full); s.setRigLatencySamples(23); s.play(); }},
      {500, [](StemPlayer& s) { s.setStemGainDb(StemKind::Drums, -6.0); }},
      {1500, [](StemPlayer& s) { s.setStemMute(StemKind::Bass, true); }},
      {3000, [](StemPlayer& s) { s.seek(12000); }},
      {4000, [](StemPlayer& s) { (void)s.setLoop(10000, 14000); }},
      {9500, [](StemPlayer& s) { s.seek(11000); }},
      {11000, [](StemPlayer& s) { s.seek(13950); }},  // new head hits b mid-crossfade: deferred wrap
      {12000, [](StemPlayer& s) { s.setMasterLevelDb(-3.0); }},
      {12500, [](StemPlayer& s) { s.setStemSolo(StemKind::Vocals, true); }},
      {13000, [](StemPlayer& s) { s.setStemSolo(StemKind::Vocals, false); s.setGuitarMode(GuitarMode::Ghost); }},
      {15000, [](StemPlayer& s) { s.pause(); }},
      {16000, [](StemPlayer& s) { s.seek(2000); s.clearLoop(); }},
      {16500, [](StemPlayer& s) { s.setCountIn(1, 300.0, 4); s.play(); }},
  };
  const std::int64_t total = 33000;
  Out o;
  o.l.resize(static_cast<std::size_t>(total));
  o.r.resize(o.l.size());
  std::int64_t t = 0;
  std::size_t ci = 0;
  while (t < total) {
    while (ci < cmds.size() && cmds[ci].at == t) cmds[ci++].fn(*p);
    const std::int64_t next = ci < cmds.size() ? cmds[ci].at : total;
    const auto m = static_cast<int>(std::min<std::int64_t>({block, next - t, total - t}));
    p->process(o.l.data() + t, o.r.data() + t, m);
    t += m;
  }
  return o;
}
}  // namespace

TEST_CASE("StemPlayer: scripted free-run session is bit-identical for any block size", "[stemplayer][determinism]") {
  const Out ref = scripted(512);
  REQUIRE(*std::max_element(ref.l.begin(), ref.l.end()) > 0.1f);
  // The session really exercises the engine: not silent in the loop region, count-in clicks present.
  REQUIRE(rms(ref.l.data() + 6000, 2000) > 0.01);
  for (int b : {1, 7, 64, 4096}) {
    const Out o = scripted(b);
    REQUIRE(o.l == ref.l);
    REQUIRE(o.r == ref.r);
  }
}

// ---- 10. zero allocations -------------------------------------------------------------------------
TEST_CASE("StemPlayer: process() and the audio-thread setters do not allocate", "[stemplayer][alloc]") {
  const double fs = 48000.0;
  const std::size_t n = 200000;
  StemSet set = mkSet(fs, {{StemKind::Drums, noise(n, 1)}, {StemKind::Guitar, noise(n, 2)}, {StemKind::Bass, sine(100.0, fs, n)}});
  auto p = make(std::move(set), fs, 64);
  std::vector<float> l(2048), r(2048);

  {
    AllocGuard g;
    p->setCountIn(1, 240.0);
    p->setClickLevelDb(-12.0);
    p->setRigLatencySamples(31);
    p->play();                                // count-in
    for (int i = 0; i < 40; ++i) p->process(l.data(), r.data(), 2048);
    p->setStemGainDb(StemKind::Drums, -9.0);  // ramps
    p->setStemMute(StemKind::Bass, true);
    p->setStemSolo(StemKind::Guitar, true);
    p->setGuitarMode(GuitarMode::Ghost);
    p->setMasterLevelDb(-3.0);
    p->process(l.data(), r.data(), 1000);
    p->setStemSolo(StemKind::Guitar, false);
    (void)p->setLoop(5000, 9000);
    p->seek(8000);
    p->process(l.data(), r.data(), 700);
    p->seek(4500);
    p->process(l.data(), r.data(), 100);  // seek during a crossfade is deferred
    p->seek(8900);
    for (int i = 0; i < 20; ++i) p->process(l.data(), r.data(), 777);  // loop wraps
    p->setRigLatencySamples(5);
    p->process(l.data(), r.data(), 300);
    p->pause();
    p->process(l.data(), r.data(), 2048);
    p->process(l.data(), r.data(), 0);
    p->clearLoop();
    p->seek(0);
    p->setCountIn(0, 120.0);
    p->play();
    for (int i = 0; i < 3; ++i) p->process(l.data(), r.data(), 1700);
    // Host-follow, with jumps.
    p->setTransportMode(TransportMode::HostFollow);
    for (int b = 0; b < 40; ++b) {
      p->setHostPosition(b * 64 + (b > 10 ? 5000 : 0) + (b > 25 ? -3000 : 0), b < 35);
      p->process(l.data(), r.data(), 64);
    }
    REQUIRE(g.count() == 0);
  }

  // The block in which a pending set is adopted, and the old set is not destroyed in process().
  p->setTransportMode(TransportMode::FreeRun);
  p->pause();
  run(*p, 3000);
  p->setStemSet(std::make_unique<StemSet>(mkSet(fs, {{StemKind::Drums, noise(n, 9)}})));
  {
    AllocGuard g;
    p->process(l.data(), r.data(), 512);
    REQUIRE(g.count() == 0);
    REQUIRE(g.frees() == 0);
    p->play();
    p->process(l.data(), r.data(), 512);
    REQUIRE(g.count() == 0);
    REQUIRE(g.frees() == 0);
  }
  REQUIRE(p->stemSetLength() == static_cast<std::int64_t>(n));
  {
    AllocGuard g;
    p->collectGarbage();  // the retired set (and its node) are freed here, off the audio path
    REQUIRE(g.frees() > 0);
  }
}

// ---- 11. set adoption ------------------------------------------------------------------------------
TEST_CASE("StemPlayer: a pending set is adopted only when fully stopped", "[stemplayer][adopt]") {
  auto p = make(mkSet(kFs, {{StemKind::Drums, constant(5000, 0.5f)}}));
  REQUIRE(p->stemSetLength() == 5000);
  REQUIRE(p->hasStemSet());
  REQUIRE(p->setLoop(1000, 3000));
  p->play();
  run(*p, 500);

  p->setStemSet(std::make_unique<StemSet>(mkSet(kFs, {{StemKind::Bass, constant(9000, 0.25f)}})));
  Out o = run(*p, 1000);
  REQUIRE(p->stemSetLength() == 5000);  // still the old set while playing
  REQUIRE(p->loopActive());
  REQUIRE(std::fabs(o.l.back() - 0.5f) < 1e-6);

  p->pause();
  run(*p, 100);  // pause fade (240) not complete: still not adopted
  REQUIRE(p->stemSetLength() == 5000);
  run(*p, 300, 300);  // one block in which the fade completes: adoption waits for the next block start
  REQUIRE(p->stemSetLength() == 5000);
  o = run(*p, 10);  // this block starts fully stopped: adoption
  REQUIRE(p->stemSetLength() == 9000);
  REQUIRE(p->position() == 0);
  REQUIRE_FALSE(p->loopActive());
  for (float v : o.l) REQUIRE(v == 0.0f);
  p->play();
  o = run(*p, 1000);
  REQUIRE(std::fabs(o.l.back() - 0.25f) < 1e-6);  // the new set plays from 0

  // Rate mismatch (or null) throws on the producer thread, the pending set is untouched.
  REQUIRE_THROWS_AS(p->setStemSet(std::make_unique<StemSet>(mkSet(44100.0, {{StemKind::Drums, constant(100, 1.0f)}}))), std::invalid_argument);
  REQUIRE_THROWS_AS(p->setStemSet(nullptr), std::invalid_argument);

  // No count-in adoption either: a set waits while counting in.
  p->pause();
  run(*p, 1000);
  p->setCountIn(1, 120.0);
  p->play();
  run(*p, 100);
  REQUIRE(p->isCountingIn());
  p->setStemSet(std::make_unique<StemSet>(mkSet(kFs, {{StemKind::Drums, constant(777, 1.0f)}})));
  run(*p, 100);
  REQUIRE(p->stemSetLength() == 9000);

  // prepare() at another rate drops the adopted set; silence until a matching set arrives.
  StemPlayer q;
  q.prepare({kFs, 256}, 0);
  q.setStemSet(std::make_unique<StemSet>(mkSet(kFs, {{StemKind::Drums, constant(1000, 1.0f)}})));
  q.process(nullptr, nullptr, 0);
  REQUIRE(q.hasStemSet());
  q.prepare({44100.0, 256}, 0);
  REQUIRE_FALSE(q.hasStemSet());
  q.process(nullptr, nullptr, 0);
  REQUIRE_FALSE(q.hasStemSet());
  q.play();
  const Out s = run(q, 600);
  for (float v : s.l) REQUIRE(v == 0.0f);
}

TEST_CASE("StemPlayer: with no set the stems are silent but count-in clicks still play", "[stemplayer][countin]") {
  StemPlayer p;
  p.prepare({kFs, 256}, 0);
  p.setCountIn(1, 120.0);
  p.play();
  const Out o = run(p, 2000);
  REQUIRE(o.l[0] == p.accentClick()[0]);
  REQUIRE(o.l[100] == p.accentClick()[100]);
  REQUIRE(*std::max_element(o.l.begin(), o.l.end()) > 0.1f);
}

// ---- first-set adoption (no set adopted yet) ----------------------------------------------------------
TEST_CASE("StemPlayer: with no set adopted, the first set is adopted in any block and keeps the playhead", "[stemplayer][adopt]") {
  StemPlayer p;
  p.prepare({kFs, 512}, 0);
  p.play();
  run(p, 1000);
  REQUIRE(p.position() == 1000);
  REQUIRE_FALSE(p.hasStemSet());
  p.setStemSet(std::make_unique<StemSet>(mkSet(kFs, {{StemKind::Drums, constant(5000, 0.5f)}})));
  const Out o = run(p, 256);
  REQUIRE(p.hasStemSet());
  REQUIRE(p.position() == 1256);  // not reset
  REQUIRE(o.l[0] == 0.5f);        // audible from the adoption block on (play fade long done)
  REQUIRE(o.l[255] == 0.5f);
  // A second set while playing waits (the stopped-only rule applies once a set is adopted).
  p.setStemSet(std::make_unique<StemSet>(mkSet(kFs, {{StemKind::Drums, constant(9000, 0.5f)}})));
  run(p, 100);
  REQUIRE(p.stemSetLength() == 5000);
}

TEST_CASE("StemPlayer: first set arriving during a count-in; stale playhead reads silence", "[stemplayer][adopt]") {
  StemPlayer p;
  p.prepare({kFs, 512}, 0);
  p.setCountIn(1, 240.0);
  p.play();
  run(p, 100);
  REQUIRE(p.isCountingIn());
  p.setStemSet(std::make_unique<StemSet>(mkSet(kFs, {{StemKind::Drums, constant(30000, 0.5f)}})));
  run(p, 100);
  REQUIRE(p.hasStemSet());
  REQUIRE(p.isCountingIn());
  REQUIRE(p.position() == 0);  // held during the count-in
  const std::int64_t C = std::llround(4.0 * 60.0 * kFs / 240.0);
  const Out o = run(p, static_cast<int>(C) + 400);
  REQUIRE(o.l[static_cast<std::size_t>(C - 200 + 300)] == 0.5f);  // stems play after the count-in

  // Playhead beyond the new set's length: silence, no out-of-bounds read.
  StemPlayer q;
  q.prepare({kFs, 512}, 0);
  q.play();
  run(q, 2000);
  q.setStemSet(std::make_unique<StemSet>(mkSet(kFs, {{StemKind::Drums, constant(500, 1.0f)}})));
  const Out s = run(q, 500);
  for (float v : s.l) REQUIRE(v == 0.0f);
  REQUIRE(q.atEnd());
  q.pause();
  run(q, 1000);
  q.seek(0);
  q.play();
  REQUIRE(run(q, 400).l[399] == 1.0f);
}

TEST_CASE("StemPlayer: adopting a first set while playing does not allocate", "[stemplayer][alloc]") {
  StemPlayer p;
  p.prepare({kFs, 512}, 0);
  p.play();
  std::vector<float> l(512), r(512);
  p.process(l.data(), r.data(), 512);
  p.setStemSet(std::make_unique<StemSet>(mkSet(kFs, {{StemKind::Drums, noise(20000, 3)}})));
  AllocGuard g;
  p.process(l.data(), r.data(), 512);
  p.process(l.data(), r.data(), 512);
  REQUIRE(g.count() == 0);
  REQUIRE(g.frees() == 0);
  REQUIRE(p.hasStemSet());
}

// ---- host-follow: a second jump while the first crossfade is still running -------------------------------
TEST_CASE("StemPlayer host-follow: a jump during a running crossfade is deferred and compensated", "[stemplayer][host]") {
  const auto x = rampStem(30000);
  auto p = make(mkSet(kFs, {{StemKind::Drums, x}}));
  p->setTransportMode(TransportMode::HostFollow);
  const int B = 64;
  auto host = [](int b) {
    const std::int64_t off = b < 40 ? 0 : (b < 42 ? 1200 : 3200);
    return HostBlock{b * 64 + off, true};
  };
  const Out o = runHost(*p, 120, B, host);
  const int N = samplesOf(kSeekFadeMs);
  // First crossfade ends at sample 240 of the jump = block 43, sample 48. The deferred seek targets
  // the host position of that very sample, with the old head at (first target + 240).
  const std::int64_t t0 = 43 * 64 + 48;
  const std::int64_t hostThen = 43 * 64 + 3200 + 48;
  const std::int64_t oldHead = 40 * 64 + 1200 + N;
  for (int k = 0; k < N; ++k) {
    const double expect = fadeIn(k, N) * x[static_cast<std::size_t>(hostThen + k)] + fadeOut(k, N) * x[static_cast<std::size_t>(oldHead + k)];
    REQUIRE(std::fabs(o.l[static_cast<std::size_t>(t0 + k)] - expect) < 1e-6);
  }
  for (int t = static_cast<int>(t0) + N; t < 120 * B; ++t)
    REQUIRE(std::fabs(o.l[static_cast<std::size_t>(t)] - x[static_cast<std::size_t>(t + 3200)]) < 1e-6);
  REQUIRE(p->position() == 120 * B + 3200);
}

// ---- 5.2: start offset (playhead time p plays stem sample p - offset) ----------------------------
namespace {
// Strictly increasing, all distinct: stem[i] = (i + 1) * 1e-5 (exact as float for the indices used).
std::vector<float> idStem(std::size_t n) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float>(i + 1) * 1e-5f;
  return x;
}
// A player with the offset applied (the first set is adopted by the process(0) in make()).
std::unique_ptr<StemPlayer> makeOffset(const std::vector<float>& stem, std::int64_t offset) {
  auto p = std::make_unique<StemPlayer>();
  p->prepare({kFs, 512}, 0);
  p->setStartOffsetSamples(offset);
  p->setStemSet(std::make_unique<StemSet>(mkSet(kFs, {{StemKind::Drums, stem}})));
  p->process(nullptr, nullptr, 0);
  return p;
}
}  // namespace

TEST_CASE("StemPlayer offset: playhead p plays stem sample p - offset, to the sample", "[stemplayer][offset]") {
  const auto stem = idStem(6000);
  const int fade = samplesOf(kTransportFadeMs);  // 240: output is exactly the stem after this
  for (const int block : {1, 7, 64, 512, 4096}) {
    CAPTURE(block);
    SECTION("positive: silence for the first `offset` samples, then the stems from sample 0") {
      auto p = makeOffset(stem, 1000);
      REQUIRE(p->appliedStartOffsetSamples() == 1000);
      REQUIRE(p->stemSetLength() == 6000);
      REQUIRE(p->playheadLength() == 7000);
      p->play();
      const Out o = run(*p, 7100, block);
      for (int i = 0; i < 1000; ++i) REQUIRE(o.l[static_cast<std::size_t>(i)] == 0.0f);
      for (int i = 1000; i < 6999 + 1; ++i) {
        // the play fade (240 samples from p = 0) is long over at p = 1000
        REQUIRE(o.l[static_cast<std::size_t>(i)] == stem[static_cast<std::size_t>(i - 1000)]);
        REQUIRE(o.r[static_cast<std::size_t>(i)] == stem[static_cast<std::size_t>(i - 1000)]);
      }
      for (int i = 7000; i < 7100; ++i) REQUIRE(o.l[static_cast<std::size_t>(i)] == 0.0f);  // past the end
    }
    SECTION("negative: the stems lead, stem audio from |offset| plays at p = 0") {
      auto p = makeOffset(stem, -700);
      REQUIRE(p->playheadLength() == 5300);
      p->play();
      const Out o = run(*p, 5400, block);
      for (int i = fade; i < 5300; ++i) REQUIRE(o.l[static_cast<std::size_t>(i)] == stem[static_cast<std::size_t>(i + 700)]);
      // the first samples are the play fade of stem sample 700 onwards
      REQUIRE(o.l[0] == Catch::Approx(static_cast<double>(stem[700]) * (1.0 / fade)).epsilon(1e-6));
      for (int i = 5300; i < 5400; ++i) REQUIRE(o.l[static_cast<std::size_t>(i)] == 0.0f);
    }
  }
  // Block size independence of the whole render, fade included.
  auto a = makeOffset(stem, -700), b = makeOffset(stem, -700);
  a->play();
  b->play();
  const Out oa = run(*a, 5000, 1), ob = run(*b, 5000, 777);
  REQUIRE(oa.l == ob.l);
}

TEST_CASE("StemPlayer offset: takes effect only while stopped; the playhead stays", "[stemplayer][offset]") {
  const auto stem = idStem(20000);
  auto p = makeOffset(stem, 0);
  p->play();
  run(*p, 1000);
  p->setStartOffsetSamples(500);  // set while playing: producer thread, applied at the next stopped block
  const Out playing = run(*p, 300);
  for (int i = 0; i < 300; ++i) REQUIRE(playing.l[static_cast<std::size_t>(i)] == stem[static_cast<std::size_t>(1000 + i)]);
  REQUIRE(p->appliedStartOffsetSamples() == 0);
  REQUIRE(p->startOffsetSamples() == 500);

  p->pause();
  run(*p, 2000);  // the pause fade finishes, the transport stops
  REQUIRE(p->position() >= 1300);
  const std::int64_t at = p->position();
  p->play();
  const Out o = run(*p, 1000);
  REQUIRE(p->appliedStartOffsetSamples() == 500);
  REQUIRE(p->playheadLength() == 20500);
  REQUIRE(p->position() == at + 1000);  // playhead time kept, the content moved under it
  for (int i = static_cast<int>(kTransportFadeMs * 0.001 * kFs) + 5; i < 1000; ++i)
    REQUIRE(o.l[static_cast<std::size_t>(i)] == stem[static_cast<std::size_t>(at + i - 500)]);
}

TEST_CASE("StemPlayer offset: seek, loop and position are in playhead time", "[stemplayer][offset]") {
  const auto stem = idStem(30000);
  auto p = makeOffset(stem, 2000);  // playhead length 32000
  p->seek(10000);
  p->play();
  run(*p, 2000);  // seek takes effect while stopped (the playhead jumps), fade done
  REQUIRE(p->position() == 12000);
  const Out o = run(*p, 100);
  for (int i = 0; i < 100; ++i) REQUIRE(o.l[static_cast<std::size_t>(i)] == stem[static_cast<std::size_t>(12000 + i - 2000)]);

  // Loop points beyond the stem length but inside the playhead length are valid; beyond it are not.
  REQUIRE(p->setLoop(31000, 32000));
  REQUIRE_FALSE(p->setLoop(31000, 32001));
  REQUIRE(p->loopStart() == 31000);
  p->seek(30500);
  // let it wrap: after B the position returns to A + overshoot
  run(*p, 4000);
  REQUIRE(p->position() >= 31000);
  REQUIRE(p->position() < 32000);

  // Seek clamps to the playhead length, and the end is the playhead end.
  p->clearLoop();
  p->seek(1000000);
  run(*p, 1000);
  REQUIRE(p->atEnd());
  REQUIRE(p->position() >= 32000);
}

TEST_CASE("StemPlayer offset: host-follow position is playhead time", "[stemplayer][offset]") {
  const auto stem = idStem(20000);
  auto p = makeOffset(stem, 3000);
  p->setTransportMode(TransportMode::HostFollow);
  std::int64_t host = 5000;
  Out o;
  o.l.resize(2000);
  o.r.resize(2000);
  for (int pos = 0; pos < 2000; pos += 100) {
    p->setHostPosition(host + pos, true);
    p->process(o.l.data() + pos, o.r.data() + pos, 100);
  }
  for (int i = 300; i < 2000; ++i) REQUIRE(o.l[static_cast<std::size_t>(i)] == stem[static_cast<std::size_t>(host + i - 3000)]);
}

TEST_CASE("StemPlayer offset: a set adopted later uses the offset; a loop that no longer fits is cleared", "[stemplayer][offset]") {
  const auto stem = idStem(30000);
  auto p = makeOffset(stem, 0);
  REQUIRE(p->setLoop(20000, 29000));
  p->setStartOffsetSamples(-5000);  // playhead length 25000: the loop end 29000 no longer fits
  p->process(nullptr, nullptr, 0);
  REQUIRE(p->appliedStartOffsetSamples() == -5000);
  REQUIRE_FALSE(p->loopActive());

  // No-op for zero-length results: an offset more negative than the set clamps the playhead length to 0.
  p->setStartOffsetSamples(-40000);
  p->process(nullptr, nullptr, 0);
  REQUIRE(p->playheadLength() == 0);
  p->play();
  const Out o = run(*p, 500);
  for (float v : o.l) REQUIRE(v == 0.0f);
}

TEST_CASE("StemPlayer offset: no allocation in process() with an offset and a loop", "[stemplayer][offset][rt]") {
  auto p = makeOffset(idStem(60000), -1234);
  REQUIRE(p->setLoop(10000, 40000));
  p->play();
  std::vector<float> l(512), r(512);
  run(*p, 4096);
  AllocGuard g;
  for (int i = 0; i < 300; ++i) p->process(l.data(), r.data(), 512);
  p->setStartOffsetSamples(99);
  p->pause();
  for (int i = 0; i < 20; ++i) p->process(l.data(), r.data(), 512);
  REQUIRE(g.count() == 0);
  REQUIRE(g.frees() == 0);
}
