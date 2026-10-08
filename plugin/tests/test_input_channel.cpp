// v0.8 I4b part 1 in the plugin: the stereo DI chooser wired where the 0.5 * (L + R) sum was (docs/specs/v0_8-I4b-plugin.md section 1).
// Headless (no window): the processor is driven like a host, with a stereo input layout. The Init preset passes the chosen mono signal
// straight through, so the output IS the chosen DI.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "alloc_guard.h"
#include "lock_guard.h"
#include "settings/Settings.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::plugin;
using namespace sawblade::test;
using Catch::Approx;
using nlohmann::json;
namespace fs = std::filesystem;

#include "processor_harness.h"

namespace {

constexpr double kRate = 48000.0;

// A played guitar-like channel: 0.4 s of a 300 Hz tone (peak 0.1) every 0.5 s with 5 ms ramps, -85 dBFS noise underneath.
struct Source {
  double lGain = 1.0, rGain = 1.0;
  std::mt19937 rng{3};
  std::uniform_real_distribution<float> noise{-1.0f, 1.0f};
  long n = 0;
  double playedS = 0.0;
  static double env(double t) {
    constexpr double on = 0.4, ramp = 0.005;
    if (t >= on) return 0.0;
    if (t < ramp) return 0.5 - 0.5 * std::cos(std::numbers::pi * t / ramp);
    if (t > on - ramp) return 0.5 - 0.5 * std::cos(std::numbers::pi * (on - t) / ramp);
    return 1.0;
  }
  void block(std::vector<float>& l, std::vector<float>& r, int len) {
    l.resize(static_cast<std::size_t>(len));
    r.resize(static_cast<std::size_t>(len));
    for (int i = 0; i < len; ++i, ++n) {
      const double t = static_cast<double>(n) / kRate;
      const double e = env(std::fmod(t, 0.5));
      const double tone = 0.1 * e * std::sin(2.0 * std::numbers::pi * 300.0 * t);
      l[static_cast<std::size_t>(i)] = static_cast<float>(tone * lGain) + 5.6e-5f * noise(rng);
      r[static_cast<std::size_t>(i)] = static_cast<float>(tone * rGain) + 5.6e-5f * noise(rng);
      if (e > 0.0) playedS += 1.0 / kRate;
    }
  }
};

// A host with a stereo input layout.
struct StereoHost {
  Host h;
  Source src;
  std::vector<float> l, r, o1, o2;
  float prev = 0.0f, maxStep = 0.0f;
  StereoHost() : h(kRate, 256) {
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(juce::AudioChannelSet::stereo());
    layout.outputBuses.add(juce::AudioChannelSet::stereo());
    REQUIRE(h.p.setBusesLayout(layout));
    h.prepare(kRate, 256);
  }
  void block(int len = 256) {
    src.block(l, r, len);
    o1.resize(l.size());
    o2.resize(l.size());
    h.process(l.data(), o1.data(), len, r.data(), o2.data());
    for (float v : o1) {
      maxStep = std::max(maxStep, std::fabs(v - prev));
      prev = v;
    }
  }
  template <class F>
  void go(double seconds, F&& f) {
    for (long done = 0, total = static_cast<long>(seconds * kRate); done < total; done += 256) {
      block();
      f(src.playedS);
    }
  }
  void go(double seconds) {
    go(seconds, [](double) {});
  }
  std::string notice() const { return h.p.inputChannelNotice(); }
};

// The Init preset is a pass-through up to float rounding in the engine (the existing stereo test compares with 1e-7).
bool same(const std::vector<float>& a, const std::vector<float>& b, float margin = 1e-6f) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::fabs(a[i] - b[i]) > margin) return false;
  return true;
}

double rmsOf(const std::vector<float>& x) {
  double s = 0.0;
  for (float v : x) s += static_cast<double>(v) * v;
  return std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, x.size())));
}

}  // namespace

