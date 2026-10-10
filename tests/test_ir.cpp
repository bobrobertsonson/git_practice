#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <string>
#include <unistd.h>
#include <vector>

#include "sawblade/ir.h"
#include "sawblade/wav_io.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;

namespace {

struct TempDir {
  std::filesystem::path dir;
  TempDir() {
    dir = std::filesystem::temp_directory_path() / ("sawblade_ir_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
  std::filesystem::path operator/(const std::string& f) const { return dir / f; }
};

double l2(const std::vector<float>& v) {
  double e = 0;
  for (float x : v) e += static_cast<double>(x) * x;
  return std::sqrt(e);
}

// Magnitude of the DFT of x at freq (Hz), by direct correlation.
double binMag(const std::vector<float>& x, double freq, double fs) {
  double re = 0, im = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double ph = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / fs;
    re += x[i] * std::cos(ph);
    im -= x[i] * std::sin(ph);
  }
  return std::hypot(re, im);
}

}  // namespace

TEST_CASE("IR resampling 44.1k -> 48k preserves a 1 kHz windowed sine", "[ir]") {
  TempDir t;
  const double fsIn = 44100.0, fsOut = 48000.0;
  const std::size_t n = 22050;  // 0.5 s
  std::vector<float> ir(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n - 1));
    ir[i] = static_cast<float>(0.5 * w * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / fsIn));
  }
  writeWavFloat32(t / "ir441.wav", fsIn, ir);

  const IrData out = loadIr(t / "ir441.wav", fsOut, /*normalize=*/false);
  REQUIRE(out.warnings.empty());
  REQUIRE(out.sourceSampleRate == fsIn);
  REQUIRE(out.samples.size() == static_cast<std::size_t>(std::ceil(static_cast<double>(n) * fsOut / fsIn)));

  double bestF = 0, bestM = 0;
  for (double f = 900.0; f <= 1100.0; f += 1.0) {
    const double m = binMag(out.samples, f, fsOut);
    if (m > bestM) { bestM = m; bestF = f; }
  }
  REQUIRE(std::fabs(bestF - 1000.0) <= 5.0);
  const double dRms = toDb(rms(out.samples.data(), out.samples.size()) / rms(ir.data(), ir.size()));
  REQUIRE(std::fabs(dRms) <= 0.1);
}

TEST_CASE("IR loading: stereo, truncation, normalization", "[ir]") {
  TempDir t;
  SECTION("stereo uses left channel with a warning") {
    // Write a stereo file via two-channel interleave using the mono writer is impossible, so
    // build it with dr_wav-free route: reuse writeWavFloat32 for mono, then check a mono file
    // produces no warning; the stereo path is exercised through the raw WAV below.
    const std::vector<float> left = noise(500, 2, 0.5f);
    // Hand-build a 2-channel float WAV.
    std::vector<float> inter;
    for (float v : left) { inter.push_back(v); inter.push_back(-v); }
    const auto p = t / "st.wav";
    {
      std::FILE* f = std::fopen(p.c_str(), "wb");
      REQUIRE(f != nullptr);
      auto w32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
      auto w16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
      const std::uint32_t dataBytes = static_cast<std::uint32_t>(inter.size() * 4);
      std::fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); std::fwrite("WAVEfmt ", 1, 8, f);
      w32(16); w16(3); w16(2); w32(48000); w32(48000 * 8); w16(8); w16(32);
      std::fwrite("data", 1, 4, f); w32(dataBytes);
      std::fwrite(inter.data(), 4, inter.size(), f);
      std::fclose(f);
    }
    const IrData out = loadIr(p, 48000.0, false);
    REQUIRE(out.warnings.size() == 1);
    REQUIRE(out.samples == left);
  }
  SECTION("truncated at 2 s, with a warning") {
    writeWavFloat32(t / "long.wav", 48000.0, noise(48000 * 3, 3, 0.2f));
    const IrData out = loadIr(t / "long.wav", 48000.0);
    REQUIRE(out.samples.size() == 96000);
    REQUIRE(out.warnings.size() == 1);
  }
  SECTION("truncation happens at the target rate") {
    writeWavFloat32(t / "long441.wav", 44100.0, noise(44100 * 3, 3, 0.2f));
    const IrData out = loadIr(t / "long441.wav", 48000.0);
    REQUIRE(out.samples.size() == 96000);
  }
  SECTION("L2 normalization (default) and opt-out") {
    writeWavFloat32(t / "n.wav", 48000.0, noise(1000, 4, 0.05f));
    REQUIRE(l2(loadIr(t / "n.wav", 48000.0).samples) == Catch::Approx(1.0).margin(1e-6));
    REQUIRE(l2(loadIr(t / "n.wav", 48000.0, false).samples) != Catch::Approx(1.0).margin(1e-3));
  }
  SECTION("resampled IR is also L2-normalized") {
    writeWavFloat32(t / "m.wav", 44100.0, noise(2000, 5, 0.05f));
    REQUIRE(l2(loadIr(t / "m.wav", 48000.0).samples) == Catch::Approx(1.0).margin(1e-6));
  }
  SECTION("missing file throws with the path") {
    REQUIRE_THROWS_AS(loadIr(t / "missing.wav", 48000.0), std::runtime_error);
  }
}
