#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/convolver.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;

namespace {

std::vector<float> naiveConv(const std::vector<float>& x, const std::vector<float>& h) {
  std::vector<double> y(x.size(), 0.0);
  for (std::size_t n = 0; n < x.size(); ++n) {
    const std::size_t kmax = std::min(n + 1, h.size());
    double acc = 0.0;
    for (std::size_t k = 0; k < kmax; ++k) acc += static_cast<double>(h[k]) * x[n - k];
    y[n] = acc;
  }
  return {y.begin(), y.end()};
}

std::vector<float> runBlocks(Convolver& c, std::vector<float> x, int block) {
  std::size_t pos = 0;
  while (pos < x.size()) {
    const auto n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos));
    c.process(x.data() + pos, n);
    pos += static_cast<std::size_t>(n);
  }
  return x;
}

}  // namespace

TEST_CASE("Convolver matches direct convolution", "[convolver]") {
  for (const std::size_t len : {1u, 100u, 128u, 129u, 257u, 4800u, 96000u}) {
    const std::vector<float> ir = noise(len, 100 + static_cast<unsigned>(len), 0.5f);
    double l1 = 0.0;
    for (float v : ir) l1 += std::fabs(v);
    const std::size_t nIn = std::min<std::size_t>(len + 1500, 20000);
    const std::vector<float> x = noise(nIn, 7, 0.5f);
    const std::vector<float> ref = naiveConv(x, ir);

    Convolver c;
    c.setIr(ir);
    REQUIRE(c.latencySamples() == 0);
    for (const int block : {1, 32, 128, 300, 512}) {
      DYNAMIC_SECTION("len " << len << " block " << block) {
        c.prepare({48000.0, block});
        const auto y = runBlocks(c, x, block);
        double maxErr = 0.0;
        for (std::size_t i = 0; i < y.size(); ++i) maxErr = std::max(maxErr, std::fabs(static_cast<double>(y[i]) - ref[i]));
        INFO("max error " << maxErr << " bound " << 1e-5 * l1);
        REQUIRE(maxErr <= 1e-5 * l1);
      }
    }
  }
}

TEST_CASE("Convolver has zero latency (impulse)", "[convolver]") {
  for (const std::size_t len : {1u, 50u, 128u, 129u, 1000u}) {
    std::vector<float> ir = noise(len, 3, 0.5f);
    ir[0] = 0.75f;
    Convolver c;
    c.setIr(ir);
    c.prepare({48000.0, 256});
    std::vector<float> x(len + 300, 0.0f);
    x[0] = 1.0f;
    const auto y = runBlocks(c, x, 256);
    REQUIRE(y[0] == 0.75f);
    for (std::size_t i = 1; i < len; ++i) REQUIRE(std::fabs(y[i] - ir[i]) <= 1e-6);
    for (std::size_t i = len; i < y.size(); ++i) REQUIRE(std::fabs(y[i]) <= 1e-6);
  }
}

TEST_CASE("Convolver output is bit-identical for every host block size", "[convolver]") {
  const auto ir = noise(5000, 21, 0.4f);
  const auto x = noise(30000, 22, 0.5f);
  Convolver c;
  c.setIr(ir);
  c.prepare({48000.0, 1024});
  const auto ref = runBlocks(c, x, 128);
  for (const int block : {1, 7, 64, 127, 129, 333, 1000, 1024}) {
    c.reset();
    REQUIRE(runBlocks(c, x, block) == ref);
  }
}

TEST_CASE("Convolver reset clears state; setIr replaces the IR; no IR passes through", "[convolver]") {
  Convolver c;
  std::vector<float> x = noise(500, 1);
  const auto orig = x;
  c.process(x.data(), 500);
  REQUIRE(x == orig);
  REQUIRE_THROWS_AS(c.setIr({}), std::invalid_argument);

  c.setIr(noise(1000, 2, 0.3f));
  c.prepare({48000.0, 256});
  const auto a = runBlocks(c, orig, 256);
  c.reset();
  REQUIRE(runBlocks(c, orig, 256) == a);

  c.setIr(std::vector<float>{2.0f});
  c.prepare({48000.0, 256});
  const auto b = runBlocks(c, orig, 100);
  for (std::size_t i = 0; i < orig.size(); ++i) REQUIRE(b[i] == 2.0f * orig[i]);
}

TEST_CASE("Convolver process performs no allocations", "[convolver][alloc]") {
  for (const std::size_t len : {64u, 300u, 20000u}) {
    Convolver c;
    c.setIr(noise(len, 5, 0.3f));
    c.prepare({48000.0, 512});
    std::vector<float> buf = noise(512, 9, 0.3f);
    const int sizes[] = {512, 1, 7, 64, 333, 128, 2, 500, 17, 256, 129, 1000 / 2, 100};
    long allocs = -1;
    {
      AllocGuard g;
      for (int rep = 0; rep < 4; ++rep)
        for (const int n : sizes) c.process(buf.data(), n);
      // reset() is checked here as an implementation property, not an interface contract.
      c.reset();
      allocs = g.count();
    }
    REQUIRE(allocs == 0);
  }
}

TEST_CASE("Convolver reproduces long IRs through every partition", "[convolver]") {
  // Impulse in -> IR out exercises every partition and the frequency-domain delay line wraparound.
  for (const std::size_t len : {96000u, 128u * 400u + 1u}) {
    const std::vector<float> ir = noise(len, 55, 0.5f);
    double l1 = 0.0;
    for (float v : ir) l1 += std::fabs(v);
    std::vector<float> x(len + 1000, 0.0f);
    x[0] = 1.0f;
    Convolver c;
    c.setIr(ir);
    for (const int block : {1, 512}) {
      DYNAMIC_SECTION("len " << len << " block " << block) {
        c.prepare({48000.0, block});
        const auto y = runBlocks(c, x, block);
        double maxErr = 0.0;
        for (std::size_t i = 0; i < y.size(); ++i) {
          const double expect = i < len ? ir[i] : 0.0;
          maxErr = std::max(maxErr, std::fabs(static_cast<double>(y[i]) - expect));
        }
        INFO("max error " << maxErr << " bound " << 1e-5 * l1);
        REQUIRE(maxErr <= 1e-5 * l1);
      }
    }
  }
}
