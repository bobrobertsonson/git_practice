// Phase 7: modeled pedal blocks pedal.hm and pedal.ts (docs/specs/phase7_modeled_pedals.md).
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "alloc_guard.h"
#include "fft_util.h"
#include "pedal_fr_util.h"
#include "sawblade/adaa_clipper.h"
#include "sawblade/block_registry.h"
#include "sawblade/chain.h"
#include "sawblade/oversampler.h"
#include "sawblade/pedal_hm.h"
#include "sawblade/pedal_ts.h"
#include "sawblade/render.h"
#include "sawblade/wav_io.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = SAWBLADE_FIXTURES_DIR;

// ---- helpers --------------------------------------------------------------------------------
void run(Processor& p, std::vector<float>& x, int block) {
  for (std::size_t pos = 0; pos < x.size(); pos += static_cast<std::size_t>(block))
    p.process(x.data() + pos, static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos)));
}

// f snapped to the nearest bin centre of an N-point FFT at fs.
double binCentred(double f, double fs, std::size_t n) {
  return std::round(f * static_cast<double>(n) / fs) * fs / static_cast<double>(n);
}

// Sine through `p` (already prepared): `warm` samples discarded, the next `n` returned.
std::vector<float> sineThrough(Processor& p, double fs, double f, double ampLin, std::size_t warm, std::size_t n,
                               int block = 512) {
  auto x = sine(f, fs, warm + n, ampLin);
  run(p, x, block);
  return std::vector<float>(x.begin() + static_cast<std::ptrdiff_t>(warm), x.end());
}

double magAt(const std::vector<std::complex<double>>& spec, std::size_t k) { return std::abs(spec[k]); }

// THD (harmonics 2..20 over the fundamental), dB; also H2 in dBc.
struct Thd {
  double thdDb, h2Dbc;
};
Thd measureThd(Processor& p, double levelDbfs = -20.0, double fs = 48000.0) {
  constexpr std::size_t N = 65536;
  const double f0 = binCentred(500.0, fs, N);
  p.prepare({fs, 512});
  const auto y = sineThrough(p, fs, f0, std::pow(10.0, levelDbfs / 20.0), 48000, N);
  const auto spec = fftReal(y, N);
  const std::size_t k0 = static_cast<std::size_t>(std::llround(f0 * static_cast<double>(N) / fs));
  double s = 0.0;
  for (std::size_t h = 2; h <= 20; ++h) s += std::pow(magAt(spec, h * k0), 2.0);
  const double fund = magAt(spec, k0);
  return {10.0 * std::log10(s / (fund * fund)), toDb(magAt(spec, 2 * k0) / fund)};
}

// Aliasing (spec 3): 5 kHz bin-centred sine at -6 dBFS, 1 s warm-up, Blackman-Harris window,
// largest line in 20 Hz..20 kHz outside +-200 Hz of a harmonic of 5 kHz, relative to the fundamental.
double measureAliasDb(Processor& p, double fs = 48000.0) {
  constexpr std::size_t N = 32768;
  const double f0 = binCentred(5000.0, fs, N);
  p.prepare({fs, 512});
  auto y = sineThrough(p, fs, f0, std::pow(10.0, -6.0 / 20.0), 48000, N);
  std::vector<std::complex<double>> a(N);
  for (std::size_t i = 0; i < N; ++i) {
    const double ph = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(N);
    const double w = 0.35875 - 0.48829 * std::cos(ph) + 0.14128 * std::cos(2 * ph) - 0.01168 * std::cos(3 * ph);
    a[i] = static_cast<double>(y[i]) * w;
  }
  fft(a);
  const double binHz = fs / static_cast<double>(N);
  const std::size_t k0 = static_cast<std::size_t>(std::llround(f0 / binHz));
  double fund = 0.0;
  for (std::size_t k = k0 - 4; k <= k0 + 4; ++k) fund = std::max(fund, std::abs(a[k]));
  double worst = 0.0;
  for (std::size_t k = 1; k < N / 2; ++k) {
    const double f = static_cast<double>(k) * binHz;
    if (f < 20.0 || f > 20000.0) continue;
    const double nearest = std::round(f / 5000.0);
    if (nearest >= 1.0 && std::fabs(f - nearest * 5000.0) <= 200.0) continue;
    worst = std::max(worst, std::abs(a[k]));
  }
  return toDb(worst / fund);
}

HmParams hmAlias() {
  HmParams q = HmParams::v2();
  q.distortion = 10;
  q.low = q.high = 5;
  return q;
}
TsParams tsAlias() {
  TsParams q;
  q.drive = 10;
  q.tone = 10;
  return q;
}

struct Fr {
  std::vector<double> db;
  double at(double f) const { return frAt(db, f); }
  double rel(double f) const { return at(f) - at(400.0); }
  // Maximum over [lo, hi] Hz; also reports whether it is an interior (local) maximum of the band.
  double maxIn(double lo, double hi, double* argHz = nullptr, bool* interior = nullptr) const {
    const std::size_t a = frBin(lo), b = frBin(hi);
    std::size_t best = a;
    for (std::size_t k = a; k <= b; ++k)
      if (db[k] > db[best]) best = k;
    if (argHz) *argHz = static_cast<double>(best) * kFrFs / static_cast<double>(kFrN);
    if (interior) *interior = best > a && best < b;
    return db[best];
  }
};

