// Real-time fixed-ratio polyphase resampler (core/include/sawblade/rt_resample.h).
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/resample.h"
#include "sawblade/rt_resample.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;

namespace {

struct RatePair {
  double from, to;
};
const RatePair kPairs[] = {{44100, 48000}, {48000, 44100}, {96000, 48000}, {48000, 96000},
                           {88200, 48000}, {48000, 88200}, {44100, 96000}, {192000, 48000},
                           {48000, 192000}, {22050, 48000}, {44100, 44100 * 2}};

// Runs x through `r` in blocks of the given (cycled) sizes.
std::vector<float> runRt(RtResampler& r, const std::vector<float>& x, const std::vector<int>& sizes) {
  std::vector<float> y;
  std::vector<float> tmp(static_cast<std::size_t>(r.maxOutputFor(8192)));
  std::size_t pos = 0, k = 0;
  while (pos < x.size()) {
    const auto n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(sizes[k++ % sizes.size()]), x.size() - pos));
    const int m = r.process(x.data() + pos, n, tmp.data());
    y.insert(y.end(), tmp.begin(), tmp.begin() + m);
    pos += static_cast<std::size_t>(n);
  }
  return y;
}

RtResampler make(double from, double to, int maxBlock = 4096) {
  RtResampler r;
  r.prepare(from, to, maxBlock);
  return r;
}

// Peak amplitude of the sinusoid at `freq` in x[i0, i1) by least squares.
double toneAmplitude(const std::vector<float>& x, double freq, double fs, std::size_t i0, std::size_t i1) {
  double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
  for (std::size_t i = i0; i < i1; ++i) {
    const double ph = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / fs;
    const double s = std::sin(ph), c = std::cos(ph), v = x[i];
    ss += s * s;
    cc += c * c;
    sc += s * c;
    xs += v * s;
    xc += v * c;
  }
  const double det = ss * cc - sc * sc;
  const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
  return std::hypot(a, b);
}

// Gain in dB of a tone at f (input rate `from`) measured at the output frequency fOut (the alias
// for a stopband tone); steady state, after the filter has settled.
double toneGainDb(double from, double to, double f, double fOut, double amp = 0.5) {
  RtResampler r = make(from, to);
  const auto n = static_cast<std::size_t>(from * 0.5);
  const auto x = sine(f, from, n, amp);
  const auto y = runRt(r, x, {512});
  // Whole number of 20 ms periods (integer cycles of every multiple of 50 Hz): no leakage of the
  // strong in-band tone into the alias/image measurement.
  const auto per = static_cast<std::size_t>(to / 50.0);  // 20 ms: integer cycles of every multiple of 50 Hz
  const std::size_t i0 = ((y.size() / 4 + per - 1) / per) * per;
  const std::size_t i1 = i0 + ((y.size() - i0 - y.size() / 8) / per) * per;
  return toDb(toneAmplitude(y, fOut, to, i0, i1) / amp);
}

}  // namespace

TEST_CASE("RtResampler: ratios are exact and reduced", "[rtresample]") {
  auto r = RtResampler::ratioFor(44100, 48000);
  CHECK(r.L == 160);
  CHECK(r.M == 147);
  r = RtResampler::ratioFor(48000, 44100);
  CHECK(r.L == 147);
  CHECK(r.M == 160);
  r = RtResampler::ratioFor(96000, 48000);
  CHECK(r.L == 1);
  CHECK(r.M == 2);
  // Non-integral rates: bounded rational approximation, reciprocal-consistent.
  r = RtResampler::ratioFor(44100.37, 48000);
  CHECK(r.L <= RtResampler::kMaxPhases);
  CHECK(std::fabs(static_cast<double>(r.L) / static_cast<double>(r.M) - 48000.0 / 44100.37) < 1e-6);
  CHECK_THROWS_AS(RtResampler::ratioFor(0, 48000), std::invalid_argument);
  CHECK_THROWS_AS(RtResampler::ratioFor(1000, 192000), std::invalid_argument);
  CHECK_THROWS_AS(RtResampler::ratioFor(std::nan(""), 48000), std::invalid_argument);
}

TEST_CASE("RtResampler matches the offline resampler", "[rtresample]") {
  // Same kernel: RT output j is the offline output of the input delayed by H samples.
  for (const auto& p : kPairs) {
    CAPTURE(p.from, p.to);
    auto r = make(p.from, p.to);
    const int H = r.wholeDelay();
    const auto x = noise(6000, 7, 0.4f);
    const auto y = runRt(r, x, {97, 1, 480, 33, 2048});
    REQUIRE(y.size() > 1000);
    std::vector<float> xd(static_cast<std::size_t>(H), 0.0f);
    xd.insert(xd.end(), x.begin(), x.end());
    const auto ref = resample(xd, p.from, p.to, y.size());
    double maxErr = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) maxErr = std::max(maxErr, static_cast<double>(std::fabs(y[i] - ref[i])));
    CHECK(maxErr < 2e-6);
    // The output count is what the timing model predicts: all j with floor((jM)/L) <= N-1 (s = 0).
    const auto ratio = r.ratio();
    const std::int64_t expected = (static_cast<std::int64_t>(x.size()) * ratio.L + ratio.M - 1) / ratio.M;  // ceil(N L / M)
    CHECK(static_cast<std::int64_t>(y.size()) == expected);
  }
}

