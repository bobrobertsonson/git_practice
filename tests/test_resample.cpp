// T5: shared offline resampler and tonerender's sample-rate handling.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "latency_stub.h"
#include "sawblade/render.h"
#include "sawblade/resample.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = SAWBLADE_FIXTURES_DIR;
const fs::path kPresetDir = kFixtures / "presets";

struct TempDir {
  fs::path dir;
  TempDir() {
    static int counter = 0;
    dir = fs::temp_directory_path() / ("sawblade_resample_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~TempDir() { std::error_code ec; fs::remove_all(dir, ec); }
  fs::path operator/(const std::string& f) const { return dir / f; }
};

std::string q(const fs::path& p) { return "'" + p.string() + "'"; }
int runCli(const std::string& args, const fs::path& errFile) {
  const std::string cmd = q(SAWBLADE_TONERENDER_EXE) + " " + args + " >/dev/null 2>" + q(errFile);
  const int st = std::system(cmd.c_str());
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
std::string slurp(const fs::path& p) {
  std::ifstream f(p);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Peak amplitude of the sinusoid at `freq` in x[i0, i1) by least squares.
double toneAmplitude(const std::vector<float>& x, double freq, double fs, std::size_t i0, std::size_t i1) {
  double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
  for (std::size_t i = i0; i < i1; ++i) {
    const double ph = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / fs;
    const double s = std::sin(ph), c = std::cos(ph);
    ss += s * s; cc += c * c; sc += s * c;
    xs += x[i] * s; xc += x[i] * c;
  }
  const double det = ss * cc - sc * sc;
  const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
  return std::hypot(a, b);
}

// Phase (radians) of the sinusoid at `freq` in x[i0, i1): x ~ A sin(wt + phase).
double tonePhase(const std::vector<float>& x, double freq, double fs, std::size_t i0, std::size_t i1) {
  double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
  for (std::size_t i = i0; i < i1; ++i) {
    const double ph = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / fs;
    const double s = std::sin(ph), c = std::cos(ph);
    ss += s * s; cc += c * c; sc += s * c;
    xs += x[i] * s; xc += x[i] * c;
  }
  const double det = ss * cc - sc * sc;
  const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
  return std::atan2(b, a);
}

void fft(std::vector<std::complex<double>>& a) {
  const std::size_t n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * std::numbers::pi / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0);
      for (std::size_t k = 0; k < len / 2; ++k) {
        const auto u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// Highest component (dBFS, peak amplitude) of the central 65536 samples away from `toneHz`
// (+-16 bins), 4-term Blackman-Harris window.
double maxSpurDbfs(const std::vector<float>& x, double fs, double toneHz) {
  constexpr std::size_t N = 65536;
  REQUIRE(x.size() >= N);
  const std::size_t off = (x.size() - N) / 2;
  std::vector<std::complex<double>> a(N);
  double wsum = 0.0;
  for (std::size_t i = 0; i < N; ++i) {
    const double t = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(N);
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    a[i] = x[off + i] * w;
    wsum += w;
  }
  fft(a);
  const double toneBin = toneHz * N / fs;
  double worst = 0.0;
  for (std::size_t k = 1; k < N / 2; ++k) {
    if (std::fabs(static_cast<double>(k) - toneBin) <= 16.0) continue;
    worst = std::max(worst, 2.0 * std::abs(a[k]) / wsum);
  }
  return toDb(worst);
}

// Sum of 40 fixed-seed sinusoids below 15 kHz: a band-limited, noise-like test signal.
std::vector<float> multiSine(std::size_t n, double fs) {
  std::mt19937 g(5);
  std::uniform_real_distribution<double> f(100.0, 15000.0), ph(0.0, 6.2831853);
  std::vector<double> fr(40), p(40);
  for (int i = 0; i < 40; ++i) { fr[i] = f(g); p[i] = ph(g); }
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    double v = 0.0;
    for (int k = 0; k < 40; ++k) v += std::sin(2.0 * std::numbers::pi * fr[k] * static_cast<double>(i) / fs + p[k]);
    x[i] = static_cast<float>(0.02 * v);
  }
  return x;
}

json identityPreset() {
  return {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "identity"},
          {"paths", {{"a", {{"blocks", json::array({{{"id", "a1"}, {"type", "nam"}, {"model", {{"file", "../nam/linear_identity.nam"}}}}})}}},
                     {"b", {{"blocks", json::array({{{"id", "b1"}, {"type", "nam"}, {"model", {{"file", "../nam/linear_identity.nam"}}}}})}}}}},
          {"align", {{"mode", "off"}}},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}}};
}

Preset identity() { return parsePreset(identityPreset(), kPresetDir); }

AudioFile mono(const std::vector<float>& x, double fs) { return AudioFile{fs, 1, x}; }

}  // namespace