Fr hmFr(double low, double high, double dist = 5.0, double level = 5.0) {
  HmParams q = HmParams::v2();
  q.low = low;
  q.high = high;
  q.distortion = dist;
  q.level = level;
  HmPedal p(q);
  return {smallSignalResponseDb(p)};
}
Fr tsFr(double drive, double tone, double level = 5.0) {
  TsParams q;
  q.drive = drive;
  q.tone = tone;
  q.level = level;
  TsPedal p(q);
  return {smallSignalResponseDb(p)};
}

// Adapter: the colour-mix EQ section alone as a Processor.
class ColorEqProc : public Processor {
 public:
  ColorEqProc(double low, double high) : low_(low), high_(high) {}
  void prepare(const ProcessSpec& s) override { eq_.configure(s.sampleRate, low_, high_); }
  void reset() override { eq_.reset(); }
  void process(float* io, int n) noexcept override { eq_.process(io, n); }

 private:
  double low_, high_;
  HmColorEq eq_;
};

}  // namespace

// ---- Oversampler4x -----------------------------------------------------------------------------
namespace {

double halfBandDb(const std::vector<double>& h, double f) {  // f in cycles/sample at the stage's high rate
  const int c = (static_cast<int>(h.size()) - 1) / 2;
  std::complex<double> s = 0.0;
  for (std::size_t j = 0; j < h.size(); ++j)
    s += static_cast<double>(static_cast<float>(h[j])) * std::polar(1.0, -2.0 * std::numbers::pi * f * (static_cast<int>(j) - c));
  return 20.0 * std::log10(std::abs(s));
}

}  // namespace

TEST_CASE("Oversampler4x: half-band stages meet passband ripple and >=100 dB stopband", "[pedal][oversampler]") {
  const double beta = Oversampler4x::kaiserBetaFor(Oversampler4x::kDesignStopbandDb);
  struct S {
    int taps;
    double passEdge, stopEdge;  // cycles/sample at the stage's high rate
  };
  // fs-fractions: pass 0.4167 fs, stop 0.5833 fs (stage 1, high rate 2 fs); stage 2 (high rate 4 fs):
  // pass 0.4167 fs, images from 2 fs - 0.4167 fs.
  const S stages[] = {{Oversampler4x::kStage1Taps, 0.4166667 / 2.0, 0.5833333 / 2.0},
                      {Oversampler4x::kStage2Taps, 0.4166667 / 4.0, (2.0 - 0.4166667) / 4.0}};
  int idx = 1;
  for (const S& s : stages) {
    const auto h = Oversampler4x::designHalfBand(s.taps, beta);
    double sum = 0.0;
    for (double v : h) sum += v;
    REQUIRE(sum == Catch::Approx(1.0).margin(1e-12));
    REQUIRE(h[static_cast<std::size_t>((s.taps - 1) / 2)] == 0.5);
    double maxRipple = 0.0, minStop = 1e9;
    for (int i = 0; i <= 4000; ++i) {
      const double fp = s.passEdge * i / 4000.0;
      maxRipple = std::max(maxRipple, std::fabs(halfBandDb(h, fp)));
      const double fq = s.stopEdge + (0.5 - s.stopEdge) * i / 4000.0;
      minStop = std::min(minStop, -halfBandDb(h, fq));
    }
    std::printf("[oversampler] stage %d: %d taps, passband ripple %.2e dB, min stopband attenuation %.1f dB\n", idx++, s.taps, maxRipple, minStop);
    CHECK(maxRipple <= 0.005);
    CHECK(minStop >= 100.0);
  }
}

TEST_CASE("Oversampler4x: round trip is flat to 0.4167 fs; imaging and aliasing >= 100 dB down", "[pedal][oversampler]") {
  Oversampler4x os;
  os.prepare(4096);
  // Round trip (impulse): ripple over 0..0.4167 fs.
  {
    constexpr std::size_t N = 65536;
    std::vector<float> x(N, 0.0f), up(4 * 4096), y(N, 0.0f);
    x[0] = 1.0f;
    for (std::size_t pos = 0; pos < N; pos += 4096) {
      os.upsample(x.data() + pos, 4096, up.data());
      os.downsample(up.data(), 4096, y.data() + pos);
    }
    const auto spec = fftReal(y, N);
    double maxDev = 0.0;
    for (std::size_t k = 0; k <= static_cast<std::size_t>(0.4166667 * N); ++k) maxDev = std::max(maxDev, std::fabs(20.0 * std::log10(std::abs(spec[k]))));
    std::printf("[oversampler] round-trip passband ripple (0..0.4167 fs) = %.5f dB peak\n", maxDev);
    CHECK(maxDev <= 0.05);
  }
  // Imaging: bin-centred tones in the passband, all images (everything outside +-2 bins) <= -100 dB.
  {
    constexpr std::size_t Nb = 8192;
    double worst = -300.0;
    for (double frac : {0.05, 0.2, 0.3, 0.4, 0.4166}) {
      const std::size_t kb = static_cast<std::size_t>(std::llround(frac * Nb));
      std::vector<float> x(2 * Nb), y(8 * Nb);
      for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * static_cast<double>(kb) * static_cast<double>(i) / Nb));
      os.reset();
      for (std::size_t pos = 0; pos < x.size(); pos += 4096) os.upsample(x.data() + pos, 4096, y.data() + 4 * pos);
      std::vector<float> tail(y.end() - static_cast<std::ptrdiff_t>(4 * Nb), y.end());
      const auto spec = fftReal(tail, 4 * Nb);
      const double fund = std::abs(spec[kb]);
      for (std::size_t k = 1; k < 2 * Nb; ++k) {
        if (k + 2 >= kb && k <= kb + 2) continue;
        worst = std::max(worst, toDb(std::abs(spec[k]) / fund));
      }
    }
    std::printf("[oversampler] worst image level (tones up to 0.4166 fs) = %.1f dB\n", worst);
    CHECK(worst <= -100.0);
  }
  // Decimation: tones in the stopband regions [0.5833, 1.4167] fs and [1.5833, 1.95] fs of the 4x
  // signal must not reach the output (anywhere) above -100 dB.
  {
    constexpr std::size_t Nb = 8192;
    double worst = -300.0;
    for (double frac : {0.6, 0.8, 1.0, 1.2, 1.4, 1.6, 1.8, 1.95}) {
      const std::size_t kb = static_cast<std::size_t>(std::llround(frac * Nb));
      std::vector<float> x(8 * Nb), y(2 * Nb);
      for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * static_cast<double>(kb) * static_cast<double>(i) / (4.0 * Nb)));
      os.reset();
      for (std::size_t pos = 0; pos < y.size(); pos += 2048) os.downsample(x.data() + 4 * pos, 2048, y.data() + pos);
      std::vector<float> tail(y.end() - static_cast<std::ptrdiff_t>(Nb), y.end());
      const auto spec = fftReal(tail, Nb);
      for (std::size_t k = 0; k <= Nb / 2; ++k) worst = std::max(worst, toDb(std::abs(spec[k]) / (static_cast<double>(Nb) / 2.0)));
    }
    std::printf("[oversampler] worst decimation alias level (stopband tones) = %.1f dB\n", worst);
    CHECK(worst <= -100.0);
  }
}

