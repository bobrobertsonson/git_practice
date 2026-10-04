// Phase 5.1b: BS.1770-4 integrated loudness and StemSet::backingLoudnessLufs.
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

#include "sawblade/loudness.h"
#include "sawblade/stem_set.h"
#include "stem_test_util.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;

namespace {

std::vector<float> tone(double hz, double fs, double seconds, double peakDbfs) {
  return sine(hz, fs, static_cast<std::size_t>(seconds * fs), std::pow(10.0, peakDbfs / 20.0));
}

std::optional<double> measure(const std::vector<float>& l, const std::vector<float>& r, double fs) {
  return integratedLoudnessLufs(l.data(), r.data(), static_cast<std::int64_t>(l.size()), fs);
}

std::vector<float> cat(std::initializer_list<std::vector<float>> parts) {
  std::vector<float> o;
  for (const auto& p : parts) o.insert(o.end(), p.begin(), p.end());
  return o;
}

double magDb(const LoudnessBiquad& b, double hz, double fs) {
  const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * hz / fs), z2 = z1 * z1;
  return 20.0 * std::log10(std::abs((b.b0 + b.b1 * z1 + b.b2 * z2) / (1.0 + b.a1 * z1 + b.a2 * z2)));
}

double kMagDb(const KWeighting& k, double hz, double fs) { return magDb(k.shelf, hz, fs) + magDb(k.highpass, hz, fs); }

}  // namespace

TEST_CASE("Loudness: EBU Tech 3341 case 1 and a left-only sine", "[loudness]") {
  for (double fs : {48000.0, 44100.0}) {
    INFO(fs);
    const auto x = tone(1000.0, fs, 20.0, -23.0);
    const auto v = measure(x, x, fs);
    REQUIRE(v.has_value());
    REQUIRE(std::fabs(*v - (-23.0)) < 0.1);
  }
  const auto l = tone(997.0, 48000.0, 20.0, 0.0);
  const std::vector<float> silent(l.size(), 0.0f);
  const auto v = measure(l, silent, 48000.0);
  REQUIRE(v.has_value());
  REQUIRE(std::fabs(*v - (-3.01)) < 0.1);
}

TEST_CASE("Loudness: gating", "[loudness]") {
  const double fs = 48000.0;
  // 20 s sections: the blocks straddling a tone/silence edge (kept by the standard's -10 LU gate)
  // bias the result by roughly 0.7 dB-seconds / length, so shorter sections would exceed 0.05.
  const auto a = tone(1000.0, fs, 20.0, -23.0);
  const auto ref = measure(cat({a, a}), cat({a, a}), fs);
  REQUIRE(ref.has_value());

  // Long digital silence between two sections does not change the result.
  const std::vector<float> gap(static_cast<std::size_t>(20 * fs), 0.0f);
  const auto gapped = cat({a, gap, a});
  const auto vg = measure(gapped, gapped, fs);
  REQUIRE(vg.has_value());
  REQUIRE(std::fabs(*vg - *ref) < 0.05);

  const auto main20 = tone(1000.0, fs, 20.0, -23.0);
  const auto base = measure(main20, main20, fs);
  // -80 dBFS section: below the absolute gate.
  const auto quiet = cat({main20, tone(1000.0, fs, 10.0, -80.0)});
  const auto vq = measure(quiet, quiet, fs);
  REQUIRE(vq.has_value());
  REQUIRE(std::fabs(*vq - *base) < 0.1);
  // A tone 20 dB below the main section: below the relative gate.
  const auto low = cat({main20, tone(1000.0, fs, 10.0, -43.0)});
  const auto vl = measure(low, low, fs);
  REQUIRE(vl.has_value());
  REQUIRE(std::fabs(*vl - *base) < 0.1);
}

TEST_CASE("Loudness: silence and short signals give none", "[loudness]") {
  const std::vector<float> z(96000, 0.0f);
  REQUIRE_FALSE(measure(z, z, 48000.0).has_value());
  const auto s = tone(1000.0, 48000.0, 0.3, -10.0);  // shorter than 400 ms
  REQUIRE_FALSE(measure(s, s, 48000.0).has_value());
  REQUIRE_FALSE(integratedLoudnessLufs(nullptr, nullptr, 0, 48000.0).has_value());
}

TEST_CASE("Loudness: K-weighting design matches BS.1770-4 at 48 kHz and is rate independent", "[loudness]") {
  const KWeighting k = designKWeighting(48000.0);
  REQUIRE(std::fabs(k.shelf.b0 - 1.53512485958697) < 1e-6);
  REQUIRE(std::fabs(k.shelf.b1 - (-2.69169618940638)) < 1e-6);
  REQUIRE(std::fabs(k.shelf.b2 - 1.19839281085285) < 1e-6);
  REQUIRE(std::fabs(k.shelf.a1 - (-1.69065929318241)) < 1e-6);
  REQUIRE(std::fabs(k.shelf.a2 - 0.73248077421585) < 1e-6);
  REQUIRE(k.highpass.b0 == 1.0);
  REQUIRE(k.highpass.b1 == -2.0);
  REQUIRE(k.highpass.b2 == 1.0);
  REQUIRE(std::fabs(k.highpass.a1 - (-1.99004745483398)) < 1e-6);
  REQUIRE(std::fabs(k.highpass.a2 - 0.99007225036621) < 1e-6);

  const KWeighting k44 = designKWeighting(44100.0);
  for (double hz : {50.0, 100.0, 1000.0, 4000.0, 10000.0}) {
    INFO(hz);
    REQUIRE(std::fabs(kMagDb(k44, hz, 44100.0) - kMagDb(k, hz, 48000.0)) < 0.05);
  }
}

TEST_CASE("StemSet backing loudness: guitar excluded, silence none", "[loudness][stems]") {
  const double fs = 48000.0;
  const auto drums = tone(1000.0, fs, 2.0, -23.0);
  const auto bass = tone(100.0, fs, 2.0, -26.0);
  const auto loudGuitar = tone(500.0, fs, 2.0, -3.0);

  const StemSet a = mkSet(fs, {{StemKind::Drums, drums}, {StemKind::Bass, bass}});
  REQUIRE(a.backingLoudnessLufs.has_value());
  const StemSet b = mkSet(fs, {{StemKind::Drums, drums}, {StemKind::Bass, bass}, {StemKind::Guitar, loudGuitar}});
  REQUIRE(b.backingLoudnessLufs.has_value());
  REQUIRE(*b.backingLoudnessLufs == *a.backingLoudnessLufs);

  // The value is the loudness of the unity-gain sum of the non-guitar stems.
  std::vector<float> sum(drums.size());
  for (std::size_t i = 0; i < sum.size(); ++i) sum[i] = drums[i] + bass[i];
  REQUIRE(std::fabs(*a.backingLoudnessLufs - *measure(sum, sum, fs)) < 1e-9);

  const std::vector<float> z(96000, 0.0f);
  REQUIRE_FALSE(mkSet(fs, {{StemKind::Drums, z}}).backingLoudnessLufs.has_value());
  REQUIRE_FALSE(mkSet(fs, {{StemKind::Guitar, loudGuitar}}).backingLoudnessLufs.has_value());

  // The file loaders measure too.
  StemTempDir t;
  writeWavFloat32(t / "drums.wav", fs, drums);
  writeWavFloat32(t / "guitar.wav", fs, loudGuitar);
  const StemSet d = loadStemDirectory(t.dir, fs);
  REQUIRE(d.backingLoudnessLufs.has_value());
  REQUIRE(std::fabs(*d.backingLoudnessLufs - *measure(drums, drums, fs)) < 1e-9);
}
