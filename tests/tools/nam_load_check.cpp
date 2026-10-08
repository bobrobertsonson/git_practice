// nam_load_check: standalone "does a stock NAM core accept this file" check (v0.6 decision 18).
// Links ONLY NeuralAmpModelerCore (no Sawblade code or headers): every file goes through nam::get_dsp(path), the same
// entry point any NAM host uses, then ~1 s of a deterministic test signal is processed in 64-sample blocks.
// Usage: nam_load_check file.nam [file.nam ...]   (exit 0 only if every file loads and plays sane audio)
#include <NAM/dsp.h>
#include <NAM/get_dsp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 64;
constexpr int kSamples = 48000;

// Deterministic: seeded LCG noise (sigma ~0.1) for the first 0.4 s, then a sine sweep, so every path sees both.
std::vector<float> testSignal() {
  std::vector<float> x(kSamples);
  std::uint32_t s = 20261008u;
  const int noiseEnd = kSamples * 2 / 5;
  double phase = 0.0;
  for (int i = 0; i < kSamples; ++i) {
    if (i < noiseEnd) {
      double acc = 0.0;
      for (int k = 0; k < 12; ++k) {  // sum of 12 uniforms ~ gaussian
        s = s * 1664525u + 1013904223u;
        acc += static_cast<double>(s >> 8) / 16777216.0;
      }
      x[static_cast<std::size_t>(i)] = static_cast<float>(0.1 * (acc - 6.0));
    } else {
      const double t = static_cast<double>(i - noiseEnd) / static_cast<double>(kSamples - noiseEnd);
      const double f = 50.0 * std::pow(320.0, t);  // 50 Hz .. 16 kHz
      phase += 2.0 * 3.14159265358979323846 * f / kFs;
      x[static_cast<std::size_t>(i)] = static_cast<float>(0.3 * std::sin(phase));
    }
  }
  return x;
}

struct Info {
  std::string architecture = "?";
  std::string version = "?";
  std::string sampleRate = "?";
};

Info readInfo(const std::filesystem::path& p) {
  Info info;
  try {
    std::ifstream in(p, std::ios::binary);
    const auto j = nlohmann::json::parse(in);
    if (j.contains("architecture") && j["architecture"].is_string()) info.architecture = j["architecture"];
    if (j.contains("version") && j["version"].is_string()) info.version = j["version"];
    if (j.contains("sample_rate") && j["sample_rate"].is_number()) info.sampleRate = std::to_string(j["sample_rate"].get<double>());
  } catch (...) {
  }
  return info;
}

bool check(const std::string& path, const std::vector<float>& x) {
  const Info info = readInfo(path);
  std::printf("%s: architecture=%s version=%s sample_rate=%s\n", path.c_str(), info.architecture.c_str(),
              info.version.c_str(), info.sampleRate.c_str());
  try {
    auto dsp = nam::get_dsp(std::filesystem::path(path));
    if (!dsp) {
      std::printf("  FAIL: get_dsp returned null\n");
      return false;
    }
    dsp->Reset(kFs, kBlock);
    dsp->prewarm();
    std::vector<float> out(x.size(), 0.0f);
    std::vector<float> in(x);
    for (std::size_t pos = 0; pos < in.size(); pos += kBlock) {
      const int n = static_cast<int>(std::min<std::size_t>(kBlock, in.size() - pos));
      float* inp = in.data() + pos;
      float* outp = out.data() + pos;
      dsp->process(&inp, &outp, n);
    }
    bool finite = true;
    float peak = 0.0f;
    for (const float v : out) {
      if (!std::isfinite(v)) finite = false;
      else peak = std::max(peak, std::fabs(v));
    }
    if (!finite) {
      std::printf("  FAIL: output contains NaN/Inf\n");
      return false;
    }
    if (!(peak > 0.0f)) {
      std::printf("  FAIL: output is all zero\n");
      return false;
    }
    std::printf("  ok: peak %.5f\n", static_cast<double>(peak));
    return true;
  } catch (const std::exception& e) {
    std::printf("  FAIL: %s\n", e.what());
    return false;
  } catch (...) {
    std::printf("  FAIL: unknown exception\n");
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: nam_load_check file.nam [file.nam ...]\n");
    return 2;
  }
  const auto x = testSignal();
  bool ok = true;
  for (int i = 1; i < argc; ++i) ok = check(argv[i], x) && ok;
  std::printf(ok ? "all %d file(s) passed\n" : "FAILED (see above)\n", argc - 1);
  return ok ? 0 : 1;
}
