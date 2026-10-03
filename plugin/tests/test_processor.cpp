// Headless tests of SawbladeProcessor: no window, no audio device, no host. The processor is driven
// the way a host would drive it (prepareToPlay / processBlock / get+setStateInformation).
#include <algorithm>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "PluginProcessor.h"
#include "alloc_guard.h"
#include "latency_stub.h"
#include "lock_guard.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::plugin;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = SAWBLADE_FIXTURES_DIR;
const fs::path kPresetDir = kFixtures / "presets";

// One JUCE message-manager/initialiser for the whole run (the processor and its parameter tree
// expect one to exist; it opens no window and no display connection). It is created and destroyed
// by a Catch listener: a function-local/static instance would be torn down at exit, after the
// objects JUCE's shutdown code touches are already gone.
class JuceLifetime : public Catch::EventListenerBase {
 public:
  using Catch::EventListenerBase::EventListenerBase;
  void testRunStarting(const Catch::TestRunInfo&) override { init_ = std::make_unique<juce::ScopedJuceInitialiser_GUI>(); }
  void testRunEnded(const Catch::TestRunStats&) override { init_.reset(); }

 private:
  std::unique_ptr<juce::ScopedJuceInitialiser_GUI> init_;
};
CATCH_REGISTER_LISTENER(JuceLifetime)

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
fs::path writeIdentityPreset(const fs::path& dir, const std::string& name, int stubLatency) {
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
struct Host {
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

std::vector<int> randomSizes(unsigned seed, int count = 64, int maxSize = 4096) {
  std::mt19937 g(seed);
  std::vector<int> s;
  for (int i = 0; i < count; ++i) s.push_back(1 + static_cast<int>(g() % static_cast<unsigned>(maxSize)));
  return s;
}

}  // namespace

TEST_CASE("test harness: LockGuard sees mutex acquisitions", "[processor][harness]") {
  if (!LockGuard::enabled()) SKIP("lock counting is only wired up on Linux");
  std::mutex m;
  LockGuard lg;
  m.lock();
  m.unlock();
  CHECK(lg.count() >= 1);
}

TEST_CASE("Processor: starts as a zero-latency pass-through (Init preset)", "[processor]") {
  Host h(48000.0, 256);
  CHECK(h.p.getLatencySamples() == 0);
  CHECK(h.p.status().presetName == "Init");
  const auto x = noise(1000, 3, 0.5f);
  std::vector<float> y;
  h.run(x, y, {256, 100, 1});
  CHECK(y == x);
}

TEST_CASE("Processor: prepare/process at 44.1/48/96 kHz, block sizes 1..4096, no allocations or locks", "[processor][rt]") {
  for (const double rate : {44100.0, 48000.0, 96000.0}) {
    CAPTURE(rate);
    Host h(rate, 512);
    h.load(kPresetDir / "golden_shared.json");  // gate, WaveNet + LSTM + linear, shared cab, post EQ, bus comp
    const auto st = h.p.status();
    CHECK(st.modelRate == 48000.0);
    CHECK(st.hostRate == rate);
    CHECK(st.resampling == (rate != 48000.0));
    CHECK(h.p.getLatencySamples() == st.latencySamples);

    const auto x = noise(static_cast<std::size_t>(rate * 0.8), 5, 0.3f);
    std::vector<float> y;
    h.run(x, y, randomSizes(static_cast<unsigned>(rate)));
    CHECK(h.allocs == 0);
    if (LockGuard::enabled()) CHECK(h.locks == 0);
    CHECK_FALSE(h.nonFinite);
    CHECK(rms(y.data() + y.size() / 2, y.size() / 2) > 1e-3);  // the chain actually produces sound

    // Block size 1, the worst case for per-block overhead.
    std::vector<float> x1(std::min<std::size_t>(x.size(), 3000)), y1;
    std::copy(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(x1.size()), x1.begin());
    h.run(x1, y1, {1});
    CHECK(h.allocs == 0);
    if (LockGuard::enabled()) CHECK(h.locks == 0);
  }
}

TEST_CASE("Processor: a block larger than the prepared size is handled", "[processor]") {
  Host h(44100.0, 64);
  TempDir t;
  h.load(writeIdentityPreset(t.dir, "ident", 0));
  const auto x = noise(8000, 2, 0.4f);
  std::vector<float> y;
  h.run(x, y, {8000});  // 8000 samples into a processor prepared for 64
  CHECK(h.allocs == 0);
  CHECK_FALSE(h.nonFinite);
  CHECK(rms(y.data() + 4000, 4000) > 0.1);
}

TEST_CASE("Processor: state round trip is the preset JSON", "[processor][state]") {
  Host a(48000.0, 512);
  a.load(kPresetDir / "golden_shared.json");
  a.setParam(kInputGain, 3.25);
  a.setParam(kBlend, 0.3);
  a.setParam(kLevelA, -4.5);
  a.setParam(kPostEqFirst, 5.0);  // golden_shared: slot 0 is the 1.5 kHz peak
  juce::MemoryBlock s1;
  a.p.getStateInformation(s1);
  const std::string text(static_cast<const char*>(s1.getData()), s1.getSize());

  // It is a valid preset: parses, carries the parameter values, absolute capture paths.
  const json j = json::parse(text);
  CHECK(j["schema"] == "sawblade.preset");
  CHECK(j["blend"].get<double>() == Catch::Approx(0.3).margin(1e-4));
  CHECK(j["input"]["gainDb"].get<double>() == Catch::Approx(3.25).margin(1e-4));
  CHECK(j["paths"]["a"]["levelDb"].get<double>() == Catch::Approx(-4.5).margin(1e-4));
  CHECK(j["postEq"][1]["gainDb"].get<double>() == Catch::Approx(5.0).margin(1e-4));
  CHECK(fs::path(j["cab"]["ir"]["file"].get<std::string>()).is_absolute());
  CHECK(fs::path(j["paths"]["a"]["blocks"][0]["model"]["file"].get<std::string>()).is_absolute());

  // Restore into a fresh processor (as a host does on session load), possibly before prepareToPlay.
  SawbladeProcessor b;
  b.setStateInformation(s1.getData(), static_cast<int>(s1.getSize()));
  b.setRateAndBufferSizeDetails(48000.0, 512);
  b.prepareToPlay(48000.0, 512);
  REQUIRE(b.waitForLoader());
  CHECK(b.status().error.empty());
  CHECK(b.status().presetName == a.p.status().presetName);
  CHECK(b.getLatencySamples() == a.p.getLatencySamples());
  juce::MemoryBlock s2;
  b.getStateInformation(s2);
  CHECK(std::string(static_cast<const char*>(s2.getData()), s2.getSize()) == text);

  // And the restored preset equals the parsed state.
  CHECK(b.currentPreset() == parsePreset(j, std::filesystem::current_path()));

  // Garbage state is ignored, reported, and keeps the current preset.
  const char junk[] = "not json";
  b.setStateInformation(junk, sizeof junk);
  CHECK_FALSE(b.status().error.empty());
  CHECK(b.status().presetName == a.p.status().presetName);
}

TEST_CASE("Processor: loading a preset sets the parameters to its values", "[processor][params]") {
  Host h(48000.0, 512);
  h.load(kPresetDir / "golden_shared.json");  // blend 0.55, level A -3 dB, output -3 dB, gate -55 dB
  CHECK(h.param(kBlend) == Catch::Approx(0.55).margin(1e-6));
  CHECK(h.param(kLevelA) == Catch::Approx(-3.0).margin(1e-6));
  CHECK(h.param(kOutputGain) == Catch::Approx(-3.0).margin(1e-6));
  CHECK(h.param(kGateThreshold) == Catch::Approx(-55.0).margin(1e-6));
  CHECK(h.param(kPostEqFirst) == Catch::Approx(2.0).margin(1e-6));  // the 1.5 kHz peak
  const auto slots = h.p.postEqSlots();
  CHECK(slots[0] == 1);
  CHECK(slots[1] == -1);
}

TEST_CASE("Processor: reported latency equals the measured impulse delay", "[processor][latency]") {
  TempDir t;
  for (const int stub : {0, 137}) {
    const fs::path preset = writeIdentityPreset(t.dir, "ident" + std::to_string(stub), stub);
    for (const double rate : {44100.0, 48000.0, 88200.0, 96000.0}) {
      CAPTURE(stub, rate);
      Host h(rate, 512);
      h.load(preset);
      const int reported = h.p.getLatencySamples();
      REQUIRE(reported == h.p.status().latencySamples);
      if (rate == 48000.0) CHECK(reported == stub);

      const int k0 = 700;
      std::vector<float> x(6000, 0.0f), y;
      x[static_cast<std::size_t>(k0)] = 1.0f;
      h.run(x, y, randomSizes(9, 20, 1500));
      const auto peak = std::max_element(y.begin(), y.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) - y.begin();
      CHECK(peak - k0 == reported);
      if (rate == 44100.0 && stub == 0) WARN("44.1 kHz host, 48 kHz models: " << reported << " samples latency (measured delay " << peak - k0 << ")");
    }
  }
}

TEST_CASE("Processor: changing the host rate rebuilds and re-reports the latency", "[processor][latency]") {
  TempDir t;
  Host h(48000.0, 512);
  h.load(writeIdentityPreset(t.dir, "ident", 0));
  CHECK(h.p.getLatencySamples() == 0);
  h.prepare(44100.0, 256);
  CHECK(h.p.getLatencySamples() > 0);
  CHECK(h.p.status().resampling);
  const int l441 = h.p.getLatencySamples();
  const std::uint64_t builds = h.p.engineBuilds();
  h.prepare(44100.0, 128);  // unchanged rate, smaller block: the engine is kept
  CHECK(h.p.engineBuilds() == builds);
  h.prepare(96000.0, 1024);
  CHECK(h.p.engineBuilds() == builds + 1);
  CHECK(h.p.getLatencySamples() > l441);
  h.prepare(48000.0, 512);
  CHECK(h.p.getLatencySamples() == 0);
  const auto x = noise(2000, 4, 0.4f);
  std::vector<float> y;
  h.run(x, y, {300, 77});
  CHECK(rms(y.data() + 1000, 1000) > 0.1);
}

TEST_CASE("Processor: parameter changes never reload or rebuild anything", "[processor][params]") {
  TempDir t;
  Host h(44100.0, 512);
  h.load(kPresetDir / "golden_shared.json");
  const std::uint64_t builds = h.p.engineBuilds();
  const auto x = noise(40000, 8, 0.3f);
  std::vector<float> y;

  // Sweep every parameter across its whole range while audio runs.
  std::size_t pos = 0;
  int step = 0;
  while (pos + 512 <= x.size()) {
    for (int i = 0; i < kNumParams; ++i) {
      const ParamSpec& s = paramSpec(i);
      const double ph = 0.5 + 0.5 * std::sin(0.01 * step * (i + 1));
      h.setParam(i, s.min + ph * (s.max - s.min));
    }
    std::vector<float> out(512);
    h.process(x.data() + pos, out.data(), 512);
    pos += 512;
    ++step;
  }
  CHECK(h.p.engineBuilds() == builds);
  CHECK(h.allocs == 0);
  if (LockGuard::enabled()) CHECK(h.locks == 0);
  CHECK_FALSE(h.nonFinite);
  CHECK(h.p.status().error.empty());
  CHECK_FALSE(h.p.status().loading);
}

TEST_CASE("Processor: output gain and blend act on the audio, smoothly", "[processor][params]") {
  TempDir t;
  Host h(48000.0, 512);
  h.load(writeIdentityPreset(t.dir, "ident", 0));
  const std::uint64_t builds = h.p.engineBuilds();
  std::vector<float> x(48000, 0.25f), y;
  h.setParam(kOutputGain, 0.0);
  h.run(x, y, {512});
  CHECK(y.back() == Catch::Approx(0.25).epsilon(1e-4));
  h.setParam(kOutputGain, -12.0);
  h.run(x, y, {512});
  CHECK(y.back() == Catch::Approx(0.25 * std::pow(10.0, -12.0 / 20.0)).epsilon(1e-4));
  // No zipper: a 12 dB step spreads over ~20 ms; consecutive samples never jump.
  h.setParam(kOutputGain, 0.0);
  h.run(x, y, {64});
  double maxStep = 0.0;
  for (std::size_t i = 1; i < 4000; ++i) maxStep = std::max(maxStep, static_cast<double>(std::fabs(y[i] - y[i - 1])));
  CHECK(maxStep < 0.001);
  CHECK(h.p.engineBuilds() == builds);
}

TEST_CASE("Processor: swap under load - a thread loads presets while the audio thread runs", "[processor][swap][rt]") {
  TempDir t;
  const fs::path ident = writeIdentityPreset(t.dir, "ident", 0);
  const fs::path identDelay = writeIdentityPreset(t.dir, "identdelay", 200);
  const fs::path shared = kPresetDir / "golden_shared.json";
  const fs::path perPath = kPresetDir / "golden_perpath.json";
  const fs::path presets[] = {ident, shared, identDelay, perPath};

  Host h(44100.0, 256);  // 44.1 host: every engine includes the resamplers and the FIFOs
  h.load(ident);
  const std::uint64_t builds0 = h.p.engineBuilds();

  std::atomic<bool> stop{false};
  std::atomic<long> blocks{0};
  std::atomic<long> audioAllocs{0}, audioLocks{0};
  std::atomic<bool> bad{false};
  std::thread audio([&] {
    const auto x = noise(256, 21, 0.3f);
    std::vector<float> out(256);
    juce::AudioBuffer<float> storage(2, 256);
    juce::MidiBuffer midi;
    while (!stop.load()) {
      float* ch[2] = {storage.getWritePointer(0), storage.getWritePointer(1)};
      std::copy(x.begin(), x.end(), ch[0]);
      juce::AudioBuffer<float> view(ch, 2, 256);
      long a, l;
      {
        AllocGuard ag;
        LockGuard lg;
        h.p.processBlock(view, midi);
        a = ag.count();
        l = lg.count();
      }
      audioAllocs += a;
      audioLocks += l;
      for (int i = 0; i < 256; ++i)
        if (!std::isfinite(ch[0][i]) || std::fabs(ch[0][i]) > 100.0f) bad = true;
      ++blocks;
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  });

  // Loader side: back-to-back loads (some superseded before they finish), parameter moves in between.
  std::string err;
  for (int i = 0; i < 14; ++i) {
    REQUIRE(h.p.loadPresetFile(presets[i % 4], &err));
    if (i % 3 == 2) {
      REQUIRE(h.p.waitForLoader());
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    h.setParam(kBlend, 0.2 + 0.05 * i);
    h.setParam(kOutputGain, -3.0 + 0.2 * i);
  }
  REQUIRE(h.p.waitForLoader());
  // Keep the audio thread running across the last swap for a moment, then stop.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop = true;
  audio.join();

  CHECK(blocks.load() > 100);
  CHECK(audioAllocs.load() == 0);
  if (LockGuard::enabled()) CHECK(audioLocks.load() == 0);
  CHECK_FALSE(bad.load());
  CHECK(h.p.engineBuilds() > builds0);
  CHECK(h.p.status().error.empty());
  CHECK(h.p.status().presetName == "Golden shared (live-compatible)");  // the last preset loaded (i = 13 -> presets[1])

  // The engines the audio thread replaced were destroyed by the loader, not leaked or left pending.
  CHECK(h.p.engineBuilds() <= builds0 + 14);
}

TEST_CASE("Processor: a failed load keeps the running engine and reports the error", "[processor]") {
  TempDir t;
  Host h(48000.0, 512);
  h.load(writeIdentityPreset(t.dir, "ident", 0));
  const std::uint64_t builds = h.p.engineBuilds();

  // Parses, but the model file does not exist: fails while building on the loader thread.
  json j = json::parse(std::ifstream(t.dir / "ident.json"));
  j["name"] = "broken";
  j["paths"]["a"]["blocks"][0]["model"]["file"] = (t.dir / "missing.nam").string();
  std::string err;
  REQUIRE(h.p.loadPresetJson(j.dump(), t.dir, &err));  // parse succeeds
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(h.p.status().error.empty());
  CHECK(h.p.status().error.find("missing.nam") != std::string::npos);
  CHECK(h.p.engineBuilds() == builds);  // nothing was swapped in

  const auto x = noise(2000, 6, 0.4f);
  std::vector<float> y;
  h.run(x, y, {256});
  CHECK(rms(y.data() + 1000, 1000) > 0.1);  // the previous identity engine still runs

  // A preset that does not parse is rejected synchronously.
  CHECK_FALSE(h.p.loadPresetJson("{\"schema\": \"nope\"}", t.dir, &err));
  CHECK_FALSE(err.empty());
  CHECK_FALSE(h.p.loadPresetFile(t.dir / "does_not_exist.json", &err));
}

TEST_CASE("Processor: stereo input is summed to mono, output is dual mono", "[processor][layout]") {
  Host h(48000.0, 256);
  juce::AudioProcessor::BusesLayout layout;
  layout.inputBuses.add(juce::AudioChannelSet::stereo());
  layout.outputBuses.add(juce::AudioChannelSet::stereo());
  REQUIRE(h.p.setBusesLayout(layout));
  h.prepare(48000.0, 256);
  const auto l = noise(512, 1, 0.5f), r = noise(512, 2, 0.5f);
  std::vector<float> o1(512), o2(512);
  h.process(l.data(), o1.data(), 512, r.data(), o2.data());  // Init preset: pure pass-through of the mono sum
  for (std::size_t i = 0; i < 512; ++i) {
    CHECK(o1[i] == Catch::Approx(0.5f * (l[i] + r[i])).margin(1e-7));
    CHECK(o1[i] == o2[i]);
  }
  // Unsupported layouts are refused.
  juce::AudioProcessor::BusesLayout bad;
  bad.inputBuses.add(juce::AudioChannelSet::quadraphonic());
  bad.outputBuses.add(juce::AudioChannelSet::stereo());
  CHECK_FALSE(h.p.checkBusesLayoutSupported(bad));
}

TEST_CASE("Processor: an engine swap cross-fades without a dropout", "[processor][swap][rt]") {
  TempDir t;
  const fs::path a = writeIdentityPreset(t.dir, "identA", 0);
  const fs::path b = writeIdentityPreset(t.dir, "identB", 0);
  for (const double rate : {44100.0, 48000.0}) {
    CAPTURE(rate);
    Host h(rate, 256);
    h.load(a);
    const int win = static_cast<int>(rate * 0.010);  // 10 ms RMS windows
    std::size_t n = 0;
    std::vector<double> windowRms;
    std::vector<float> acc;
    auto run = [&](double seconds) {
      const auto total = static_cast<std::size_t>(rate * seconds);
      std::vector<float> in(256), out(256);
      for (std::size_t done = 0; done < total; done += 256) {
        for (std::size_t i = 0; i < 256; ++i)
          in[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * 1000.0 * static_cast<double>(n + i) / rate));
        h.process(in.data(), out.data(), 256);
        n += 256;
        acc.insert(acc.end(), out.begin(), out.end());
        while (static_cast<int>(acc.size()) >= win) {
          windowRms.push_back(rms(acc.data(), static_cast<std::size_t>(win)));
          acc.erase(acc.begin(), acc.begin() + win);
        }
      }
    };
    run(0.5);
    const std::size_t steadyEnd = windowRms.size();
    const double steady = windowRms.back();
    h.load(b);  // publishes a second engine; the next block starts the fade
    run(0.3);
    REQUIRE(windowRms.size() > steadyEnd + 20);
    double lo = 1e9, hi = 0.0;
    for (std::size_t i = steadyEnd; i < windowRms.size(); ++i) {
      lo = std::min(lo, windowRms[i]);
      hi = std::max(hi, windowRms[i]);
    }
    WARN("rate " << rate << ": swap window RMS " << toDb(lo / steady) << " .. " << toDb(hi / steady) << " dB re steady");
    CHECK(toDb(lo / steady) > -3.0);
    CHECK(toDb(hi / steady) < 3.0);
    CHECK(h.allocs == 0);
    if (LockGuard::enabled()) CHECK(h.locks == 0);
    CHECK_FALSE(h.nonFinite);
  }
}