TEST_CASE("stereo input: a silent R latches L within 3 s of played audio and plays at L's level (not -6 dB)", "[stereo][processor]") {
  StereoHost s;
  s.src.rGain = 0.0;
  CHECK(s.notice() == "Input: L+R mix (auto)");
  double latchedAt = -1.0;
  s.go(6.0, [&](double played) {
    if (latchedAt < 0.0 && s.h.p.stereoInput().decision() == InputChannel::Left) latchedAt = played;
  });
  REQUIRE(latchedAt > 0.0);
  CHECK(latchedAt <= 3.0);
  CHECK(s.notice() == "Input: L only (auto)");
  s.go(1.0);
  s.block(4800);
  CHECK(same(s.o1, s.l));  // L itself, not 0.5 * L
  CHECK(rmsOf(s.o1) == Approx(rmsOf(s.l)).epsilon(1e-4));
  CHECK(s.o1 == s.o2);  // dual-mono output
  CHECK(s.h.allocs == 0);
  CHECK(s.h.locks == 0);
}

TEST_CASE("stereo input: a silent L latches R", "[stereo][processor]") {
  StereoHost s;
  s.src.lGain = 0.0;
  s.go(6.0);
  CHECK(s.notice() == "Input: R only (auto)");
  s.go(1.0);
  s.block(4800);
  CHECK(same(s.o1, s.r));
  CHECK(s.h.allocs == 0);
}

TEST_CASE("stereo input: both channels active stays Mix", "[stereo][processor]") {
  StereoHost s;
  s.src.rGain = 0.2;  // 14 dB down: not one channel
  s.go(20.0);
  CHECK(s.notice() == "Input: L+R mix (auto)");
  s.block(2400);
  for (std::size_t i = 0; i < s.l.size(); ++i) REQUIRE(s.o1[i] == Approx(0.5f * (s.l[i] + s.r[i])).margin(1e-6));
}

TEST_CASE("stereo input: a mid-stream reversal does not switch before 10 s of played frames, then fades over, no step", "[stereo][processor]") {
  StereoHost s;
  s.src.rGain = 0.0;
  s.go(6.0);
  REQUIRE(s.h.p.stereoInput().decision() == InputChannel::Left);
  while (std::fmod(static_cast<double>(s.src.n) / kRate, 0.5) > 0.41) s.block(240);  // swap in a gap
  s.src.lGain = 0.0;
  s.src.rGain = 1.0;
  const double t0 = s.src.playedS;
  double switchedAt = -1.0;
  s.maxStep = 0.0;
  s.go(14.0, [&](double played) {
    if (switchedAt < 0.0 && s.h.p.stereoInput().decision() == InputChannel::Right) switchedAt = played - t0;
  });
  REQUIRE(switchedAt > 0.0);
  CHECK(switchedAt >= 10.0 - 0.01);
  CHECK(switchedAt <= 11.5);
  const float toneStep = static_cast<float>(0.1 * 2.0 * std::numbers::pi * 300.0 / kRate);
  CHECK(s.maxStep <= toneStep + 0.1f / static_cast<float>(0.020 * kRate) + 2e-4f);  // the ramp of a >= 20 ms fade
  s.go(1.0);
  CHECK(s.notice() == "Input: R only (auto)");
  CHECK(s.h.allocs == 0);
  CHECK(s.h.locks == 0);
}

TEST_CASE("stereo input: a mono layout is bit-identical to a plain pass-through", "[stereo][processor]") {
  Host h(kRate, 256);  // mono in
  CHECK(h.p.inputChannelNotice().empty());
  const auto x = noise(4096, 9, 0.5f);
  std::vector<float> y;
  h.run(x, y, {256});
  CHECK(same(y, x));
  CHECK(h.p.inputChannelNotice().empty());
  CHECK(h.allocs == 0);
}