TEST_CASE("Oversampler4x: reported round-trip delay is exact and chunking does not change a bit", "[pedal][oversampler]") {
  // Round trip = 198 samples at 4 fs = 49.5 base samples (a pedal pads and adds ADAA to reach a whole number).
  static_assert(Oversampler4x::roundTripLatencyOs() == 198);
  Oversampler4x a, b;
  a.prepare(512);
  b.prepare(512);
  const auto x = noise(5000, 3, 0.5f);
  std::vector<float> ya(5000), yb(5000), up(4 * 512), upb(4 * 512);
  for (std::size_t pos = 0; pos < x.size();) {
    const int n = static_cast<int>(std::min<std::size_t>(512, x.size() - pos));
    a.upsample(x.data() + pos, n, up.data());
    a.downsample(up.data(), n, ya.data() + pos);
    pos += static_cast<std::size_t>(n);
  }
  const int sizes[] = {1, 7, 64, 3, 500};
  std::size_t pos = 0, k = 0;
  while (pos < x.size()) {
    const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(sizes[k++ % 5]), x.size() - pos));
    b.upsample(x.data() + pos, n, upb.data());
    b.downsample(upb.data(), n, yb.data() + pos);
    pos += static_cast<std::size_t>(n);
  }
  REQUIRE(ya == yb);
  // A base-rate impulse: the 4x stream peaks at the one-way delay.
  Oversampler4x c;
  c.prepare(64);
  std::vector<float> imp(64, 0.0f), up4(256, 0.0f), all;
  imp[0] = 1.0f;
  c.upsample(imp.data(), 64, up4.data());
  const auto peak = std::max_element(up4.begin(), up4.end()) - up4.begin();
  REQUIRE(peak == Oversampler4x::kOneWayLatencyOs);  // 2*43 + 13 = 99 samples at 4 fs
}

// ---- AdaaClipper ----------------------------------------------------------------------------
TEST_CASE("SoftClipShape: C1 shape, continuous antiderivatives with exact derivatives", "[pedal][adaa]") {
  SoftClipShape s;
  s.kPos = 0.45;
  s.kNeg = 0.30;
  const double h = 1e-6;
  for (double u : {-2.0, -0.31, -0.30, -0.29, -0.1, -1e-3, 0.0, 1e-3, 0.1, 0.44, 0.45, 0.46, 1.0, 5.0}) {
    // F1' = c, F2' = F1 (central differences; points straddling the knees included)
    CHECK((s.f1(u + h) - s.f1(u - h)) / (2 * h) == Catch::Approx(s.f(u)).margin(1e-6));
    CHECK((s.f2(u + h) - s.f2(u - h)) / (2 * h) == Catch::Approx(s.f1(u)).margin(1e-6));
  }
  CHECK(s.f(0.0) == 0.0);
  CHECK(s.f1(0.0) == 0.0);
  CHECK(s.f2(0.0) == 0.0);
  CHECK((s.f(h) - s.f(-h)) / (2 * h) == Catch::Approx(1.0).margin(1e-5));  // slope 1 at 0
  CHECK(s.f(10.0) == Catch::Approx(2.0 * 0.45 / 3.0));
  CHECK(s.f(-10.0) == Catch::Approx(-2.0 * 0.30 / 3.0));
  // C1: slope 0 at both knees, value continuous
  CHECK((s.f(0.45 + h) - s.f(0.45 - h)) / (2 * h) == Catch::Approx(0.0).margin(1e-5));
  CHECK((s.f(-0.30 + h) - s.f(-0.30 - h)) / (2 * h) == Catch::Approx(0.0).margin(1e-5));
  // continuity of F1 and F2 across 0 and +-k
  for (double k : {0.45, -0.30, 0.0}) {
    CHECK(s.f1(k + 1e-12) == Catch::Approx(s.f1(k - 1e-12)).margin(1e-10));
    CHECK(s.f2(k + 1e-12) == Catch::Approx(s.f2(k - 1e-12)).margin(1e-10));
  }
  // symmetric when k+ = k-
  SoftClipShape sym;
  sym.kPos = sym.kNeg = 0.5;
  for (double u : {0.1, 0.4, 0.5, 2.0}) {
    CHECK(sym.f(-u) == -sym.f(u));
    CHECK(sym.f1(-u) == sym.f1(u));
    CHECK(sym.f2(-u) == -sym.f2(u));
  }
}

