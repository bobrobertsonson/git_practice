#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/eq.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;

namespace {

// Steady-state gain of `eq` at `freq`: 1 s warm-up, then RMS over a whole number of cycles
// (>= 40) of the input and output; returns dB(out/in).
double measureGainDb(ParametricEq& eq, double freq, double fs) {
  const auto warm = static_cast<std::size_t>(fs);
  const int cycles = std::max(40, static_cast<int>(std::ceil(2.0 * freq)));
  const auto win = static_cast<std::size_t>(std::llround(cycles * fs / freq));
  std::vector<float> x = sine(freq, fs, warm + win, 0.25);
  const std::vector<float> in = x;
  eq.reset();
  eq.process(x.data(), static_cast<int>(x.size()));
  return toDb(rms(x.data() + warm, win) / rms(in.data() + warm, win));
}

struct Case {
  const char* name;
  EqBand band;
};

}  // namespace

TEST_CASE("EQ magnitude response matches analytic and textbook values", "[eq]") {
  const std::vector<Case> cases = {
      {"peak +6", {EqType::Peak, 1200.0, 6.0, 1.0, true}},
      {"peak -9", {EqType::Peak, 1200.0, -9.0, 2.5, true}},
      {"lowShelf +6", {EqType::LowShelf, 120.0, 6.0, 0.7071, true}},
      {"lowShelf -4", {EqType::LowShelf, 120.0, -4.0, 0.7071, true}},
      {"highShelf -4", {EqType::HighShelf, 6000.0, -4.0, 0.7071, true}},
      {"highShelf +5", {EqType::HighShelf, 6000.0, 5.0, 0.7071, true}},
      {"highPass", {EqType::HighPass, 80.0, 0.0, 0.7071, true}},
      {"lowPass", {EqType::LowPass, 9000.0, 0.0, 0.7071, true}},
  };

  for (const double fs : {48000.0, 44100.0}) {
    for (const Case& c : cases) {
      DYNAMIC_SECTION(c.name << " @ " << fs) {
        ParametricEq eq;
        eq.configure(fs, std::vector<EqBand>{c.band});
        eq.prepare({fs, 512});

        const double f0 = c.band.freq;
        std::vector<double> freqs = {f0 / 8, f0 / 4, f0 / 2, f0, f0 * 2, f0 * 4, f0 * 8, 1000.0};
        freqs.erase(std::remove_if(freqs.begin(), freqs.end(), [&](double f) { return f < 20.0 || f > 0.45 * fs; }),
                    freqs.end());
        REQUIRE(freqs.size() >= 6);

        for (const double f : freqs) {
          const double measured = measureGainDb(eq, f, fs);
          INFO("freq " << f << " measured " << measured << " analytic " << eq.magnitudeDb(f));
          REQUIRE(std::fabs(measured - eq.magnitudeDb(f)) <= 0.1);
        }

        switch (c.band.type) {
          case EqType::Peak:
            REQUIRE(eq.magnitudeDb(f0) == Catch::Approx(c.band.gainDb).margin(0.05));
            break;
          case EqType::LowShelf:
            REQUIRE(eq.magnitudeDb(f0 / 40) == Catch::Approx(c.band.gainDb).margin(0.2));
            REQUIRE(eq.magnitudeDb(f0 * 40) == Catch::Approx(0.0).margin(0.2));
            break;
          case EqType::HighShelf:
            REQUIRE(eq.magnitudeDb(f0 * 3.2) == Catch::Approx(c.band.gainDb).margin(0.2));
            REQUIRE(eq.magnitudeDb(f0 / 40) == Catch::Approx(0.0).margin(0.2));
            break;
          case EqType::HighPass:
          case EqType::LowPass:
            REQUIRE(eq.magnitudeDb(f0) == Catch::Approx(-3.01).margin(0.05));
            REQUIRE(measureGainDb(eq, f0, fs) == Catch::Approx(-3.01).margin(0.05));
            break;
        }
      }
    }
  }
}

