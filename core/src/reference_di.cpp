#include "sawblade/reference_di.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// NOTE: no libm call and no fused multiply-add here (CMake sets -ffp-contract=off on this file): see the header.
namespace sawblade {
namespace {

struct Rng {  // xorshift64*; uniform in [-1, 1)
  std::uint64_t s;
  double next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    const std::uint64_t r = s * 0x2545F4914F6CDD1Dull;
    return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0;
  }
};

constexpr int kRate = 48000;
constexpr int kSteps = 80;       // sixteenth notes at 120 bpm: 6000 samples each
constexpr int kStepSamples = 6000;

// Loss per period: palm-muted (60 dB in ~0.2 s), semi-muted, open ring (about 3 s).
constexpr double kMute = 0.72, kSemi = 0.88, kOpen = 0.985;

struct Chord {
  int period[3];  // strings (whole-sample periods at 48 kHz); a single note has n = 1
  int n;
};
constexpr Chord kE5{{582, 389, 291}, 3}, kD5{{654, 436, 327}, 3}, kC5{{734, 490, 367}, 3}, kG5{{490, 327, 245}, 3},
    kA5{{436, 291, 218}, 3}, kF5{{550, 367, 275}, 3};
constexpr Chord kE2{{582}, 1}, kD2{{654}, 1}, kC2{{734}, 1}, kA2{{436}, 1}, kE3{{291}, 1};

// One Karplus-Strong string added into `out` at `start`: excitation = one period of seeded noise, one-pole low-passed
// (softer pick) and made zero-mean; loop y[n] = g * 0.5 * (y[n-N] + y[n-N-1]); rung for at most `len` samples.
void pluck(std::vector<double>& out, std::size_t start, int N, double g, double amp, std::size_t len, Rng& rng) {
  if (start >= out.size()) return;
  len = std::min(len, out.size() - start);
  std::vector<double> y(len, 0.0);
  const auto period = static_cast<std::size_t>(N);
  double lp = 0.0, mean = 0.0;
  for (std::size_t i = 0; i < period && i < len; ++i) {
    lp = 0.6 * lp + 0.4 * rng.next();
    y[i] = lp;
    mean += lp;
  }
  mean /= static_cast<double>(period);
  for (std::size_t i = 0; i < period && i < len; ++i) y[i] -= mean;
  for (std::size_t i = period; i < len; ++i) y[i] = g * 0.5 * (y[i - period] + (i > period ? y[i - period - 1] : 0.0));
  for (std::size_t i = 0; i < len; ++i) out[start + i] += amp * y[i];
}

// A strum of `c` at sixteenth-note `step`: strings 0..n-1 offset by 3 ms each (down-strum), `vel` scales the hit.
void hit(std::vector<double>& out, Rng& rng, int step, const Chord& c, double g, double vel, double seconds) {
  const std::size_t start = static_cast<std::size_t>(step) * static_cast<std::size_t>(kStepSamples);
  const auto len = static_cast<std::size_t>(seconds * static_cast<double>(kRate));
  for (int j = 0; j < c.n; ++j) pluck(out, start + static_cast<std::size_t>(j) * 144u, c.period[j], g, vel / static_cast<double>(c.n), len, rng);
}

std::vector<float> generate() {
  const auto total = static_cast<std::size_t>(kSteps) * static_cast<std::size_t>(kStepSamples);
  std::vector<double> x(total, 0.0);
  Rng rng{0x9E3779B97F4A7C15ull * 7ull};

  // Bar 1 (steps 0-15): E, palm-muted gallop, power chord accents on the downbeats.
  for (int s : {0, 1, 3, 4, 6, 8, 9, 11, 12, 14}) hit(x, rng, s, kE2, kMute, (s % 8 == 0) ? 0.9 : 0.7, 0.45);
  hit(x, rng, 0, kE5, kMute, 0.8, 0.5);
  hit(x, rng, 8, kE5, kMute, 0.8, 0.5);
  // Bar 2 (16-31): D then C, muted, a semi-muted accent.
  for (int s : {16, 17, 19, 20, 22}) hit(x, rng, s, kD2, kMute, (s == 16) ? 0.9 : 0.65, 0.45);
  for (int s : {24, 25, 27, 28, 30}) hit(x, rng, s, kC2, kMute, (s == 24) ? 0.9 : 0.65, 0.45);
  hit(x, rng, 16, kD5, kSemi, 0.75, 0.6);
  hit(x, rng, 24, kC5, kSemi, 0.75, 0.6);
  // Bar 3 (32-47): open power chords, left to ring.
  hit(x, rng, 32, kE5, kOpen, 0.85, 2.0);
  hit(x, rng, 40, kD5, kOpen, 0.8, 2.0);
  hit(x, rng, 44, kG5, kOpen, 0.75, 1.0);
  // Bar 4 (48-63): tremolo burst on E3, then an open chord.
  for (int s = 48; s < 56; ++s) hit(x, rng, s, kE3, kMute, 0.45 + 0.05 * static_cast<double>(s % 3), 0.3);
  hit(x, rng, 56, kA5, kOpen, 0.85, 1.5);
  hit(x, rng, 60, kF5, kSemi, 0.7, 0.8);
  // Bar 5 (64-79): muted A chugs, then the last open E chord rings out.
  for (int s : {64, 65, 67, 68, 70}) hit(x, rng, s, kA2, kMute, (s == 64) ? 0.9 : 0.7, 0.45);
  hit(x, rng, 64, kA5, kMute, 0.8, 0.5);
  hit(x, rng, 72, kE5, kOpen, 0.9, 2.0);

  double peak = 0.0;
  for (double v : x) peak = std::max(peak, std::fabs(v));
  // 10^(kReferenceDiPeakDbfs / 20), written as a literal so that no libm call is involved.
  constexpr double kPeak = 0.31622776601683794;
  const double scale = peak > 0.0 ? kPeak / peak : 0.0;
  std::vector<float> out(total);
  for (std::size_t i = 0; i < total; ++i) out[i] = static_cast<float>(x[i] * scale);
  return out;
}

}  // namespace

const std::vector<float>& referenceDi() {
  static const std::vector<float> kSignal = generate();
  return kSignal;
}

}  // namespace sawblade
