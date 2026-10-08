#pragma once

// Shared harness of the processor-level tests: temp dir, identity-preset writer and a Host that drives
// a SawbladeProcessor like a DAW (mono in, stereo out) while counting allocations and locks.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "alloc_guard.h"
#include "latency_stub.h"
#include "lock_guard.h"
#include "test_util.h"

namespace {

using namespace sawblade;
using namespace sawblade::plugin;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

const fs::path kFixtures = SAWBLADE_FIXTURES_DIR;
const fs::path kPresetDir = kFixtures / "presets";

struct TempDir {
  fs::path dir;
  TempDir() {
    static int counter = 0;
    dir = fs::temp_directory_path() / ("sawblade_plugin_tests_" + std::to_string(std::random_device{}()) + "_" + std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

// Writes an identity preset (both paths: linear identity NAM at 48 kHz, optional N-sample delay
// that reports N) with absolute capture paths, so it can live anywhere.
[[maybe_unused]] fs::path writeIdentityPreset(const fs::path& dir, const std::string& name, int stubLatency) {
  registerLatencyStub();
  auto block = [&](const std::string& id) {
    return json{{"id", id}, {"type", "nam"}, {"model", {{"file", (kFixtures / "nam" / "linear_identity.nam").string()}}}};
  };
  json a = json::array({block("a1")}), b = json::array({block("b1")});
  if (stubLatency > 0) {
    a.push_back({{"id", "as"}, {"type", "test.latency"}, {"latency", stubLatency}});
    b.push_back({{"id", "bs"}, {"type", "test.latency"}, {"latency", stubLatency}});
  }
  json j = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", name},
            {"paths", {{"a", {{"blocks", a}}}, {"b", {{"blocks", b}}}}},
            {"align", {{"mode", "off"}}},
            {"blend", 0.5},
            {"cab", {{"mode", "shared"}, {"ir", {{"file", (kFixtures / "ir" / "impulse.wav").string()}}}}},
            {"postEq", json::array({{{"type", "peak"}, {"freq", 1000}, {"gainDb", 0.0}, {"q", 1.0}}})}};
  const fs::path p = dir / (name + ".json");
  std::ofstream(p) << j.dump(2);
  return p;
}

// Drives a processor like a host: mono in (channel 0), stereo out.
// v0.8 I2: the processor reads Settings::shared() (calibration toggle, device record, LEVEL MATCH) when it is prepared and on every
// load. A Host therefore isolates the settings (its own temp file) unless the test already did (SAWBLADE_SETTINGS_FILE is set, e.g. by
// a SettingsEnv or a ScopedVar declared before the Host), so no test ever reads the developer's real settings file.
inline std::unique_ptr<SettingsEnv> isolateSettingsUnlessSet() {
  if (const char* f = std::getenv("SAWBLADE_SETTINGS_FILE"); f != nullptr && *f != '\0') return nullptr;
  return std::make_unique<SettingsEnv>("{}");
}

struct Host {
  std::unique_ptr<SettingsEnv> ownSettings = isolateSettingsUnlessSet();  // first: the processor below reads the settings
  SawbladeProcessor p;
  juce::AudioBuffer<float> storage{2, 8192};
  juce::MidiBuffer midi;
  long allocs = 0, locks = 0;
  bool nonFinite = false;

  Host(double rate, int block) { prepare(rate, block); }
  void prepare(double rate, int block) {
    p.setRateAndBufferSizeDetails(rate, block);
    p.prepareToPlay(rate, block);
  }
  // Processes n <= 8192 samples; `in` -> channel 0 (and 1 if the layout has two inputs).
  void process(const float* in, float* out, int n, const float* in2 = nullptr, float* out2 = nullptr) {
    float* ch[2] = {storage.getWritePointer(0), storage.getWritePointer(1)};
    std::copy(in, in + n, ch[0]);
    if (in2) std::copy(in2, in2 + n, ch[1]);
    juce::AudioBuffer<float> view(ch, 2, n);
    {
      AllocGuard ag;
      LockGuard lg;
      p.processBlock(view, midi);
      allocs += ag.count();
      locks += lg.count();
    }
    for (int i = 0; i < n; ++i)
      if (!std::isfinite(ch[0][i])) nonFinite = true;
    std::copy(ch[0], ch[0] + n, out);
    if (out2) std::copy(ch[1], ch[1] + n, out2);
  }
  void run(const std::vector<float>& x, std::vector<float>& y, const std::vector<int>& sizes) {
    y.assign(x.size(), 0.0f);
    std::size_t pos = 0, k = 0;
    while (pos < x.size()) {
      const auto n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(sizes[k++ % sizes.size()]), x.size() - pos));
      process(x.data() + pos, y.data() + pos, n);
      pos += static_cast<std::size_t>(n);
    }
  }
  void setParam(int i, double v) {
    auto* prm = p.parameters().getParameter(paramSpec(i).id);
    prm->setValueNotifyingHost(prm->convertTo0to1(static_cast<float>(v)));
  }
  double param(int i) { return static_cast<double>(p.parameters().getRawParameterValue(paramSpec(i).id)->load()); }
  void load(const fs::path& f) {
    std::string err;
    REQUIRE(p.loadPresetFile(f, &err));
    REQUIRE(p.waitForLoader());
    REQUIRE(p.status().error.empty());
  }
};

[[maybe_unused]] std::vector<int> randomSizes(unsigned seed, int count = 64, int maxSize = 4096) {
  std::mt19937 g(seed);
  std::vector<int> s;
  for (int i = 0; i < count; ++i) s.push_back(1 + static_cast<int>(g() % static_cast<unsigned>(maxSize)));
  return s;
}


}  // namespace
