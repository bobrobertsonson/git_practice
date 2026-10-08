// v0.8 I4b part 1: the stereo DI chooser (core/include/sawblade/stereo_input.h). Everything is synthetic.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/stereo_input.h"

using namespace sawblade;
using sawblade::test::AllocGuard;

namespace {

constexpr double kFs = 48000.0;

// A played guitar-like channel: 0.4 s of a 300 Hz tone (peak 0.1 = -20 dBFS, 5 ms raised-cosine ramps) every 0.5 s, silence between.
// Both channels also carry -85 dBFS noise, so "silent" is a noise floor, not digital zero.
struct Source {
  double lGain = 1.0, rGain = 1.0;  // linear, relative to the 0.1 tone
  std::mt19937 rng{7};
  std::uniform_real_distribution<float> noise{-1.0f, 1.0f};
  long n = 0;
  double playedS = 0.0;  // seconds of the tone that were produced (burst on, ramps included)

  static double env(double t) {  // t within the 0.5 s period
    constexpr double on = 0.4, ramp = 0.005;
    if (t >= on) return 0.0;
    if (t < ramp) return 0.5 - 0.5 * std::cos(std::numbers::pi * t / ramp);
    if (t > on - ramp) return 0.5 - 0.5 * std::cos(std::numbers::pi * (on - t) / ramp);
    return 1.0;
  }
  void block(std::vector<float>& l, std::vector<float>& r, int len) {
    l.resize(static_cast<std::size_t>(len));
    r.resize(static_cast<std::size_t>(len));
    for (int i = 0; i < len; ++i, ++n) {
      const double t = static_cast<double>(n) / kFs;
      const double e = env(std::fmod(t, 0.5));
      const double tone = 0.1 * e * std::sin(2.0 * std::numbers::pi * 300.0 * t);
      constexpr float nz = 5.6e-5f;  // -85 dBFS peak
      l[static_cast<std::size_t>(i)] = static_cast<float>(tone * lGain) + nz * noise(rng);
      r[static_cast<std::size_t>(i)] = static_cast<float>(tone * rGain) + nz * noise(rng);
      if (e > 0.0) playedS += 1.0 / kFs;
    }
  }
};

struct Run {
  StereoInputChooser c;
  Source src;
  int block;
  std::vector<float> l, r, out;
  float prev = 0.0f;
  float maxStep = 0.0f;
  explicit Run(int blk = 256, InputChannelMode m = InputChannelMode::Auto) : block(blk) {
    l.reserve(8192);
    r.reserve(8192);
    out.reserve(8192);
    c.setMode(m);
    c.prepare(kFs);
  }
  // Runs `seconds` of audio; calls f(playedS) after each block.
  template <class F>
  void go(double seconds, F&& f) {
    const long total = static_cast<long>(seconds * kFs);
    for (long done = 0; done < total;) {
      const int len = static_cast<int>(std::min<long>(block, total - done));
      src.block(l, r, len);
      out.resize(l.size());
      c.process(l.data(), r.data(), out.data(), len);
      for (int i = 0; i < len; ++i) {
        maxStep = std::max(maxStep, std::fabs(out[static_cast<std::size_t>(i)] - prev));
        prev = out[static_cast<std::size_t>(i)];
      }
      done += len;
      f(src.playedS);
    }
  }
  void go(double seconds) { go(seconds, [](double) {}); }
};

double rms(const std::vector<float>& x) {
  double s = 0.0;
  for (float v : x) s += static_cast<double>(v) * v;
  return std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, x.size())));
}

}  // namespace

TEST_CASE("Stereo input: silent R latches L within 3 s of played audio, at L's level (not -6 dB)", "[stereo]") {
  Run run;
  run.src.rGain = 0.0;
  double latchedAt = -1.0;
  run.go(6.0, [&](double played) {
    if (latchedAt < 0.0 && run.c.decision() == InputChannel::Left) latchedAt = played;
  });
  REQUIRE(latchedAt > 0.0);
  CHECK(latchedAt >= 1.9);  // about 2 s of played frames first
  CHECK(latchedAt <= 3.0);
  // After the fade the output is L, sample for sample, not 0.5 * L.
  run.go(1.0);
  run.src.block(run.l, run.r, 4800);
  run.out.resize(run.l.size());
  run.c.process(run.l.data(), run.r.data(), run.out.data(), 4800);
  CHECK(run.out == run.l);
  CHECK(rms(run.out) == Catch::Approx(rms(run.l)).epsilon(1e-6));
  CHECK(inputChannelText(run.c.mode(), run.c.decision()) == "Input: L only (auto)");
}