namespace {
// linear_identity.nam retrained "at" 44.1 kHz.
fs::path identity44(const TempDir& t) {
  std::string s = slurp(kFixtures / "nam" / "linear_identity.nam");
  const auto pos = s.find("\"sample_rate\":48000");
  REQUIRE(pos != std::string::npos);
  s.replace(pos, 19, "\"sample_rate\":44100");
  const fs::path f = t / "identity44.nam";
  std::ofstream(f) << s;
  return f;
}
json absolute(json j) {
  for (const char* path : {"a", "b"})
    for (auto& b : j["paths"][path]["blocks"])
      if (b.contains("model")) b["model"]["file"] = (kFixtures / "nam" / fs::path(b["model"]["file"].get<std::string>()).filename()).string();
  j["cab"]["ir"]["file"] = (kFixtures / "ir" / "impulse.wav").string();
  return j;
}
}  // namespace

TEST_CASE("Resample: equal rates are bit-identical, invalid rates throw", "[resample]") {
  const auto x = noise(1000, 1);
  REQUIRE(resample(x, 48000.0, 48000.0) == x);
  REQUIRE_THROWS_AS(resample(x, 0.0, 48000.0), std::invalid_argument);
  REQUIRE_THROWS_AS(resample(x, 44100.0, -1.0), std::invalid_argument);
  REQUIRE(resample({}, 44100.0, 48000.0).empty());
}

TEST_CASE("Resample: passband is flat within 0.05 dB to 20 kHz (44.1 <-> 48)", "[resample]") {
  struct Dir { double from, to; };
  double worstDb = 0.0;
  for (const Dir d : {Dir{44100.0, 48000.0}, Dir{48000.0, 44100.0}}) {
    for (const double f : {30.0, 1000.0, 5000.0, 10000.0, 15000.0, 17000.0, 18500.0, 19500.0, 20000.0}) {
      const auto x = sine(f, d.from, 40000, 0.5);
      const auto y = resample(x, d.from, d.to);
      const double amp = toneAmplitude(y, f, d.to, 3000, y.size() - 3000);
      const double db = toDb(amp / 0.5);
      INFO(d.from << " -> " << d.to << " at " << f << " Hz: " << db << " dB");
      REQUIRE(std::fabs(db) < 0.05);
      worstDb = std::max(worstDb, std::fabs(db));
    }
  }
  INFO("worst passband deviation " << worstDb << " dB");
}

TEST_CASE("Resample: stopband rejects at least 90 dB (alias rejection)", "[resample]") {
  // 48 -> 44.1 kHz: a 23 kHz tone would fold to 21.1 kHz.
  for (const double f : {23000.0, 23500.0, 23900.0}) {
    const auto x = sine(f, 48000.0, 48000, 0.5);
    const auto y = resample(x, 48000.0, 44100.0);
    const double rejDb = toDb(rms(y.data() + 3000, y.size() - 6000) / rms(x.data(), x.size()));
    INFO(f << " Hz: " << rejDb << " dB");
    REQUIRE(rejDb <= -90.0);
  }
  // Same through the whole pipeline: a 23 kHz tone at 48 kHz in, a 44.1 kHz render out.
  const auto x = sine(23000.0, 48000.0, 48000, 0.5);
  RenderOptions o;
  o.renderRate = 48000.0;
  const RenderResult r = renderPreset(identity(), mono(x, 48000.0), o);
  REQUIRE(r.samples.size() == x.size());
  const auto y = resample(r.samples, 48000.0, 44100.0);
  REQUIRE(toDb(rms(y.data() + 3000, y.size() - 6000) / rms(x.data(), x.size())) <= -90.0);
}