TEST_CASE("RtResampler output is independent of the block sizes", "[rtresample]") {
  const auto x = noise(20000, 3, 0.5f);
  for (const auto& p : {RatePair{44100, 48000}, RatePair{48000, 44100}, RatePair{96000, 48000}}) {
    CAPTURE(p.from, p.to);
    auto r1 = make(p.from, p.to);
    auto r2 = make(p.from, p.to);
    auto r3 = make(p.from, p.to);
    const auto a = runRt(r1, x, {1});
    const auto b = runRt(r2, x, {4096});
    std::mt19937 g(5);
    std::vector<int> sizes;
    for (int i = 0; i < 64; ++i) sizes.push_back(1 + static_cast<int>(g() % 4096));
    const auto c = runRt(r3, x, sizes);
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == c.size());
    CHECK(a == b);
    CHECK(a == c);
  }
}

TEST_CASE("RtResampler process() allocates nothing", "[rtresample][rt]") {
  auto r = make(44100, 48000, 512);
  const auto x = noise(512, 1);
  std::vector<float> y(static_cast<std::size_t>(r.maxOutputFor(512)));
  r.process(x.data(), 512, y.data());
  AllocGuard guard;
  for (int i = 0; i < 200; ++i) r.process(x.data(), 1 + (i * 37) % 512, y.data());
  CHECK(guard.count() == 0);
}

TEST_CASE("RtResampler reset restarts the stream", "[rtresample]") {
  auto r = make(44100, 48000);
  const auto x = noise(3000, 9);
  const auto a = runRt(r, x, {128});
  r.reset();
  const auto b = runRt(r, x, {300});
  CHECK(a == b);
}

TEST_CASE("RtResampler frequency response vs the offline spec", "[rtresample][response]") {
  // The offline converter's spec (resample.h / README): passband flat within 0.05 dB to 20 kHz at
  // 44.1 <-> 48 kHz, stopband >= 90 dB. The RT converter uses the same kernel design with float
  // coefficients; the numbers below are what it achieves.
  double worstPass = 0.0, worstStop = -1000.0;
  std::vector<std::string> lines;
  auto passband = [&](double from, double to, std::initializer_list<double> freqs) {
    double worst = 0.0;
    for (double f : freqs) worst = std::max(worst, std::fabs(toneGainDb(from, to, f, f)));
    worstPass = std::max(worstPass, worst);
    char buf[160];
    std::snprintf(buf, sizeof buf, "passband %6.0f -> %6.0f Hz: worst |gain| %.4f dB", from, to, worst);
    lines.emplace_back(buf);
    return worst;
  };

  // Dense stopband sweep, 10 Hz steps (downsampling; 50 Hz for upsampling images). Downsampling: every tone from the stopband edge (1.04 x the
  // output Nyquist) up to the input Nyquist, measured where it folds to. Upsampling: every in-band
  // tone whose image (from - f) lies in the stopband, measured where the image lands.
  auto fold = [](double f, double to) {
    double a = std::fmod(f, to);
    return a > 0.5 * to ? to - a : a;
  };
  auto stopSweep = [&](double from, double to) {
    const double nyq = 0.5 * std::min(from, to);
    double worst = -1000.0, worstAt = 0.0;
    int count = 0;
    auto one = [&](double f, double fa) {
      const double g = toneGainDb(from, to, f, fa);
      ++count;
      if (g > worst) worst = g, worstAt = f;
    };
    if (to <= from) {
      for (double f = std::ceil(1.04 * nyq / 10.0) * 10.0; f < 0.5 * from - 10.0; f += 10.0) one(f, fold(f, to));
    } else {
      const double hi = std::min(0.9 * nyq, from - 1.04 * nyq);
      for (double f = 1000.0; f <= hi; f += 50.0) one(f, fold(from - f, to));
    }
    worstStop = std::max(worstStop, worst);
    char buf[200];
    std::snprintf(buf, sizeof buf, "stopband %6.0f -> %6.0f Hz: %d tones, worst alias/image %.1f dB (tone %.0f Hz)", from, to, count, worst, worstAt);
    lines.emplace_back(buf);
    return worst;
  };

  // 44.1 <-> 48 kHz: 20 kHz passband (spec: within 0.05 dB), stopband from 22.9 kHz (48 -> 44.1).
  CHECK(passband(44100, 48000, {50, 1000, 5000, 10000, 15000, 18000, 19800, 20000}) < 0.05);
  CHECK(passband(48000, 44100, {50, 1000, 5000, 10000, 15000, 18000, 19800, 20000}) < 0.05);
  CHECK(stopSweep(48000, 44100) < -90.0);
  CHECK(stopSweep(44100, 48000) < -90.0);
  CHECK(passband(96000, 48000, {100, 5000, 12000, 18000, 21000, 21500}) < 0.05);
  CHECK(stopSweep(96000, 48000) < -90.0);
  CHECK(passband(48000, 96000, {100, 5000, 12000, 18000, 21000, 21500}) < 0.05);
  CHECK(stopSweep(48000, 96000) < -90.0);
  CHECK(passband(88200, 48000, {100, 5000, 12000, 18000, 21000, 21500}) < 0.05);
  CHECK(stopSweep(88200, 48000) < -90.0);

  for (const auto& l : lines) WARN(l);
  WARN("worst passband deviation " << worstPass << " dB, worst stopband " << worstStop << " dB");
}

