#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/nam_block.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;

namespace {

std::filesystem::path fixture(const std::string& name) {
  return std::filesystem::path(SAWBLADE_FIXTURES_DIR) / "nam" / name;
}

std::unique_ptr<NamBlock> make(const std::string& file, double sr = 48000.0, int maxBlock = 512,
                               NamBlockConfig cfg = {}) {
  auto b = NamBlock::load(fixture(file), cfg);
  b->prepare({sr, maxBlock});
  return b;
}

std::vector<float> render(NamBlock& b, std::vector<float> x, int block) {
  std::size_t pos = 0;
  while (pos < x.size()) {
    const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos));
    b.process(x.data() + pos, n);
    pos += static_cast<std::size_t>(n);
  }
  return x;
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
  return m;
}

}  // namespace

TEST_CASE("NamBlock Linear identity applies only the gains", "[nam]") {
  NamBlockConfig cfg;
  cfg.inputGainDb = 6.0;
  cfg.outputGainDb = -3.0;
  auto b = make("linear_identity.nam", 48000.0, 512, cfg);
  REQUIRE(b->latencySamples() == 0);
  REQUIRE(b->expectedSampleRate() == 48000.0);
  REQUIRE_FALSE(b->loudnessDb().has_value());
  REQUIRE(b->metadata().name == "Linear identity");
  REQUIRE(b->metadata().gearType == "amp");
  REQUIRE(b->metadata().modeledBy == "sawblade-tests");

  const auto x = noise(3000, 4, 0.3f);
  const auto y = render(*b, x, 300);
  const double g = std::pow(10.0, 3.0 / 20.0);
  for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(std::fabs(y[i] - x[i] * g) <= 1e-6);
}

TEST_CASE("NamBlock actually applies the model (Linear 0.5, 0.25)", "[nam]") {
  auto b = make("linear_05_025.nam");
  const auto x = noise(2000, 5, 0.5f);
  const auto y = render(*b, x, 128);
  REQUIRE(std::fabs(y[0] - 0.5 * x[0]) <= 1e-6);
  for (std::size_t i = 1; i < x.size(); ++i) REQUIRE(std::fabs(y[i] - (0.5 * x[i] + 0.25 * x[i - 1])) <= 1e-6);
}

TEST_CASE("NamBlock loudness normalization and bypass", "[nam]") {
  const auto x = noise(1000, 6, 0.3f);
  NamBlockConfig cfg;
  cfg.normalizeLoudness = true;

  auto loud = make("linear_identity_loud24.nam", 48000.0, 512, cfg);
  REQUIRE(loud->loudnessDb().has_value());
  REQUIRE(*loud->loudnessDb() == Catch::Approx(-24.0));
  const double g = std::pow(10.0, 6.0 / 20.0);  // (-18 - -24) = +6 dB
  const auto y = render(*loud, x, 100);
  for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(std::fabs(y[i] - x[i] * g) <= 1e-6);

  // Without the flag the loudness metadata is ignored.
  auto plain = make("linear_identity_loud24.nam");
  const auto y2 = render(*plain, x, 100);
  for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(std::fabs(y2[i] - x[i]) <= 1e-6);

  // No loudness metadata: flag has no effect.
  auto noLoud = make("linear_identity.nam", 48000.0, 512, cfg);
  const auto y3 = render(*noLoud, x, 100);
  for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(std::fabs(y3[i] - x[i]) <= 1e-6);

  // Bypass is an exact passthrough, regardless of gains.
  NamBlockConfig bp;
  bp.bypass = true;
  bp.inputGainDb = 12.0;
  auto byp = make("linear_05_025.nam", 48000.0, 512, bp);
  REQUIRE(render(*byp, x, 77) == x);
}

TEST_CASE("NamBlock example models: finite, deterministic, block-size invariant", "[nam]") {
  const auto x = noise(96000, 7, 0.3f);
  for (const std::string file : {"wavenet.nam", "lstm.nam"}) {
    DYNAMIC_SECTION(file) {
      std::vector<float> ref;
      for (const int block : {64, 128, 1000}) {
        auto b = make(file, 48000.0, block);
        const auto y = render(*b, x, block);
        for (const float v : y) REQUIRE(std::isfinite(v));
        REQUIRE(rms(y.data(), y.size()) > 1e-4);  // the model does something
        if (ref.empty()) {
          ref = y;
          auto b2 = make(file, 48000.0, block);
          REQUIRE(render(*b2, x, block) == y);  // bit-identical on a second run
        } else {
          REQUIRE(maxAbsDiff(ref, y) <= 1e-5);
        }
      }
    }
  }
}

TEST_CASE("NamBlock sample-rate mismatch throws; load errors carry the path", "[nam]") {
  auto b = NamBlock::load(fixture("wavenet.nam"), {});
  REQUIRE(b->expectedSampleRate() == 48000.0);
  REQUIRE_THROWS_AS(b->prepare({44100.0, 256}), std::runtime_error);
  REQUIRE_NOTHROW(b->prepare({48000.0, 256}));

  try {
    NamBlock::load(fixture("nope.nam"), {});
    FAIL("expected throw");
  } catch (const std::runtime_error& e) {
    REQUIRE(std::string(e.what()).find("nope.nam") != std::string::npos);
  }
}

TEST_CASE("NamBlock process performs no allocations", "[nam][alloc]") {
  for (const std::string file : {"wavenet.nam", "lstm.nam", "linear_identity.nam", "linear_05_025.nam"}) {
    DYNAMIC_SECTION(file) {
      auto b = make(file, 48000.0, 512);
      std::vector<float> buf = noise(512, 8, 0.3f);
      const int sizes[] = {512, 1, 7, 64, 333, 128, 2, 500, 17, 256, 512, 1, 100};
      long allocs = -1;
      {
        AllocGuard g;
        for (const int n : sizes) b->process(buf.data(), n);
        allocs = g.count();
      }
      REQUIRE(allocs == 0);
    }
  }
}