TEST_CASE("stereo input: Settings force L, R or Mix with a fade, and the notice names the decision", "[stereo][processor][settings]") {
  StereoHost s;
  s.src.rGain = 0.3;
  auto& st = settings::Settings::shared();
  CHECK(st.inputChannel() == InputChannelMode::Auto);
  for (const auto& [mode, text] : {std::pair{InputChannelMode::Left, "Input: L only (forced)"}, std::pair{InputChannelMode::Right, "Input: R only (forced)"},
                                   std::pair{InputChannelMode::Mix, "Input: L+R mix (forced)"}}) {
    REQUIRE(st.setInputChannel(mode).ok);
    s.h.p.syncInputChannel();
    s.maxStep = 0.0;
    s.go(0.5);
    CHECK(s.notice() == text);
    s.block(2400);
    for (std::size_t i = 0; i < s.l.size(); ++i) {
      const float want = mode == InputChannelMode::Left ? s.l[i] : mode == InputChannelMode::Right ? s.r[i] : 0.5f * (s.l[i] + s.r[i]);
      REQUIRE(s.o1[i] == Approx(want).margin(1e-6));
    }
    const float toneStep = static_cast<float>(0.1 * 2.0 * std::numbers::pi * 300.0 / kRate);
    CHECK(s.maxStep <= toneStep + 0.1f / static_cast<float>(0.020 * kRate) + 2e-4f);
  }
  REQUIRE(st.setInputChannel(InputChannelMode::Auto).ok);
  s.h.p.syncInputChannel();
  s.go(0.2);
  CHECK(s.notice() == "Input: L+R mix (auto)");
  CHECK(s.h.allocs == 0);
  CHECK(s.h.locks == 0);
}

TEST_CASE("stereo input: a bus-layout change re-evaluates from scratch", "[stereo][processor]") {
  StereoHost s;
  s.src.rGain = 0.0;
  s.go(6.0);
  REQUIRE(s.h.p.stereoInput().decision() == InputChannel::Left);
  juce::AudioProcessor::BusesLayout mono;
  mono.inputBuses.add(juce::AudioChannelSet::mono());
  mono.outputBuses.add(juce::AudioChannelSet::stereo());
  REQUIRE(s.h.p.setBusesLayout(mono));
  juce::AudioProcessor::BusesLayout stereo;
  stereo.inputBuses.add(juce::AudioChannelSet::stereo());
  stereo.outputBuses.add(juce::AudioChannelSet::stereo());
  REQUIRE(s.h.p.setBusesLayout(stereo));
  s.block();
  CHECK(s.h.p.stereoInput().decision() == InputChannel::Mix);
}

TEST_CASE("stereo input: the setting is stored in Settings beside the device record, never in the preset or the state", "[stereo][settings]") {
  SettingsEnv env("{}");
  auto& st = settings::Settings::shared();
  CHECK(st.inputChannel() == InputChannelMode::Auto);
  REQUIRE(st.setInputChannel(InputChannelMode::Right).ok);
  CHECK(st.inputChannel() == InputChannelMode::Right);
  std::ifstream in(env.dir / "settings.json");
  const json doc = json::parse(in);
  CHECK(doc.at("inputChannel") == "right");
  settings::Settings::resetSharedForTests();
  CHECK(settings::Settings::shared().inputChannel() == InputChannelMode::Right);  // survives a reload
  REQUIRE(settings::Settings::shared().setInputChannel(InputChannelMode::Auto).ok);
  std::ifstream in2(env.dir / "settings.json");
  CHECK_FALSE(json::parse(in2).contains("inputChannel"));  // Auto is the absence of the key
  // An unknown value reads as Auto.
  std::ofstream(env.dir / "settings.json") << R"({"version":1,"inputChannel":"both"})";
  settings::Settings::resetSharedForTests();
  CHECK(settings::Settings::shared().inputChannel() == InputChannelMode::Auto);

  SawbladeProcessor p;
  juce::MemoryBlock mb;
  p.getStateInformation(mb);
  CHECK(mb.toString().toStdString().find("inputChannel") == std::string::npos);
  CHECK(presetToStateJson(p.currentPreset()).find("inputChannel") == std::string::npos);
}