TEST_CASE("AdaaClipper: ADAA2 behaviour, fallbacks and one sample of delay", "[pedal][adaa]") {
  AdaaClipper c;
  c.setShape(0.5, 0.5);
  REQUIRE(c.latencySamples() == 1);
  // Linear region: the 3-tap mean (x[n]+x[n-1]+x[n-2])/3 up to the cubic term and the eps midpoint
  // approximation of D (error <= eps/12 ~ 8e-7 absolute, only when |x[n]-x[n-1]| < eps).
  {
    c.reset();
    const auto x = noise(2000, 5, 1e-3f);
    double maxErr = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      const double y = c.processSample(x[i]);
      if (i < 2) continue;
      maxErr = std::max(maxErr, std::fabs(y - (static_cast<double>(x[i]) + x[i - 1] + x[i - 2]) / 3.0));
    }
    CHECK(maxErr < 1e-6);
  }
  // Slowly varying signal: output is the static curve delayed by one sample.
  {
    c.reset();
    const auto x = sine(100.0, 192000.0, 4000, 2.0);
    double maxErr = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      const double y = c.processSample(x[i]);
      if (i < 3) continue;
      maxErr = std::max(maxErr, std::fabs(y - c.shape().f(x[i - 1])));
    }
    CHECK(maxErr < 2e-3);
  }
  // Constant input (all branches degenerate): y = c(const), also deep in saturation and exactly 0.
  for (float v : {0.0f, 0.1f, 0.7f, -3.0f}) {
    c.reset();
    float y = 0.0f;
    for (int i = 0; i < 8; ++i) y = c.processSample(v);
    CHECK(y == Catch::Approx(c.shape().f(v)).margin(1e-7));
  }
  // x[n] == x[n-2], x[n-1] different: the limit branch must agree with the regular formula just outside eps.
  {
    AdaaClipper a, b;
    a.setShape(0.5, 0.5);
    b.setShape(0.5, 0.5);
    for (double p : {0.2, 0.9, -0.4}) {
      for (double q : {0.6, -0.3, 2.0}) {
        a.reset();
        b.reset();
        a.processSample(static_cast<float>(p));
        a.processSample(static_cast<float>(q));
        const float inLimit = a.processSample(static_cast<float>(p));  // |x[n]-x[n-2]| = 0 < eps
        b.processSample(static_cast<float>(p));
        b.processSample(static_cast<float>(q));
        const float outside = b.processSample(static_cast<float>(p + 3e-5));  // just outside eps
        CHECK(inLimit == Catch::Approx(outside).margin(2e-3));
      }
    }
  }
  // ADAA off: memoryless, no delay.
  AdaaClipper n;
  n.setShape(0.5, 0.5);
  n.setAdaa(false);
  REQUIRE(n.latencySamples() == 0);
  CHECK(n.processSample(0.3f) == Catch::Approx(n.shape().f(0.3)).margin(1e-7));
}

// ---- 1. frequency response at the control extremes ----------------------------------------------
TEST_CASE("pedal.hm colour-mix EQ section matches the biquad table", "[pedal][fr]") {
  struct Lh {
    double low, high;
  };
  for (const Lh k : {Lh{0, 0}, Lh{10, 10}, Lh{5, 5}}) {
    ColorEqProc eq(k.low, k.high);
    const Fr fr{smallSignalResponseDb(eq)};
    // Independent copy of the spec table.
    const double g[5] = {-12.0 + 3.0 * k.low, -8.0 + 2.2 * k.high, -8.0 + 2.2 * k.high, 8.0, 0.0};
    const double f0[5] = {100.0, 1000.0, 1500.0, 4800.0, 9000.0};
    const double q[5] = {0.8, 1.2, 1.2, 2.0, 0.707};
    const auto expected = [&](double f) {
      double db = 0.0;
      for (int i = 0; i < 5; ++i) {
        EqBand b;
        b.type = i == 4 ? EqType::LowPass : EqType::Peak;
        b.freq = f0[i];
        b.gainDb = g[i];
        b.q = q[i];
        db += biquadMagnitudeDb(designBiquad(b, kFrFs), f, kFrFs);
      }
      return db;
    };
    for (double f : {50.0, 100.0, 400.0, 1000.0, 1250.0, 1500.0, 4800.0, 12000.0}) {
      const double want = expected(f) - expected(400.0), got = fr.rel(f);
      INFO("low=" << k.low << " high=" << k.high << " f=" << f << " want " << want << " got " << got);
      CHECK(std::fabs(got - want) <= 0.5);
    }
  }
}