TEST_CASE("Resample: arbitrary (non-rational) ratio", "[resample]") {
  const double from = 44100.0, to = 47999.37;
  const auto x = sine(1000.0, from, 30000, 0.5);
  const auto y = resample(x, from, to);
  REQUIRE(y.size() == resampledLength(x.size(), from, to));
  REQUIRE(y.size() == static_cast<std::size_t>(std::llround(30000.0 * to / from)));
  REQUIRE(std::fabs(toDb(toneAmplitude(y, 1000.0, to, 3000, y.size() - 3000) / 0.5)) < 0.05);
  const auto z = resample(sine(23000.0, 48000.0, 48000, 0.5), 48000.0, 44099.1);
  REQUIRE(toDb(rms(z.data() + 3000, z.size() - 6000) / rms(sine(23000.0, 48000.0, 48000, 0.5).data(), 48000)) <= -90.0);
}

TEST_CASE("Resample: linear phase, output time-aligned with the input", "[resample]") {
  for (const double f : {1000.0, 15000.0}) {
    const auto x = sine(f, 44100.0, 40000, 0.5);
    const auto y = resample(x, 44100.0, 48000.0);
    const double dPhase = tonePhase(y, f, 48000.0, 3000, y.size() - 3000) - tonePhase(x, f, 44100.0, 3000, x.size() - 3000);
    INFO(f << " Hz phase error " << dPhase << " rad");
    REQUIRE(std::fabs(dPhase) < 1e-3);
  }
}

TEST_CASE("Render: 44.1 -> 48 -> 44.1 through an identity preset", "[resample][render]") {
  const Preset p = identity();
  for (const double f : {1000.0, 15000.0}) {
    INFO("tone " << f << " Hz");
    constexpr std::size_t N = 3 * 44100;
    const auto x = sine(f, 44100.0, N, 0.5);
    const RenderResult r = renderPreset(p, mono(x, 44100.0));
    REQUIRE(r.inputRate == 44100.0);
    REQUIRE(r.renderRate == 48000.0);
    REQUIRE(r.outputRate == 44100.0);
    REQUIRE(r.samples.size() == N);

    // Level.
    const double inDb = toDb(rms(x.data() + 4000, N - 8000)), outDb = toDb(rms(r.samples.data() + 4000, N - 8000));
    INFO("level change " << outDb - inDb << " dB");
    REQUIRE(std::fabs(outDb - inDb) < 0.05);

    // Alignment: phase and the cross-correlation peak at lag 0.
    REQUIRE(std::fabs(tonePhase(r.samples, f, 44100.0, 4000, N - 4000) - tonePhase(x, f, 44100.0, 4000, N - 4000)) < 1e-3);

    // Spectrum: nothing above -90 dBFS away from the tone.
    const double spur = maxSpurDbfs(r.samples, 44100.0, f);
    INFO("worst spur " << spur << " dBFS");
    REQUIRE(spur < -90.0);
  }

  // Cross-correlation of a broadband (band-limited multi-sine) signal peaks at lag 0, with no
  // fractional offset.
  const auto x = multiSine(44100, 44100.0);
  const RenderResult r = renderPreset(p, mono(x, 44100.0));
  std::vector<double> c;
  for (int lag = -3; lag <= 3; ++lag) {
    double s = 0.0;
    for (std::size_t i = 4000; i < x.size() - 4000; ++i)
      s += static_cast<double>(x[i]) * r.samples[static_cast<std::size_t>(static_cast<long long>(i) + lag)];
    c.push_back(s);
  }
  REQUIRE(std::max_element(c.begin(), c.end()) == c.begin() + 3);
  const double frac = 0.5 * (c[2] - c[4]) / (c[2] - 2.0 * c[3] + c[4]);
  INFO("fractional lag " << frac);
  REQUIRE(std::fabs(frac) < 0.01);
}

TEST_CASE("Render: length is preserved for odd lengths; --out-rate render gives round(N*out/in)", "[resample][render]") {
  const Preset p = identity();
  for (const std::size_t n : {1u, 2u, 3u, 7u, 101u, 997u, 4411u, 44103u, 100003u}) {
    INFO("n = " << n);
    const auto x = noise(n, 3, 0.2f);
    const RenderResult r = renderPreset(p, mono(x, 44100.0));
    REQUIRE(r.samples.size() == n);
    RenderOptions o;
    o.outRate = OutRate::Render;
    const RenderResult rr = renderPreset(p, mono(x, 44100.0), o);
    REQUIRE(rr.outputRate == 48000.0);
    REQUIRE(rr.sampleRate == 48000.0);
    REQUIRE(rr.samples.size() == static_cast<std::size_t>(std::llround(static_cast<double>(n) * 48000.0 / 44100.0)));
    REQUIRE(resample(x, 44100.0, 48000.0).size() == rr.samples.size());
  }
}

