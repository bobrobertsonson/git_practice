// Deterministic generator for the T4 audio fixtures:
//   <out>/di_riff.wav   4 s @ 48 kHz, 24-bit mono: Karplus-Strong power-chord plucks (open and
//                       palm-muted) separated by gaps of -70 dBFS noise (exercises the gate)
//   <out>/ir/ir_a.wav   48 kHz,   ~200 ms decaying filtered noise (float32)
//   <out>/ir/ir_b.wav   44.1 kHz, ~220 ms, darker spectrum, 1 ms onset delay (float32);
//                       exercises resampling and per-path alignment
//
// Usage: make_fixtures <fixtures_dir>
//
// Everything derives from a fixed-seed xorshift64* generator and plain arithmetic, so the output
// is reproducible; across libm implementations the float32 IR samples can differ in the last bit,
// which is far below the golden tolerance (1e-4).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <vector>

#include "dr_wav.h"
#include "sawblade/wav_io.h"

namespace {

struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull | 1ull) {}
  double uniform() {  // [-1, 1)
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    const std::uint64_t r = s * 0x2545F4914F6CDD1Dull;
    return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0;
  }
};

constexpr double kFs = 48000.0;

// One Karplus-Strong string added into `out` starting at sample `start`.
// `damping` < 1 shortens the sustain (palm mute); `tau` is an extra exponential envelope (s).
void pluck(std::vector<double>& out, std::size_t start, double freq, double seconds, double damping, double tau,
           double amp, Rng& rng) {
  const auto period = static_cast<std::size_t>(std::lround(kFs / freq));
  std::vector<double> line(period);
  for (auto& v : line) v = rng.uniform();
  const auto len = static_cast<std::size_t>(seconds * kFs);
  const auto fade = static_cast<std::size_t>(0.02 * kFs);
  for (std::size_t n = 0; n < len && start + n < out.size(); ++n) {
    const std::size_t i = n % period, j = (n + 1) % period;
    const double y = line[i];
    line[i] = damping * 0.5 * (line[i] + line[j]);
    double env = std::exp(-static_cast<double>(n) / (tau * kFs));
    if (len - n < fade) env *= static_cast<double>(len - n) / static_cast<double>(fade);  // no click at the end
    out[start + n] += amp * y * env;
  }
}

// Power-chord-ish: root, fifth, octave.
void chord(std::vector<double>& out, double t, double root, double seconds, bool muted, Rng& rng) {
  const auto start = static_cast<std::size_t>(t * kFs);
  const double damping = muted ? 0.97 : 0.998;
  const double tau = muted ? 0.05 : 0.45;
  const double ratios[3] = {1.0, 1.4983, 2.0};
  const double amps[3] = {1.0, 0.8, 0.7};
  for (int k = 0; k < 3; ++k) pluck(out, start, root * ratios[k], seconds, damping, tau, amps[k], rng);
}

void writePcm24(const std::filesystem::path& path, const std::vector<double>& x) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(x.size() * 3);
  for (double v : x) {
    const long q = std::clamp(std::lround(v * 8388607.0), -8388608L, 8388607L);
    const auto u = static_cast<std::uint32_t>(q) & 0xFFFFFFu;
    bytes.push_back(static_cast<std::uint8_t>(u & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((u >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((u >> 16) & 0xFF));
  }
  drwav_data_format fmt{drwav_container_riff, DR_WAVE_FORMAT_PCM, 1, static_cast<drwav_uint32>(kFs), 24};
  drwav w;
  if (!drwav_init_file_write(&w, path.string().c_str(), &fmt, nullptr)) {
    std::fprintf(stderr, "cannot write %s\n", path.string().c_str());
    std::exit(1);
  }
  drwav_write_pcm_frames(&w, x.size(), bytes.data());
  drwav_uninit(&w);
}

void makeDi(const std::filesystem::path& path) {
  const std::size_t n = static_cast<std::size_t>(4.0 * kFs);
  Rng rng(1);
  std::vector<double> x(n, 0.0);
  const double E = 82.41, A = 110.0;
  chord(x, 0.10, E, 1.00, false, rng);                       // open E5, rings out
  for (int k = 0; k < 4; ++k) chord(x, 1.50 + 0.20 * k, E, 0.15, true, rng);  // palm-muted chugs
  chord(x, 2.30, A, 0.15, true, rng);
  chord(x, 2.50, A, 0.15, true, rng);
  chord(x, 2.90, A, 1.10, false, rng);                       // open A5 to the end
  double peak = 0.0;
  for (double v : x) peak = std::max(peak, std::fabs(v));
  const double g = std::pow(10.0, -6.0 / 20.0) / peak;       // peak -6 dBFS
  for (double& v : x) v *= g;
  // -70 dBFS RMS noise floor everywhere (uniform noise has RMS amp/sqrt(3)).
  const double noiseAmp = std::pow(10.0, -70.0 / 20.0) * std::sqrt(3.0);
  Rng nz(2);
  for (double& v : x) v += noiseAmp * nz.uniform();
  writePcm24(path, x);
}

// Decaying noise through a 2-pole low-pass (two cascaded one-poles at fc) and a DC-blocking
// high-pass, onset delayed by `onset` samples.
std::vector<float> makeIr(double fs, double seconds, double tau, double fc, std::size_t onset, std::uint64_t seed) {
  const auto n = static_cast<std::size_t>(seconds * fs);
  Rng rng(seed);
  const double a = std::exp(-2.0 * std::numbers::pi * fc / fs);
  std::vector<double> y(n, 0.0);
  double l1 = 0.0, l2 = 0.0, hpIn = 0.0, hpOut = 0.0;
  for (std::size_t i = 0; i + onset < n; ++i) {
    const double t = static_cast<double>(i) / fs;
    const double in = rng.uniform() * std::exp(-t / tau) * (i == 0 ? 2.0 : 1.0);
    l1 = (1.0 - a) * in + a * l1;
    l2 = (1.0 - a) * l1 + a * l2;
    hpOut = 0.995 * (hpOut + l2 - hpIn);
    hpIn = l2;
    y[i + onset] = hpOut;
  }
  double peak = 0.0;
  for (double v : y) peak = std::max(peak, std::fabs(v));
  std::vector<float> out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(0.9 * y[i] / peak);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: make_fixtures <fixtures_dir>\n");
    return 2;
  }
  const std::filesystem::path dir = argv[1];
  std::filesystem::create_directories(dir / "ir");
  makeDi(dir / "di_riff.wav");
  sawblade::writeWavFloat32(dir / "ir" / "ir_a.wav", 48000.0, makeIr(48000.0, 0.200, 0.040, 5000.0, 0, 11));
  sawblade::writeWavFloat32(dir / "ir" / "ir_b.wav", 44100.0, makeIr(44100.0, 0.220, 0.055, 2500.0, 44, 12));
  std::printf("wrote %s\n", dir.string().c_str());
  return 0;
}