TEST_CASE("pedal.hm whole-block frequency response", "[pedal][fr]") {
  const Fr max = hmFr(10, 10, 10);
  double hzLo, hzMid, hzPres;
  bool inLo, inMid, inPres;
  const double lo = max.maxIn(80, 130, &hzLo, &inLo) - max.at(400);
  const double mid = max.maxIn(1000, 1600, &hzMid, &inMid) - max.at(400);
  const double presMax = max.maxIn(3500, 6000, &hzPres, &inPres);
  const double pres = presMax - max.at(3500);
  const double r12 = max.at(12000) - max.maxIn(1000, 1600);
  std::printf("[fr] hm 10/10/10: low peak %.2f dB at %.1f Hz; mid peak %.2f dB at %.1f Hz; presence peak %.2f dB at %.1f Hz "
              "(%.2f dB above |H(3.5k)|, %.2f dB rel 400); |H(12k)| - mid max = %.1f dB\n",
              lo, hzLo, mid, hzMid, presMax - max.at(400), hzPres, pres, presMax - max.at(400), r12);
  CHECK(inLo);
  CHECK(lo >= 8.0);
  CHECK(inMid);
  CHECK(mid >= 12.0);
  CHECK(inPres);
  CHECK(pres >= 1.0);
  CHECK(r12 <= -10.0);

  const Fr min = hmFr(0, 0, 10);
  std::printf("[fr] hm 0/0/10: |H(100)|-|H(400)| = %.2f dB, |H(1250)|-|H(400)| = %.2f dB\n", min.rel(100), min.rel(1250));
  CHECK(min.rel(100) <= -6.0);
  CHECK(min.rel(1250) <= -6.0);
}

TEST_CASE("pedal.hm knobs are isolated", "[pedal][fr]") {
  // Isolation is measured as the ABSOLUTE change: "changes |H(f)| by X dB" is the difference of the
  // absolute small-signal magnitude between the two knob settings (lead-approved). (Not 400-relative: the 100 Hz gyrator's skirt moves |H(400)| by ~4 dB, which
  // would be charged to every other frequency; see the report.)
  const Fr l0 = hmFr(0, 5), l10 = hmFr(10, 5);
  const double dLow100 = l10.at(100) - l0.at(100), dLow1250 = l10.at(1250) - l0.at(1250);
  const Fr h0 = hmFr(5, 0), h10 = hmFr(5, 10);
  const double dHigh1250 = h10.at(1250) - h0.at(1250), dHigh100 = h10.at(100) - h0.at(100);
  std::printf("[fr] hm isolation (absolute): low 0->10: |H(100)| %+.2f dB, |H(1250)| %+.2f dB; high 0->10: |H(1250)| %+.2f dB, |H(100)| %+.2f dB "
              "(400-relative: %+.2f %+.2f %+.2f %+.2f)\n",
              dLow100, dLow1250, dHigh1250, dHigh100, l10.rel(100) - l0.rel(100), l10.rel(1250) - l0.rel(1250),
              h10.rel(1250) - h0.rel(1250), h10.rel(100) - h0.rel(100));
  CHECK(dLow100 >= 25.0);
  CHECK(std::fabs(dLow1250) <= 1.0);
  CHECK(dHigh1250 >= 25.0);
  CHECK(std::fabs(dHigh100) <= 1.5);
}

TEST_CASE("pedal.ts frequency response", "[pedal][fr]") {
  const Fr d10 = tsFr(10, 0);
  double hz;
  bool inside;
  const double pk = d10.maxIn(600, 1000, &hz, &inside);
  std::printf("[fr] ts drive 10 tone 0: peak %.2f dB (rel 400) at %.1f Hz; 100 Hz %.2f dB below the peak; 5 kHz %.2f dB below the peak\n",
              pk - d10.at(400), hz, pk - d10.at(100), pk - d10.at(5000));
  CHECK(inside);
  CHECK(d10.at(100) <= pk - 10.0);
  CHECK(d10.at(5000) <= pk - 12.0);

  const double toneSpan = tsFr(5, 10).at(4000) - tsFr(5, 0).at(4000);
  std::printf("[fr] ts tone 10 minus tone 0 at 4 kHz (drive 5): %.2f dB\n", toneSpan);
  CHECK(toneSpan >= 12.0);

  const Fr d0 = tsFr(0, 5);
  std::printf("[fr] ts drive 0 tone 5: |H(1000)| - |H(100)| = %.2f dB\n", d0.at(1000) - d0.at(100));
  CHECK(d0.at(100) <= d0.at(1000) - 10.0);
}

// ---- 2. THD vs gain ---------------------------------------------------------------------------------
namespace {

std::vector<Thd> thdSweep(bool hm, double levelDbfs) {
  std::vector<Thd> t;
  for (int k = 0; k <= 10; ++k) {
    if (hm) {
      HmParams q = HmParams::v2();
      q.distortion = k;
      HmPedal p(q);
      t.push_back(measureThd(p, levelDbfs));
    } else {
      TsParams q;
      q.drive = k;
      TsPedal p(q);
      t.push_back(measureThd(p, levelDbfs));
    }
  }
  return t;
}

void printThd(const char* name, double levelDbfs, const std::vector<Thd>& t) {
  std::printf("[thd] %s (500 Hz, %.0f dBFS, THD = harmonics 2-20 / fundamental)\n[thd]  knob:", name, levelDbfs);
  for (int k = 0; k <= 10; ++k) std::printf(" %7d", k);
  std::printf("\n[thd]  THD dB:");
  for (const auto& v : t) std::printf(" %7.2f", v.thdDb);
  std::printf("\n[thd]  H2 dBc:");
  for (const auto& v : t) std::printf(" %7.2f", v.h2Dbc);
  std::printf("\n");
}

}  // namespace