TEST_CASE("Render: matching rates do no resampling (bit-identical)", "[resample][render]") {
  const Preset p = identity();
  const auto x = noise(5000, 11, 0.3f);
  const RenderResult a = renderPreset(p, mono(x, 48000.0));
  RenderOptions o;
  o.renderRate = 48000.0;
  const RenderResult b = renderPreset(p, mono(x, 48000.0), o);
  REQUIRE(a.samples == b.samples);
  REQUIRE(a.resampleSeconds == 0.0);
  REQUIRE(a.inputRate == 48000.0);
  REQUIRE(a.renderRate == 48000.0);
  REQUIRE(a.outputRate == 48000.0);
}


TEST_CASE("Render: NAM blocks that disagree on the sample rate are an error naming the blocks", "[resample][render][errors]") {
  TempDir t;
  json j = absolute(identityPreset());
  j["paths"]["b"]["blocks"][0]["model"]["file"] = identity44(t).string();
  const auto x = noise(2000, 2);

  try {
    renderPreset(parsePreset(j, kPresetDir), mono(x, 48000.0));
    FAIL("expected RenderError");
  } catch (const RenderError& e) {
    REQUIRE(e.kind() == RenderErrorKind::Preset);
    REQUIRE_THAT(std::string(e.what()), ContainsSubstring("paths.a.blocks[0]"));
    REQUIRE_THAT(std::string(e.what()), ContainsSubstring("paths.b.blocks[0]"));
    REQUIRE_THAT(std::string(e.what()), ContainsSubstring("48000"));
    REQUIRE_THAT(std::string(e.what()), ContainsSubstring("44100"));
  }

  SECTION("CLI exits 3 and names the blocks") {
    { std::ofstream(t / "p.json") << j.dump(); }
    writeWavFloat32(t / "in.wav", 44100.0, x);
    REQUIRE(runCli("--preset " + q(t / "p.json") + " --in " + q(t / "in.wav") + " --out " + q(t / "o.wav"), t / "err.txt") == 3);
    REQUIRE_THAT(slurp(t / "err.txt"), ContainsSubstring("paths.b.blocks[0]"));
    REQUIRE_FALSE(fs::exists(t / "o.wav"));
  }
  SECTION("a bypassed block does not take part") {
    j["paths"]["b"]["blocks"][0]["bypass"] = true;
    const RenderResult r = renderPreset(parsePreset(j, kPresetDir), mono(x, 48000.0));
    REQUIRE(r.renderRate == 48000.0);
  }
  SECTION("a disabled path does not take part") {
    j["paths"]["b"]["enabled"] = false;
    const RenderResult r = renderPreset(parsePreset(j, kPresetDir), mono(x, 48000.0));
    REQUIRE(r.renderRate == 48000.0);
  }
}

TEST_CASE("Render: auto rate: input rate without NAM blocks, 48 kHz for models with no recorded rate", "[resample][render]") {
  const auto x = noise(3000, 4);
  json eqOnly = identityPreset();
  for (const char* path : {"a", "b"})
    eqOnly["paths"][path]["blocks"] = json::array({{{"id", std::string("e") + path}, {"type", "eq"}, {"bands", json::array({{{"type", "lowPass"}, {"freq", 8000.0}}})}}});
  const RenderResult r = renderPreset(parsePreset(eqOnly, kPresetDir), mono(x, 44100.0));
  REQUIRE(r.renderRate == 44100.0);
  REQUIRE(r.outputRate == 44100.0);
  REQUIRE(r.resampleSeconds == 0.0);

  // A model that records no sample rate counts as 48 kHz (NAM convention), like the plugin.
  TempDir t;
  std::string nam = slurp(kFixtures / "nam" / "linear_identity.nam");
  const auto pos = nam.find(",\"sample_rate\":48000");
  REQUIRE(pos != std::string::npos);
  nam.erase(pos, 20);
  { std::ofstream(t / "norate.nam") << nam; }
  json agnostic = absolute(identityPreset());
  agnostic["paths"]["a"]["blocks"][0]["model"]["file"] = (t / "norate.nam").string();
  agnostic["paths"]["b"]["blocks"][0]["model"]["file"] = (t / "norate.nam").string();
  REQUIRE(renderPreset(parsePreset(agnostic, kPresetDir), mono(x, 44100.0)).renderRate == 48000.0);
  REQUIRE(probeNamRates(parsePreset(agnostic, kPresetDir)).front().recorded == false);
}