TEST_CASE("Stereo input: silent L latches R", "[stereo]") {
  Run run;
  run.src.lGain = 0.0;
  run.go(6.0);
  CHECK(run.c.decision() == InputChannel::Right);
  run.go(1.0);
  run.src.block(run.l, run.r, 4800);
  run.out.resize(run.l.size());
  run.c.process(run.l.data(), run.r.data(), run.out.data(), 4800);
  CHECK(run.out == run.r);
  CHECK(inputChannelText(run.c.mode(), run.c.decision()) == "Input: R only (auto)");
}

TEST_CASE("Stereo input: both channels active (within 30 dB) stays Mix, the old 0.5 * (L + R)", "[stereo]") {
  Run run;
  run.src.rGain = 0.1;  // 20 dB below L: not a one-channel source
  run.go(30.0);
  CHECK(run.c.decision() == InputChannel::Mix);
  run.src.block(run.l, run.r, 2400);
  run.out.resize(run.l.size());
  run.c.process(run.l.data(), run.r.data(), run.out.data(), 2400);
  for (std::size_t i = 0; i < run.l.size(); ++i) REQUIRE(run.out[i] == 0.5f * (run.l[i] + run.r[i]));
  CHECK(inputChannelText(run.c.mode(), run.c.decision()) == "Input: L+R mix (auto)");
}

TEST_CASE("Stereo input: the played test is the drift tap's - a channel difference in silence does not latch", "[stereo]") {
  Run run;
  run.src.lGain = 0.0;
  run.src.rGain = 0.0;  // only the -85 dBFS noise
  run.go(20.0);
  CHECK(run.c.decision() == InputChannel::Mix);
}

TEST_CASE("Stereo input: a reversal switches only after 10 s of played frames, faded without a step", "[stereo]") {
  Run run;
  run.src.rGain = 0.0;
  run.go(6.0);
  REQUIRE(run.c.decision() == InputChannel::Left);
  // The guitar moves to R (L silent). Start the count at the next gap so the swap lands between notes.
  while (std::fmod(static_cast<double>(run.src.n) / kFs, 0.5) > 0.41) run.go(0.01);
  run.src.lGain = 0.0;
  run.src.rGain = 1.0;
  const double t0 = run.src.playedS;
  double switchedAt = -1.0;
  run.maxStep = 0.0;
  run.go(14.0, [&](double played) {
    if (switchedAt < 0.0 && run.c.decision() == InputChannel::Right) switchedAt = played - t0;
  });
  REQUIRE(switchedAt > 0.0);
  CHECK(switchedAt >= 10.0 - 0.01);  // not before 10 s of played frames
  CHECK(switchedAt <= 11.5);          // and soon after
  // No step bigger than the tone's own slope plus the ramp's share of a full-scale swing over >= 20 ms.
  const float toneStep = static_cast<float>(0.1 * 2.0 * std::numbers::pi * 300.0 / kFs);
  const float rampStep = 0.1f / static_cast<float>(0.020 * kFs);
  CHECK(run.maxStep <= toneStep + rampStep + 2e-4f);  // + the -85 dBFS noise
  run.go(1.0);
  run.src.block(run.l, run.r, 4800);
  run.out.resize(run.l.size());
  run.c.process(run.l.data(), run.r.data(), run.out.data(), 4800);
  CHECK(run.out == run.r);
}

TEST_CASE("Stereo input: a shorter reversal does not switch", "[stereo]") {
  Run run;
  run.src.rGain = 0.0;
  run.go(6.0);
  REQUIRE(run.c.decision() == InputChannel::Left);
  while (std::fmod(static_cast<double>(run.src.n) / kFs, 0.5) > 0.41) run.go(0.01);
  run.src.lGain = 0.0;
  run.src.rGain = 1.0;
  run.go(8.0);  // ~6.4 s played
  CHECK(run.c.decision() == InputChannel::Left);
  run.src.lGain = 1.0;  // back to L before 10 s: the count starts over
  run.go(20.0);
  CHECK(run.c.decision() == InputChannel::Left);
}