TEST_CASE("RtResampler latency is constant and the integer-delay helper is exact", "[rtresample][latency]") {
  for (const auto& p : kPairs) {
    CAPTURE(p.from, p.to);
    auto r = make(p.from, p.to);
    const auto ratio = r.ratio();
    // delayOutputSamples() is H*L/M (s = 0).
    CHECK(r.delayOutputSamples() == Catch::Approx(static_cast<double>(r.wholeDelay()) * static_cast<double>(ratio.L) / static_cast<double>(ratio.M)));
    // An impulse at k0 peaks at output index round((k0 + d) * L / M); a shift by M input samples
    // shifts the peak by exactly L output samples (constant latency).
    std::int64_t firstPeak = -1;
    for (int shift = 0; shift < 4; ++shift) {
      const std::size_t k0 = 500 + static_cast<std::size_t>(shift) * static_cast<std::size_t>(ratio.M);
      std::vector<float> x(k0 + 2000, 0.0f);
      x[k0] = 1.0f;
      r.reset();
      const auto y = runRt(r, x, {256, 1000, 77});
      const auto peak = static_cast<std::int64_t>(std::max_element(y.begin(), y.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) - y.begin());
      const double expected = (static_cast<double>(k0) + r.delayInputSamples()) * static_cast<double>(ratio.L) / static_cast<double>(ratio.M);
      CHECK(std::fabs(static_cast<double>(peak) - expected) <= 0.5);
      if (shift == 0) firstPeak = peak;
      CHECK(peak == firstPeak + shift * ratio.L);
    }
    // With the helper, extra upstream delay + this converter's delay is a whole number of output samples.
    for (std::int64_t extra = 0; extra < 400; extra += 7) {
      RtResampler q = make(p.from, p.to);
      q.setPhaseOffset(q.phaseOffsetForIntegerDelay(extra));
      const std::int64_t num = (extra + q.wholeDelay()) * ratio.L + q.phaseOffset();
      CHECK(num % ratio.M == 0);
      CHECK(q.phaseOffset() >= 0);
      CHECK(q.phaseOffset() < ratio.M);
    }
  }
}

TEST_CASE("RtResampler: every phase offset 0..M-1 works (including s > L)", "[rtresample][latency]") {
  // Regression: with s > L the first outputs' whole kernel lies before the stream start; the tap
  // count used to underflow and the converter read far outside its buffer.
  const RatePair pairs[] = {{48000, 44100}, {48000, 22050}, {48000, 11025}, {48000, 8000}, {48000, 16000},
                            {44100, 48000}, {48000, 96000}, {96000, 48000}, {48000, 32000}};
  for (const auto& p : pairs) {
    CAPTURE(p.from, p.to);
    RtResampler r = make(p.from, p.to, 700);
    const auto ratio = r.ratio();
    const std::size_t k0 = 300;
    std::vector<float> x(2200, 0.0f);
    x[k0] = 1.0f;
    for (std::int64_t s = 0; s < ratio.M; ++s) {
      r.setPhaseOffset(s);
      const auto y = runRt(r, x, {700, 33, 1});
      REQUIRE(y.size() > 100);
      bool finite = true;
      for (float v : y) finite = finite && std::isfinite(v);
      REQUIRE(finite);
      const auto peak = static_cast<double>(std::max_element(y.begin(), y.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) - y.begin());
      const double expected = (static_cast<double>(k0) + r.delayInputSamples()) * static_cast<double>(ratio.L) / static_cast<double>(ratio.M);
      REQUIRE(std::fabs(peak - expected) <= 0.5 + 1e-9);
      // The first outputs (kernel entirely before the stream) are exactly zero.
      REQUIRE(y[0] == 0.0f);
    }
  }
}