// Spec 2 (as amended): THD is monotonic in the gain knob at -20 and -40 dBFS, the span
// THD(10) - THD(0) is >= 6 dB at -40 dBFS (at -20 dBFS both models are already near saturated THD at
// the minimum knob, so no span is asserted there), and TS H2 at drive 10 is > -60 dBc.
TEST_CASE("THD is monotonic in the gain knob; span >= 6 dB at -40 dBFS", "[pedal][thd]") {
  for (double lvl : {-20.0, -40.0}) {
    for (bool hm : {true, false}) {
      const auto t = thdSweep(hm, lvl);
      printThd(hm ? "pedal.hm distortion" : "pedal.ts drive", lvl, t);
      for (std::size_t k = 1; k < t.size(); ++k) {
        INFO((hm ? "hm" : "ts") << " " << lvl << " dBFS step " << k);
        CHECK(t[k].thdDb >= t[k - 1].thdDb - 0.05);
      }
      if (lvl == -40.0) CHECK(t[10].thdDb - t[0].thdDb >= 6.0);
      if (!hm && lvl == -20.0) CHECK(t[10].h2Dbc > -60.0);
    }
  }
}

// ---- 3. aliasing ------------------------------------------------------------------------------------
TEST_CASE("aliasing is below -80 dB with OS+ADAA and the test detects its absence", "[pedal][alias]") {
  struct Cfg {
    const char* name;
    PedalImplConfig c;
  };
  const Cfg cfgs[] = {{"OS+ADAA (shipping)", {true, true, false}},
                      {"OS only, no ADAA", {true, false, false}},
                      {"ADAA only, no OS", {false, true, false}},
                      {"neither (naive)", {false, false, false}}};
  double hmShip = 0.0, tsShip = 0.0, hmNaive = 0.0, tsNaive = 0.0;
  for (const Cfg& c : cfgs) {
    HmPedal hm(hmAlias(), c.c);
    TsPedal ts(tsAlias(), c.c);
    const double a = measureAliasDb(hm), b = measureAliasDb(ts);
    std::printf("[alias] %-20s pedal.hm %7.1f dB   pedal.ts %7.1f dB\n", c.name, a, b);
    if (c.c.oversample && c.c.adaa) {
      hmShip = a;
      tsShip = b;
    }
    if (!c.c.oversample && !c.c.adaa) {
      hmNaive = a;
      tsNaive = b;
    }
  }
  CHECK(hmShip < -80.0);
  CHECK(tsShip < -80.0);
  CHECK(hmNaive > -80.0);
  CHECK(tsNaive > -80.0);
}

// ---- 4. latency -------------------------------------------------------------------------------------
namespace {

// Reported latency vs the measured peak of the -90 dBFS impulse response. With flatFilters every
// linear filter is bypassed, so the small-signal response is (up FIR) * (ADAA 3-tap mean) * (down FIR)
// plus pure delays: symmetric about the total delay by construction, and the total is a whole
// number of base-rate samples, so its peak sits exactly on the reported sample. (Documented choice,
// spec 4: "peak of the impulse response on the reported sample, exact by construction".)
int measuredPeak(Processor& p, double fs) {
  p.prepare({fs, 512});
  std::vector<float> x(2048, 0.0f);
  x[0] = static_cast<float>(std::pow(10.0, -90.0 / 20.0));
  run(p, x, 512);
  std::size_t best = 0;
  for (std::size_t i = 0; i < x.size(); ++i)
    if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
  return static_cast<int>(best);
}

}  // namespace

TEST_CASE("latencySamples() equals the measured delay", "[pedal][latency]") {
  PedalImplConfig flat;
  flat.flatFilters = true;
  for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
    HmPedal hm({}, flat);
    TsPedal ts({}, flat);
    const int mh = measuredPeak(hm, fs), mt = measuredPeak(ts, fs);
    std::printf("[latency] fs %.0f: pedal.hm reported %d measured %d; pedal.ts reported %d measured %d\n", fs, hm.latencySamples(), mh,
                ts.latencySamples(), mt);
    CHECK(hm.latencySamples() == 50);
    CHECK(ts.latencySamples() == 50);
    CHECK(mh == hm.latencySamples());
    CHECK(mt == ts.latencySamples());
    // the shipping pedals report the same latency (the filters are not counted)
    CHECK(HmPedal({}).latencySamples() == hm.latencySamples());
    CHECK(TsPedal({}).latencySamples() == ts.latencySamples());
  }
}

namespace {

void registerFlatPedals() {
  static const bool once = [] {
    PedalImplConfig flat;
    flat.flatFilters = true;
    BlockType h;
    h.parse = [](JsonObject&, const fs::path&) -> std::shared_ptr<const BlockParams> { return std::make_shared<HmBlockParams>(); };
    h.create = [flat](const Block& b, const BlockBuildContext&) -> std::unique_ptr<Processor> {
      return std::make_unique<HmPedal>(static_cast<const HmBlockParams&>(*b.params).p, flat);
    };
    BlockRegistry::instance().add("test.pedal_hm_flat", h);
    BlockType t;
    t.parse = [](JsonObject&, const fs::path&) -> std::shared_ptr<const BlockParams> { return std::make_shared<TsBlockParams>(); };
    t.create = [flat](const Block& b, const BlockBuildContext&) -> std::unique_ptr<Processor> {
      return std::make_unique<TsPedal>(static_cast<const TsBlockParams&>(*b.params).p, flat);
    };
    BlockRegistry::instance().add("test.pedal_ts_flat", t);
    return true;
  }();
  (void)once;
}

json chainPreset(const std::string& typeA, double blend) {
  json a = {{"blocks", typeA.empty() ? json::array() : json::array({{{"id", "a1"}, {"type", typeA}}})}};
  return {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "t"},
          {"paths", {{"a", a}, {"b", {{"blocks", json::array()}}}}},
          {"align", {{"mode", "off"}}}, {"blend", blend},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}}};
}

