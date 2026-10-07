// v0.6 Task B: CPU benchmark for NAM A2 playback (ctest "a2_bench"). Prints a table; fails only if the rig is not real-time.
//
//   per-instance rows: one NamBlock (A1 standard / A2 Full / A2 Lite / A2 container) at 48 kHz, 64-sample blocks
//   rig row:           the plugin's two-path graph (Chain, built from tests/fixtures/a2/rig_two_a2_full.json): gate, two paths each
//                      with a modeled pedal + an A2 Full amp, path EQs, align, blend, shared 4096-tap cab IR, post EQ, bus comp.
//
// Numbers: real-time factor (RTF = processing time / audio time; 0.10 = 10 % of one core) and microseconds per 64-sample block
// (deadline at 48 kHz: 1333 us). Method for shared CI runners: 3 untimed warm-up passes, then kRuns timed passes; report the
// MEDIAN (robust to a noisy neighbour) plus min and max. The weights are untrained fixtures: cost does not depend on them
// (the models have no data-dependent branches), but the input is the fixture signal (noise + sweep), not silence.
//
// Pass/fail (decision 5): only the rig, RTF(median) < kRigMaxRtf, in an optimised build (NDEBUG). Debug builds print only.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "json.hpp"
#include "sawblade/chain.h"
#include "sawblade/nam_block.h"
#include "sawblade/preset.h"
#include "sawblade/wav_io.h"

using namespace sawblade;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 64;
constexpr int kWarmup = 3;
constexpr int kRuns = 9;
// One pass = 2 s of audio (1500 blocks): long enough to amortise timer noise and cover the steady state, short enough
// that 12 passes per row keep the whole benchmark to about 15 s.
constexpr int kPassBlocks = 1500;

// Rig must run in less than half real time (RTF < 0.5) on the CI runners, in Release. Rationale: the plugin shares a core with
// the host, the other tracks and the OS; a shared GitHub runner is 1.5-3x noisier than a desktop; 2x headroom over the
// median means the rig still keeps up when a pass runs 2x slow. The number is deliberately far above what the machine
// measures (see the REPORT's CPU table) so it catches a structural regression (e.g. the A2 fast path lost, a per-block
// allocation, a debug-only build) and not CI weather.
constexpr double kRigMaxRtf = 0.5;

struct Stats {
  double medianRtf = 0, minRtf = 0, maxRtf = 0, medianUsPerBlock = 0, p99BlockUs = 0;
};

std::vector<float> loadInput(const fs::path& dir) {
  const AudioFile f = readWav(dir / "input.wav");  // 48000 samples, noise + sweep
  std::vector<float> x;
  x.reserve(static_cast<std::size_t>(kPassBlocks) * kBlock);
  while (x.size() < static_cast<std::size_t>(kPassBlocks) * kBlock) x.insert(x.end(), f.interleaved.begin(), f.interleaved.end());
  x.resize(static_cast<std::size_t>(kPassBlocks) * kBlock);
  return x;
}

template <class ProcessBlock>
Stats measure(ProcessBlock&& run, const std::vector<float>& x) {
  std::vector<float> buf(x.size());
  std::vector<double> secs;
  std::vector<double> blockUs;
  for (int pass = 0; pass < kWarmup + kRuns; ++pass) {
    const bool timed = pass >= kWarmup;
    buf = x;
    const auto t0 = Clock::now();
    if (timed && pass == kWarmup + kRuns - 1) {  // last pass: per-block timing for the worst-block figure (informational)
      for (int b = 0; b < kPassBlocks; ++b) {
        const auto b0 = Clock::now();
        run(buf.data() + static_cast<std::size_t>(b) * kBlock, kBlock);
        blockUs.push_back(std::chrono::duration<double, std::micro>(Clock::now() - b0).count());
      }
    } else {
      for (int b = 0; b < kPassBlocks; ++b) run(buf.data() + static_cast<std::size_t>(b) * kBlock, kBlock);
    }
    const double s = std::chrono::duration<double>(Clock::now() - t0).count();
    if (timed) secs.push_back(s);
  }
  std::sort(secs.begin(), secs.end());
  std::sort(blockUs.begin(), blockUs.end());
  const double audioSec = static_cast<double>(kPassBlocks) * kBlock / kFs;
  Stats st;
  st.medianRtf = secs[secs.size() / 2] / audioSec;
  st.minRtf = secs.front() / audioSec;
  st.maxRtf = secs.back() / audioSec;
  st.medianUsPerBlock = secs[secs.size() / 2] / kPassBlocks * 1e6;
  st.p99BlockUs = blockUs[blockUs.size() * 99 / 100];
  return st;
}