TEST_CASE("Stereo input: forced modes take that channel with no detection", "[stereo]") {
  const struct {
    InputChannelMode m;
    InputChannel d;
  } cases[] = {{InputChannelMode::Left, InputChannel::Left}, {InputChannelMode::Right, InputChannel::Right}, {InputChannelMode::Mix, InputChannel::Mix}};
  for (const auto& cs : cases) {
    Run run(256, cs.m);
    run.src.rGain = 0.3;
    run.go(3.0);
    CHECK(run.c.decision() == cs.d);
    run.src.block(run.l, run.r, 2400);
    run.out.resize(run.l.size());
    run.c.process(run.l.data(), run.r.data(), run.out.data(), 2400);
    for (std::size_t i = 0; i < run.l.size(); ++i) {
      const float want = cs.d == InputChannel::Left ? run.l[i] : cs.d == InputChannel::Right ? run.r[i] : 0.5f * (run.l[i] + run.r[i]);
      REQUIRE(run.out[i] == want);
    }
    CHECK(inputChannelText(cs.m, cs.d).find("(forced)") != std::string::npos);
  }
}

TEST_CASE("Stereo input: a change from Settings fades over at least 20 ms", "[stereo]") {
  Run run(256, InputChannelMode::Left);
  run.src.rGain = 0.5;
  run.go(1.0);
  run.maxStep = 0.0;
  run.c.setMode(InputChannelMode::Right);
  run.go(1.0);
  CHECK(run.c.decision() == InputChannel::Right);
  const float toneStep = static_cast<float>(0.1 * 2.0 * std::numbers::pi * 300.0 / kFs);
  const float rampStep = 0.05f / static_cast<float>(0.020 * kFs);  // |L - R| <= 0.05 for these sources
  CHECK(run.maxStep <= toneStep + rampStep + 2e-4f);  // + the -85 dBFS noise
  CHECK(run.maxStep > 0.0f);
  // Back to Auto: starts as Mix again.
  run.c.setMode(InputChannelMode::Auto);
  run.go(0.2);
  CHECK(run.c.decision() == InputChannel::Mix);
}

TEST_CASE("Stereo input: prepare() and a restart request re-evaluate from scratch", "[stereo]") {
  Run run;
  run.src.rGain = 0.0;
  run.go(6.0);
  REQUIRE(run.c.decision() == InputChannel::Left);
  run.c.prepare(kFs);
  CHECK(run.c.decision() == InputChannel::Mix);
  run.go(6.0);
  REQUIRE(run.c.decision() == InputChannel::Left);
  run.c.requestRestart();
  run.src.lGain = 0.0;
  run.src.rGain = 1.0;
  run.go(0.1);  // the restart is taken at the next block ...
  run.go(6.0);  // ... and the stream is judged afresh
  CHECK(run.c.decision() == InputChannel::Right);
}

TEST_CASE("Stereo input: the latch does not depend on the block size", "[stereo]") {
  for (int blk : {32, 64, 480, 1024, 4096}) {
    Run run(blk);
    run.src.rGain = 0.0;
    run.go(6.0);
    INFO("block " << blk);
    CHECK(run.c.decision() == InputChannel::Left);
  }
}

TEST_CASE("Stereo input: no allocation in process() across a latch and a switch", "[stereo][alloc]") {
  Run run;
  run.src.rGain = 0.0;
  {
    AllocGuard g;
    run.go(6.0);  // latch L
    run.c.setMode(InputChannelMode::Right);
    run.go(0.5);  // forced switch
    run.c.setMode(InputChannelMode::Auto);
    run.c.requestRestart();
    run.src.lGain = 0.0;
    run.src.rGain = 1.0;
    run.go(6.0);  // latch R
    run.go(12.0);
    CHECK(g.count() == 0);
  }
  CHECK(run.c.decision() == InputChannel::Right);
}

TEST_CASE("Stereo input: mode names round trip", "[stereo]") {
  for (auto m : {InputChannelMode::Auto, InputChannelMode::Left, InputChannelMode::Right, InputChannelMode::Mix}) {
    const auto back = parseInputChannelMode(inputChannelModeName(m));
    REQUIRE(back.has_value());
    CHECK(*back == m);
  }
  CHECK_FALSE(parseInputChannelMode("both").has_value());
}