TEST_CASE("EQ cascade, disabled bands and magnitude sum", "[eq]") {
  const double fs = 48000.0;
  std::vector<EqBand> bands = {{EqType::Peak, 500.0, 6.0, 1.0, true},
                               {EqType::Peak, 500.0, 6.0, 1.0, false},
                               {EqType::HighShelf, 5000.0, -3.0, 0.7071, true}};
  ParametricEq eq;
  eq.configure(fs, bands);
  eq.prepare({fs, 256});
  REQUIRE(eq.numActiveBands() == 2);
  const double expect = biquadMagnitudeDb(designBiquad(bands[0], fs), 700.0, fs) +
                        biquadMagnitudeDb(designBiquad(bands[2], fs), 700.0, fs);
  REQUIRE(eq.magnitudeDb(700.0) == Catch::Approx(expect).margin(1e-9));
  REQUIRE(measureGainDb(eq, 700.0, fs) == Catch::Approx(expect).margin(0.1));

  // Empty EQ is bit-transparent.
  ParametricEq none;
  none.prepare({fs, 256});
  std::vector<float> x = noise(1000);
  const auto y = x;
  none.process(x.data(), 1000);
  REQUIRE(x == y);
}

TEST_CASE("EQ rejects invalid parameters at configure time", "[eq]") {
  const double fs = 48000.0;
  ParametricEq eq;
  auto cfg = [&](EqBand b) { eq.configure(fs, std::vector<EqBand>{b}); };
  EqBand ok{EqType::Peak, 1000.0, 3.0, 1.0, true};
  REQUIRE_NOTHROW(cfg(ok));

  auto with = [&](double f, double q) {
    EqBand b = ok;
    b.freq = f;
    b.q = q;
    return b;
  };
  REQUIRE_THROWS_AS(cfg(with(0.0, 1.0)), std::invalid_argument);
  REQUIRE_THROWS_AS(cfg(with(-10.0, 1.0)), std::invalid_argument);
  REQUIRE_THROWS_AS(cfg(with(0.49 * fs, 1.0)), std::invalid_argument);
  REQUIRE_THROWS_AS(cfg(with(0.6 * fs, 1.0)), std::invalid_argument);
  REQUIRE_NOTHROW(cfg(with(0.489 * fs, 1.0)));
  REQUIRE_THROWS_AS(cfg(with(1000.0, 0.0)), std::invalid_argument);
  REQUIRE_THROWS_AS(cfg(with(1000.0, -1.0)), std::invalid_argument);
  REQUIRE_THROWS_AS(cfg(with(std::nan(""), 1.0)), std::invalid_argument);
  REQUIRE_THROWS_AS(eq.configure(fs, std::vector<EqBand>(17, ok)), std::invalid_argument);
  REQUIRE_NOTHROW(eq.configure(fs, std::vector<EqBand>(16, ok)));

  // A failed configure leaves the previous configuration intact.
  eq.configure(fs, std::vector<EqBand>{ok});
  REQUIRE_THROWS(cfg(with(0.0, 1.0)));
  REQUIRE(eq.numActiveBands() == 1);

  // prepare() at a lower rate re-validates the stored bands.
  EqBand high = ok;
  high.freq = 15000.0;
  eq.configure(48000.0, std::vector<EqBand>{high});
  REQUIRE_THROWS_AS(eq.prepare({16000.0, 256}), std::invalid_argument);
}

TEST_CASE("EQ process performs no allocations", "[eq][alloc]") {
  const double fs = 48000.0;
  std::vector<EqBand> bands = {{EqType::HighPass, 80.0, 0.0, 0.7071, true},
                               {EqType::Peak, 1500.0, 4.0, 1.2, true},
                               {EqType::LowShelf, 100.0, 2.0, 0.7071, true},
                               {EqType::HighShelf, 6000.0, -4.0, 0.7071, true},
                               {EqType::LowPass, 9000.0, 0.0, 0.7071, true}};
  ParametricEq eq;
  eq.configure(fs, bands);
  eq.prepare({fs, 1024});
  std::vector<float> buf = noise(1024);
  const int sizes[] = {1, 7, 64, 333, 1024, 128, 2, 512, 17, 1000, 256};
  long allocs = -1;
  {
    AllocGuard g;
    for (const int n : sizes) eq.process(buf.data(), n);
    // reset() is checked here as an implementation property, not an interface contract.
    eq.reset();
    allocs = g.count();
  }
  REQUIRE(allocs == 0);
}