std::unique_ptr<Chain> buildChain(const json& j, int maxBlock = 512) {
  const Preset p = parsePreset(j, kFixtures / "presets");
  auto c = std::make_unique<Chain>(p, loadResources(p, 48000.0));
  c->prepare({48000.0, maxBlock});
  return c;
}

}  // namespace

TEST_CASE("Chain compensates a pedal path against an empty path", "[pedal][latency][chain]") {
  registerFlatPedals();
  for (const std::string type : {"pedal.hm", "pedal.ts", "test.pedal_hm_flat", "test.pedal_ts_flat"}) {
    auto chain = buildChain(chainPreset(type, 0.5));
    const ChainInfo info = chain->info();
    INFO(type);
    REQUIRE(info.pathLatency[0] == 50);
    REQUIRE(info.pathLatency[1] == 0);
    REQUIRE(info.compensationDelay[1] == 50);
    REQUIRE(info.latencySamples == 50);
    REQUIRE(chain->latencySamples() == 50);
  }
  // Impulse through both paths arrives aligned: the A-only, B-only and blended outputs all peak
  // at the reported latency (flat test variants, whose A response is symmetric about it).
  for (const std::string type : {"test.pedal_hm_flat", "test.pedal_ts_flat"}) {
    std::size_t peaks[3];
    int i = 0;
    for (double blend : {0.0, 1.0, 0.5}) {
      auto chain = buildChain(chainPreset(type, blend));
      std::vector<float> x(1024, 0.0f), y(1024, 0.0f);
      x[0] = 3e-5f;  // -90 dBFS: linear region
      chain->process(x.data(), y.data(), 1024);
      peaks[i++] = static_cast<std::size_t>(std::max_element(y.begin(), y.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) - y.begin());
    }
    INFO(type);
    CHECK(peaks[0] == 50);
    CHECK(peaks[1] == 50);
    CHECK(peaks[2] == 50);
  }
}

// ---- 5. zero allocation -----------------------------------------------------------------------------
TEST_CASE("pedal process() does not allocate", "[pedal][alloc]") {
  HmPedal hm({});
  TsPedal ts({});
  for (Processor* p : {static_cast<Processor*>(&hm), static_cast<Processor*>(&ts)}) {
    p->prepare({48000.0, 512});
    auto x = noise(8000, 61, 0.5f);
    const int sizes[] = {512, 1, 64, 480, 7, 128, 300, 33, 512, 256, 100, 2, 1};
    AllocGuard g;
    std::size_t pos = 0;
    for (int n : sizes) {
      p->process(x.data() + pos, n);
      pos += static_cast<std::size_t>(n);
    }
    REQUIRE(g.count() == 0);
    for (std::size_t i = 0; i < pos; ++i) REQUIRE(std::isfinite(x[i]));
  }
}

TEST_CASE("Chain with both pedals does not allocate in process()", "[pedal][alloc][chain]") {
  json j = chainPreset("", 0.5);
  j["paths"]["a"]["blocks"] = json::array({{{"id", "a1"}, {"type", "pedal.ts"}, {"params", {{"drive", 3}}}}, {{"id", "a2"}, {"type", "pedal.hm"}}});
  j["paths"]["b"]["blocks"] = json::array({{{"id", "b1"}, {"type", "pedal.hm"}, {"params", {{"distortion", 10}}}}});
  auto chain = buildChain(j);
  auto x = noise(6000, 62, 0.4f);
  std::vector<float> y(1000);
  const int sizes[] = {512, 1, 64, 480, 7, 128, 300, 33, 512, 256, 100, 2, 1000};
  AllocGuard g;
  std::size_t pos = 0;
  for (int n : sizes) {
    chain->process(x.data() + pos, y.data(), n);
    pos += static_cast<std::size_t>(n);
  }
  REQUIRE(g.count() == 0);
  for (float v : y) REQUIRE(std::isfinite(v));
}

// ---- 6/7. block-size independence and determinism ----------------------------------------------------
namespace {

AudioFile firstSeconds(double sec) {
  AudioFile f = readWav(kFixtures / "di_riff.wav");
  const std::size_t frames = std::min<std::size_t>(f.interleaved.size() / static_cast<std::size_t>(f.channels),
                                                    static_cast<std::size_t>(sec * f.sampleRate));
  f.interleaved.resize(frames * static_cast<std::size_t>(f.channels));
  return f;
}

}  // namespace

TEST_CASE("pedal renders are bit-identical across block sizes and runs", "[pedal][blocksize]") {
  const AudioFile in = firstSeconds(5.0);
  for (const char* name : {"hm_chainsaw.json", "ts_boost.json"}) {
    const Preset p = loadPresetFile(fs::path(SAWBLADE_PRESETS_DIR) / "modeled" / name);
    RenderOptions o;
    o.blockSize = 512;
    const auto ref = renderPreset(p, in, o).samples;
    REQUIRE(!ref.empty());
    for (int bs : {1, 7, 64, 512, 4096}) {
      o.blockSize = bs;
      const auto y = renderPreset(p, in, o).samples;
      INFO(name << " block " << bs);
      REQUIRE(y.size() == ref.size());
      REQUIRE(y == ref);  // tolerance 0
    }
    o.blockSize = 512;
    REQUIRE(renderPreset(p, in, o).samples == ref);  // determinism: second render of the same preset
  }
}