TEST_CASE("CLI: 44.1 kHz input through the 48 kHz WaveNet fixture", "[resample][cli]") {
  TempDir t;
  json j = absolute(identityPreset());
  for (const char* path : {"a", "b"}) j["paths"][path]["blocks"][0]["model"]["file"] = (kFixtures / "nam" / "wavenet.nam").string();
  { std::ofstream(t / "p.json") << j.dump(); }
  constexpr std::size_t N = 44100 + 17;
  writeWavFloat32(t / "in.wav", 44100.0, sine(220.0, 44100.0, N, 0.3));
  const std::string base = "--preset " + q(t / "p.json") + " --in " + q(t / "in.wav") + " --out " + q(t / "o.wav") + " --report " + q(t / "r.json");

  REQUIRE(runCli(base, t / "err.txt") == 0);
  {
    const AudioFile o = readWav(t / "o.wav");
    REQUIRE(o.sampleRate == 44100.0);
    REQUIRE(o.interleaved.size() == N);
    const json r = json::parse(slurp(t / "r.json"));
    REQUIRE(r["inputRate"] == 44100.0);
    REQUIRE(r["renderRate"] == 48000.0);
    REQUIRE(r["outputRate"] == 44100.0);
    REQUIRE(r["sampleRate"] == 44100.0);
    REQUIRE(r["frames"] == N);
    REQUIRE(r["output"]["peakDbfs"].get<double>() > -80.0);  // not silence
  }
  REQUIRE(runCli(base + " --out-rate render", t / "err.txt") == 0);
  {
    const AudioFile o = readWav(t / "o.wav");
    REQUIRE(o.sampleRate == 48000.0);
    REQUIRE(o.interleaved.size() == static_cast<std::size_t>(std::llround(N * 48000.0 / 44100.0)));
    const json r = json::parse(slurp(t / "r.json"));
    REQUIRE(r["outputRate"] == 48000.0);
  }
  // Explicit rates: a forced rate the model cannot run at is a preset error.
  REQUIRE(runCli(base + " --render-rate 48000", t / "err.txt") == 0);
  REQUIRE(runCli(base + " --render-rate 44100", t / "err.txt") == 3);
}

TEST_CASE("Render: latency trim happens at the render rate with a 44.1 kHz input", "[resample][render]") {
  test::registerLatencyStub();
  json j = identityPreset();
  j["paths"]["a"]["blocks"] = json::array({{{"id", "s1"}, {"type", "test.latency"}, {"latency", 100}}});
  const auto x = multiSine(30001, 44100.0);
  const RenderResult r = renderPreset(parsePreset(j, kPresetDir), mono(x, 44100.0));
  REQUIRE(r.renderRate == 48000.0);
  REQUIRE(r.info.latencySamples == 100);
  REQUIRE(r.samples.size() == x.size());
  double worst = 0.0;
  for (std::size_t i = 500; i < x.size() - 500; ++i) worst = std::max(worst, std::fabs(static_cast<double>(r.samples[i]) - x[i]));
  INFO("max diff " << worst);
  REQUIRE(worst < 1e-3);
}

TEST_CASE("Resample: unity DC gain for every phase", "[resample]") {
  struct R { double from, to; };
  for (const R d : {R{44100.0, 48000.0}, R{48000.0, 44100.0}, R{44100.0, 96000.0}, R{44100.0, 47999.37}}) {
    const std::vector<float> x(20000, 1.0f);
    const auto y = resample(x, d.from, d.to);
    double worst = 0.0;
    for (std::size_t i = 300; i + 300 < y.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(y[i]) - 1.0));
    INFO(d.from << " -> " << d.to << ": max |y-1| = " << worst);
    REQUIRE(worst < 1e-6);
  }
}

TEST_CASE("Render: an out-of-range render rate is a Preset error", "[resample][render][errors]") {
  for (const double rate : {999.0, 768001.0}) {
    RenderOptions o;
    o.renderRate = rate;
    try {
      renderPreset(identity(), mono(noise(1000, 1), 48000.0), o);
      FAIL("expected RenderError");
    } catch (const RenderError& e) {
      REQUIRE(e.kind() == RenderErrorKind::Preset);
    }
  }
}