void row(const char* name, const Stats& s) {
  std::printf("%-34s %8.4f %8.4f %8.4f %12.2f %12.2f\n", name, s.medianRtf, s.minRtf, s.maxRtf, s.medianUsPerBlock, s.p99BlockUs);
}

void writeCabIr(const fs::path& p) {  // 4096-tap decaying noise: a cab-length IR, deterministic
  std::mt19937 g(7);
  std::normal_distribution<float> d(0.0f, 1.0f);
  std::vector<float> ir(4096);
  for (std::size_t i = 0; i < ir.size(); ++i) ir[i] = d(g) * std::exp(-static_cast<float>(i) / 600.0f) * 0.2f;
  ir[0] = 1.0f;
  writeWavFloat32(p, kFs, ir);
}

}  // namespace

int main() {
  const fs::path dir = fs::path(SAWBLADE_FIXTURES_DIR) / "a2";
  const auto x = loadInput(dir);
  std::printf("A2 CPU benchmark: 48 kHz, %d-sample blocks, %d s per pass, %d warm-up + %d timed passes, median.\n", kBlock,
              kPassBlocks * kBlock / static_cast<int>(kFs), kWarmup, kRuns);
  std::printf("RTF = processing time / audio time (one core). Block deadline = %.0f us.\n\n", 1e6 * kBlock / kFs);
  std::printf("%-34s %8s %8s %8s %12s %12s\n", "case", "RTF med", "RTF min", "RTF max", "us/block med", "us/block p99");

  for (const char* f : {"a1_standard.nam", "a2_full.nam", "a2_lite.nam", "a2_container.nam"}) {
    auto b = NamBlock::load(dir / f, NamBlockConfig{});
    b->prepare({kFs, kBlock});
    row(f, measure([&](float* p, int n) { b->process(p, n); }, x));
  }

  // Rig: the plugin's graph from a preset JSON. The cab IR path is patched to a generated file in a temp dir.
  const fs::path tmp = fs::temp_directory_path() / ("sawblade_a2_bench_" + std::to_string(std::random_device{}()));
  fs::create_directories(tmp);
  struct TmpCleanup {  // removes the temp dir on every exit path, exceptions included
    fs::path p;
    ~TmpCleanup() {
      std::error_code ec;
      fs::remove_all(p, ec);
    }
  } tmpCleanup{tmp};
  writeCabIr(tmp / "rig_cab_ir.wav");
  nlohmann::json j;
  {
    std::ifstream in(dir / "rig_two_a2_full.json");
    j = nlohmann::json::parse(in);
  }
  j["cab"]["ir"]["file"] = (tmp / "rig_cab_ir.wav").string();
  for (const char* path : {"a", "b"})
    for (auto& blk : j["paths"][path]["blocks"])
      if (blk["type"] == "nam") blk["model"]["file"] = (dir / blk["model"]["file"].get<std::string>()).string();
  const Preset preset = parsePreset(j, dir);
  Chain chain(preset, loadResources(preset, kFs));
  chain.prepare({kFs, kBlock});
  const Stats rig = measure([&](float* p, int n) { chain.process(p, p, n); }, x);
  row("rig: 2x A2 Full + pedals + cab", rig);

  // Validity: the rig must really produce sound (a silent chain would benchmark the gate, not the amps).
  {
    chain.reset();
    std::vector<float> y(x.begin(), x.begin() + 48000);
    for (std::size_t pos = 0; pos < y.size(); pos += kBlock) chain.process(y.data() + pos, y.data() + pos, kBlock);
    double e = 0.0;
    for (float v : y) e += static_cast<double>(v) * v;
    std::printf("\nRig output RMS %.4f (must be > 0), reported latency %d samples.\n", std::sqrt(e / static_cast<double>(y.size())),
                chain.latencySamples());
    if (!(e > 1e-6)) {
      std::printf("FAIL: rig output is silent, the benchmark is not measuring the amps.\n");
      return 1;
    }
  }

#ifdef NDEBUG
  std::printf("Rig real-time check: RTF median %.4f %s %.2f -> %s\n", rig.medianRtf, rig.medianRtf < kRigMaxRtf ? "<" : ">=", kRigMaxRtf,
              rig.medianRtf < kRigMaxRtf ? "PASS" : "FAIL");
  return rig.medianRtf < kRigMaxRtf ? 0 : 1;
#else
  std::printf("Debug build: numbers are not meaningful, real-time check skipped.\n");
  return 0;
#endif
}
