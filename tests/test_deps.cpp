// Smoke tests that the pinned third-party deps are wired up (NAM core float build, json, PFFFT).
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <type_traits>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "pffft.h"

TEST_CASE("NAM core is built with float samples", "[deps]") {
  STATIC_REQUIRE(std::is_same_v<NAM_SAMPLE, float>);
  REQUIRE(nlohmann::json::parse("{\"a\":1}")["a"] == 1);
}

TEST_CASE("PFFFT links and runs", "[deps]") {
  PFFFT_Setup* s = pffft_new_setup(256, PFFFT_REAL);
  REQUIRE(s != nullptr);
  pffft_destroy_setup(s);
}

TEST_CASE("NAM core library links: loading a missing file throws", "[deps]") {
  REQUIRE_THROWS(nam::get_dsp(std::filesystem::path("/nonexistent/model.nam")));
}