// ---- 8. preset round trip ----------------------------------------------------------------------------
namespace {

json blockPreset(const json& blocks) {
  json j = chainPreset("", 0.0);
  j["paths"]["a"]["blocks"] = blocks;
  return j;
}
Preset parseP(const json& j) { return parsePreset(j, kFixtures / "presets"); }

}  // namespace

TEST_CASE("pedal block presets round-trip and reject bad values", "[pedal][preset]") {
  const json explicitBlocks = json::array({
      {{"id", "a1"}, {"type", "pedal.hm"}, {"slot", "pedal"}, {"modelVersion", 1}, {"params", {{"level", 5.5}, {"low", 10}, {"high", 0.25}, {"distortion", 10}}}},
      {{"id", "b1"}, {"type", "pedal.ts"}, {"slot", "boost"}, {"modelVersion", 1}, {"params", {{"drive", 2}, {"tone", 6}, {"level", 8}}}}});
  const json implicitBlocks = json::array({{{"id", "a1"}, {"type", "pedal.hm"}}, {{"id", "b1"}, {"type", "pedal.ts"}, {"params", {{"drive", 7}}}}});
  for (const json& blocks : {explicitBlocks, implicitBlocks}) {
    const Preset p = parseP(blockPreset(blocks));
    const json out = toJson(p);
    const Preset q = parseP(out);
    REQUIRE(p == q);
    // toJson always writes modelVersion and every param
    const json& b0 = out["paths"]["a"]["blocks"][0];
    REQUIRE(b0["modelVersion"] == 2);  // pedal.hm writes the v2 object (every key) even when read from v1
    REQUIRE(b0["params"].size() == 19);
    REQUIRE(out["paths"]["a"]["blocks"][1]["params"].size() == 3);
    REQUIRE(toJson(q) == out);  // exact
  }
  // defaults are 5
  const Preset d = parseP(blockPreset(implicitBlocks));
  const auto& hm = static_cast<const HmBlockParams&>(*d.a.blocks[0].params).p;
  CHECK(hm.level == 5.0);
  CHECK(hm.low == 5.0);
  CHECK(hm.high == 5.0);
  CHECK(hm.distortion == 5.0);
  const auto& ts = static_cast<const TsBlockParams&>(*d.a.blocks[1].params).p;
  CHECK(ts.drive == 7.0);
  CHECK(ts.tone == 5.0);

  const auto bad = [](const json& block) { return blockPreset(json::array({block})); };
  const json base = {{"id", "a1"}, {"type", "pedal.hm"}};
  const auto with = [&](const char* k, json v, const std::string& type = "pedal.hm") {
    json b = base;
    b["type"] = type;
    b[k] = std::move(v);
    return b;
  };
  CHECK_THROWS_AS(parseP(bad(with("modelVersion", 4))), PresetError);
  CHECK_THROWS_AS(parseP(bad(with("modelVersion", 2, "pedal.ts"))), PresetError);
  CHECK_THROWS_AS(parseP(bad(with("modelVersion", 0))), PresetError);
  CHECK_THROWS_AS(parseP(bad(with("params", {{"distortion", 11}}))), PresetError);
  CHECK_THROWS_AS(parseP(bad(with("params", {{"distortion", -0.5}}))), PresetError);
  CHECK_THROWS_AS(parseP(bad(with("params", {{"drive", 10.5}}, "pedal.ts"))), PresetError);
  CHECK_THROWS_AS(parseP(bad(with("params", {{"foo", 1}}))), PresetError);
  CHECK_THROWS_AS(parseP(bad(with("params", {{"drive", 1}}))), PresetError);  // a ts key on pedal.hm
  CHECK_THROWS_AS(parseP(bad(with("params", {{"distortion", "10"}}))), PresetError);
  CHECK_THROWS_AS(parseP(bad(with("params", 5))), PresetError);
  CHECK_THROWS_AS(parseP(bad(with("params", json::array()))), PresetError);
  CHECK_THROWS_AS(parseP(bad(with("params", nullptr))), PresetError);
  CHECK_THROWS_AS(parseP(bad(with("bogus", 1))), PresetError);
  try {
    parseP(bad(with("params", {{"distortion", 11}})));
    FAIL("expected PresetError");
  } catch (const PresetError& e) {
    CHECK(e.jsonPath() == "paths.a.blocks[0].params.distortion");
  }
}

TEST_CASE("pedal block types are registered and NAM-trainable", "[pedal][preset]") {
  for (const char* t : {"pedal.hm", "pedal.ts"}) {
    const BlockType* bt = BlockRegistry::instance().find(t);
    REQUIRE(bt != nullptr);
    CHECK(bt->traits.namTrainable);
  }
}

// ---- 9. example presets -------------------------------------------------------------------------------
TEST_CASE("every presets/modeled/*.json renders the fixture DI to finite output", "[pedal][presets]") {
  int count = 0;
  for (const auto& e : fs::directory_iterator(fs::path(SAWBLADE_PRESETS_DIR) / "modeled")) {
    if (e.path().extension() != ".json") continue;
    ++count;
    INFO(e.path().string());
    RenderResult r;
    REQUIRE_NOTHROW(r = renderFile(e.path(), kFixtures / "di_riff.wav"));
    REQUIRE(!r.samples.empty());
    double peak = 0.0;
    for (float v : r.samples) {
      REQUIRE(std::isfinite(v));
      peak = std::max(peak, static_cast<double>(std::fabs(v)));
    }
    CHECK(peak > 1e-3);  // not silent
    CHECK(r.info.latencySamples > 0);
    CHECK(r.captures.empty());  // no TONE3000 captures
  }
  CHECK(count >= 2);
}
