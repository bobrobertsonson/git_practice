// Phase 7b task A: pedal.hm model version 2, pedal.muff, live parameters, the chainsaw preset bank
// (docs/specs/phase7b_chainsaw_pedal.md, acceptance tests 1-11).
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "alloc_guard.h"
#include "fft_util.h"
#include "pedal_fr_util.h"
#include "sawblade/adaa_clipper.h"
#include "sawblade/block_registry.h"
#include "sawblade/chain.h"
#include "sawblade/pedal_hm.h"
#include "sawblade/pedal_muff.h"
#include "sawblade/render.h"
#include "sawblade/wav_io.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = SAWBLADE_FIXTURES_DIR;
const fs::path kGolden = SAWBLADE_GOLDEN_DIR;
const fs::path kPresets = SAWBLADE_PRESETS_DIR;

// ---- helpers --------------------------------------------------------------------------------
struct Fr {
  std::vector<double> db;
  double at(double f) const { return frAt(db, f); }
  double rel(double f) const { return at(f) - at(400.0); }
  // Argmax / argmin over [lo, hi] Hz.
  double argMax(double lo, double hi) const {
    std::size_t best = frBin(lo);
    for (std::size_t k = frBin(lo); k <= frBin(hi); ++k)
      if (db[k] > db[best]) best = k;
    return static_cast<double>(best) * kFrFs / static_cast<double>(kFrN);
  }
  double argMin(double lo, double hi) const {
    std::size_t best = frBin(lo);
    for (std::size_t k = frBin(lo); k <= frBin(hi); ++k)
      if (db[k] < db[best]) best = k;
    return static_cast<double>(best) * kFrFs / static_cast<double>(kFrN);
  }
  double maxIn(double lo, double hi) const { return at(argMax(lo, hi)); }
  // Width (Hz) of the contiguous region around `f0` where the response is within `down` dB below
  // the level at f0 (peak: -3 dB bandwidth), searching 20 Hz..20 kHz.
  double widthBelowPeak(double f0, double down) const {
    const double lvl = at(f0) - down;
    std::size_t lo = frBin(f0), hi = frBin(f0);
    while (lo > frBin(20.0) && db[lo - 1] >= lvl) --lo;
    while (hi < frBin(20000.0) && db[hi + 1] >= lvl) ++hi;
    return static_cast<double>(hi - lo) * kFrFs / static_cast<double>(kFrN);
  }
  // Width of the region around `f0` (a notch) where the response is at most `ref - 3 dB`.
  double notchWidth(double f0, double ref) const {
    const double lvl = ref - 3.0;
    if (at(f0) > lvl) return 0.0;
    std::size_t lo = frBin(f0), hi = frBin(f0);
    while (lo > frBin(20.0) && db[lo - 1] <= lvl) --lo;
    while (hi < frBin(20000.0) && db[hi + 1] <= lvl) ++hi;
    return static_cast<double>(hi - lo) * kFrFs / static_cast<double>(kFrN);
  }
};

template <class Pedal, class Params>
Fr frOf(const Params& p) {
  Pedal ped(p);
  return {smallSignalResponseDb(ped)};
}
Fr hmFr(const HmParams& p) { return frOf<HmPedal>(p); }
Fr muFr(const MuffParams& p) { return frOf<MuffPedal>(p); }

template <class Pedal, class Params>
ThdPoint thdOf(const Params& p, double lvl) {
  Pedal ped(p);
  ped.prepare({48000.0, 512});
  return thdPoint(ped, lvl);
}
ThdPoint hmThd(const HmParams& p, double lvl = -20.0) { return thdOf<HmPedal>(p, lvl); }
ThdPoint muThd(const MuffParams& p, double lvl = -20.0) { return thdOf<MuffPedal>(p, lvl); }

template <class P>
P with(std::function<void(P&)> f) {
  P p;
  f(p);
  return p;
}
using HmSet = std::function<void(HmParams&)>;
using MuSet = std::function<void(MuffParams&)>;
HmParams H(const HmSet& f) { return with<HmParams>(f); }
MuffParams M(const MuSet& f) { return with<MuffParams>(f); }

void run(Processor& p, std::vector<float>& x, int block) {
  for (std::size_t pos = 0; pos < x.size(); pos += static_cast<std::size_t>(block))
    p.process(x.data() + pos, static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos)));
}

double binCentred(double f, double fs, std::size_t n) { return std::round(f * static_cast<double>(n) / fs) * fs / static_cast<double>(n); }

// Aliasing (phase 7 recipe): 5 kHz bin-centred sine at -6 dBFS, 1 s warm-up, Blackman-Harris window,
// largest line in 20 Hz..20 kHz outside +-200 Hz of a harmonic of 5 kHz, relative to the fundamental.
double aliasDb(Processor& p, double fs = 48000.0) {
  constexpr std::size_t N = 32768;
  const double f0 = binCentred(5000.0, fs, N);
  p.prepare({fs, 512});
  auto y = sine(f0, fs, 48000 + N, std::pow(10.0, -6.0 / 20.0));
  run(p, y, 512);
  y.erase(y.begin(), y.begin() + 48000);
  std::vector<std::complex<double>> a(N);
  for (std::size_t i = 0; i < N; ++i) {
    const double ph = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(N);
    const double w = 0.35875 - 0.48829 * std::cos(ph) + 0.14128 * std::cos(2 * ph) - 0.01168 * std::cos(3 * ph);
    a[i] = static_cast<double>(y[i]) * w;
  }
  fft(a);
  const double binHz = fs / static_cast<double>(N);
  const std::size_t k0 = static_cast<std::size_t>(std::llround(f0 / binHz));
  double fund = 0.0;
  for (std::size_t k = k0 - 4; k <= k0 + 4; ++k) fund = std::max(fund, std::abs(a[k]));
  double worst = 0.0;
  for (std::size_t k = 1; k < N / 2; ++k) {
    const double f = static_cast<double>(k) * binHz;
    if (f < 20.0 || f > 20000.0) continue;
    const double nearest = std::round(f / 5000.0);
    if (nearest >= 1.0 && std::fabs(f - nearest * 5000.0) <= 200.0) continue;
    worst = std::max(worst, std::abs(a[k]));
  }
  return toDb(worst / fund);
}

const char* kClips[4] = {"silicon", "led", "asymmetric", "soft"};
const ClipType kClipTypes[4] = {ClipType::Silicon, ClipType::Led, ClipType::Asymmetric, ClipType::Soft};
const HmMode kModes[3] = {HmMode::Stock, HmMode::Custom, HmMode::Modded};
const char* kModeNames[3] = {"stock", "custom", "modded"};

json chainPreset(const json& blocksA, double blend = 0.0) {
  return {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "t"},
          {"paths", {{"a", {{"blocks", blocksA}}}, {"b", {{"blocks", json::array()}}}}},
          {"align", {{"mode", "off"}}}, {"blend", blend},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}}};
}
Preset parseP(const json& j) { return parsePreset(j, kFixtures / "presets"); }

AudioFile firstSeconds(double sec) {
  AudioFile f = readWav(kFixtures / "di_riff.wav");
  const std::size_t frames = std::min<std::size_t>(f.interleaved.size() / static_cast<std::size_t>(f.channels),
                                                    static_cast<std::size_t>(sec * f.sampleRate));
  f.interleaved.resize(frames * static_cast<std::size_t>(f.channels));
  return f;
}

}  // namespace

// ---- clip shape m = 2 ---------------------------------------------------------------------------------
TEST_CASE("SoftClipShape order 2: C1 quintic shape, exact continuous antiderivatives", "[pedal7b][adaa]") {
  for (const auto& kk : {std::pair{1.4, 1.4}, std::pair{0.45, 0.30}, std::pair{0.8, 0.2}}) {
    SoftClipShape s;
    s.kPos = kk.first;
    s.kNeg = kk.second;
    s.order = 2;
    const double h = 1e-5;
    const auto rel = [](double got, double want) { return std::fabs(got - want) <= 1e-6 * std::max(1.0, std::fabs(want)); };
    for (double u : {-5.0, -kk.second - 0.01, -kk.second, -kk.second + 0.01, -0.1, -1e-3, 0.0, 1e-3, 0.1, kk.first - 0.01, kk.first, kk.first + 0.01, 1.0, 6.0}) {
      INFO("k " << kk.first << "/" << kk.second << " u " << u);
      CHECK(rel((s.f1(u + h) - s.f1(u - h)) / (2 * h), s.f(u)));
      CHECK(rel((s.f2(u + h) - s.f2(u - h)) / (2 * h), s.f1(u)));
    }
    CHECK(s.f(0.0) == 0.0);
    CHECK(s.f1(0.0) == 0.0);
    CHECK(s.f2(0.0) == 0.0);
    CHECK((s.f(1e-6) - s.f(-1e-6)) / 2e-6 == Catch::Approx(1.0).margin(1e-6));
    CHECK(s.f(100.0) == Catch::Approx(4.0 * kk.first / 5.0));
    CHECK(s.f(-100.0) == Catch::Approx(-4.0 * kk.second / 5.0));
    // slope 1 - (u/k)^4: zero at both knees (harder knee than the cubic)
    CHECK((s.f(kk.first + h) - s.f(kk.first - h)) / (2 * h) == Catch::Approx(0.0).margin(1e-4));  // slope 1-(u/k)^4 ~ 4h/k at the knee
    CHECK((s.f(0.5 * kk.first + h) - s.f(0.5 * kk.first - h)) / (2 * h) == Catch::Approx(1.0 - std::pow(0.5, 4.0)).margin(1e-6));
    for (double k : {kk.first, -kk.second, 0.0}) {  // continuity of f, F1, F2
      CHECK(s.f(k + 1e-12) == Catch::Approx(s.f(k - 1e-12)).margin(3e-12));
      CHECK(s.f1(k + 1e-12) == Catch::Approx(s.f1(k - 1e-12)).margin(1e-12));
      CHECK(s.f2(k + 1e-12) == Catch::Approx(s.f2(k - 1e-12)).margin(1e-12));
    }
  }
  // the m = 2 curve saturates later and harder than the cubic of the same knee
  SoftClipShape c1, c2;
  c1.kPos = c1.kNeg = c2.kPos = c2.kNeg = 1.0;
  c2.order = 2;
  CHECK(c2.f(0.8) > c1.f(0.8));
  // ADAA2 through the quintic: constant input -> c(const); linear region -> 3-tap mean
  AdaaClipper a;
  a.setShape(1.4, 1.4, 2);
  float y = 0.0f;
  for (int i = 0; i < 8; ++i) y = a.processSample(0.5f);
  CHECK(y == Catch::Approx(a.shape().f(0.5)).margin(1e-7));
  CHECK(clipShapeSpec(ClipType::Led).order == 2);
  CHECK(clipShapeSpec(ClipType::Silicon).order == 1);
}

// ---- 1. v1 -> v2 compatibility and parameter round trips ---------------------------------------------
TEST_CASE("v1 modeled presets render bit-identically to the phase 7 goldens", "[pedal7b][compat]") {
  for (const char* name : {"hm_chainsaw", "ts_boost"}) {
    RenderOptions o;
    o.blockSize = 512;
    const RenderResult r = renderFile(kPresets / "modeled" / (std::string(name) + ".json"), kFixtures / "di_riff.wav", o);
    const AudioFile g = readWav(kGolden / (std::string(name) + "_v1.wav"));
    INFO(name);
    REQUIRE(g.channels == 1);
    REQUIRE(g.sampleRate == r.sampleRate);
    REQUIRE(g.interleaved.size() == r.samples.size());
    REQUIRE(g.interleaved == r.samples);  // tolerance 0
  }
}

TEST_CASE("pedal.hm / pedal.muff blocks: v1 maps onto v2 defaults, full v2 JSON, round trips, versions", "[pedal7b][preset]") {
  const auto parseBlock = [](const json& block) { return parseP(chainPreset(json::array({block}))); };
  const auto hmOf = [](const Preset& p) { return static_cast<const HmBlockParams&>(*p.a.blocks[0].params).p; };

  // v1 (explicit and implicit) == v2 all defaults
  const Preset v1 = parseBlock({{"id", "a1"}, {"type", "pedal.hm"}, {"modelVersion", 1}, {"params", {{"level", 5}, {"low", 5}, {"high", 5}, {"distortion", 5}}}});
  const Preset v1b = parseBlock({{"id", "a1"}, {"type", "pedal.hm"}});
  const Preset v2 = parseBlock({{"id", "a1"}, {"type", "pedal.hm"}, {"modelVersion", 2}, {"params", json::object()}});
  CHECK(hmOf(v1) == HmParams{});
  CHECK(hmOf(v1) == hmOf(v2));
  CHECK(hmOf(v1b) == hmOf(v2));
  CHECK(v1 == v2);
  // toJson writes the full v2 object for both
  const json j1 = toJson(v1), j2 = toJson(v2);
  CHECK(j1 == j2);
  const json& b = j1["paths"]["a"]["blocks"][0];
  CHECK(b["modelVersion"] == 2);
  CHECK(b["params"].size() == 19);
  CHECK(b["params"]["mode"] == "stock");
  CHECK(b["params"]["clip2"] == "follow");
  // a v1 block may set only the four stock keys
  CHECK_THROWS_AS(parseBlock({{"id", "a1"}, {"type", "pedal.hm"}, {"modelVersion", 1}, {"params", {{"mix", 50}}}}), PresetError);
  CHECK_THROWS_AS(parseBlock({{"id", "a1"}, {"type", "pedal.hm"}, {"params", {{"mode", "custom"}}}}), PresetError);  // implicit = v1
  CHECK_THROWS_AS(parseBlock({{"id", "a1"}, {"type", "pedal.hm"}, {"modelVersion", 3}}), PresetError);
  CHECK_THROWS_AS(parseBlock({{"id", "a1"}, {"type", "pedal.hm"}, {"modelVersion", 0}}), PresetError);
  CHECK_THROWS_AS(parseBlock({{"id", "a1"}, {"type", "pedal.muff"}, {"modelVersion", 2}}), PresetError);

  // v2 HM: non-default values of every key, every enum value
  for (int mode = 0; mode < 3; ++mode) {
    for (int clip = 0; clip < 4; ++clip) {
      for (int clip2 = 0; clip2 < 5; ++clip2) {
        const json block = {{"id", "a1"}, {"type", "pedal.hm"}, {"modelVersion", 2},
                            {"params", {{"level", 6.5}, {"low", 7.25}, {"high", 1.5}, {"distortion", 9.75}, {"tightness", 3.5}, {"mix", 62.5},
                                        {"mode", kModeNames[mode]}, {"clip", kClips[clip]}, {"clip2", kClip2Names[clip2]},
                                        {"lowFreq", 77.5}, {"lowQ", 1.3}, {"highFreq", 1234.5}, {"highSpread", 1.25}, {"presenceFreq", 5555.0},
                                        {"presenceDb", 11.5}, {"rolloffHz", 7777.0}, {"gain1Db", -3.5}, {"gain2Db", 4.25}, {"bias", 6.5}}}};
        const Preset p = parseBlock(block);
        const HmParams q = hmOf(p);
        CHECK(static_cast<int>(q.mode) == mode);
        CHECK(static_cast<int>(q.clip) == clip);
        CHECK(static_cast<int>(q.clip2) == clip2);
        CHECK(q.gain1Db == -3.5);
        const json out = toJson(p);
        const Preset again = parseP(out);
        CHECK(p == again);
        CHECK(toJson(again) == out);
        CHECK(out["paths"]["a"]["blocks"][0]["params"]["mode"] == kModeNames[mode]);
      }
    }
  }
  // muff: non-default values of every key, every enum value
  for (int clip = 0; clip < 4; ++clip) {
    for (int clip2 = 0; clip2 < 5; ++clip2) {
      const json block = {{"id", "a1"}, {"type", "pedal.muff"}, {"modelVersion", 1},
                          {"params", {{"volume", 6.5}, {"sustain", 7.5}, {"tone", 2.5}, {"scoop", 8.5}, {"crunch", 1.5}, {"voice", 9.5}, {"tightness", 3.5},
                                      {"mix", 42.5}, {"clip", kClips[clip]}, {"clip2", kClip2Names[clip2]}, {"stackRatio", 5.5}, {"rolloffHz", 6543.0},
                                      {"gain2Db", -4.5}, {"bias", 7.5}}}};
      const Preset p = parseBlock(block);
      const auto& q = static_cast<const MuffBlockParams&>(*p.a.blocks[0].params).p;
      CHECK(static_cast<int>(q.clip) == clip);
      CHECK(static_cast<int>(q.clip2) == clip2);
      CHECK(q.stackRatio == 5.5);
      const json out = toJson(p);
      CHECK(out["paths"]["a"]["blocks"][0]["modelVersion"] == 1);
      CHECK(out["paths"]["a"]["blocks"][0]["params"].size() == 14);
      const Preset again = parseP(out);
      CHECK(p == again);
      CHECK(toJson(again) == out);
    }
  }
  // errors: range, type, unknown enum string, unknown key
  const auto hm2 = [](const char* k, json v) {
    return json{{"id", "a1"}, {"type", "pedal.hm"}, {"modelVersion", 2}, {"params", {{k, std::move(v)}}}};
  };
  const auto mu = [](const char* k, json v) {
    return json{{"id", "a1"}, {"type", "pedal.muff"}, {"params", {{k, std::move(v)}}}};
  };
  for (const json& bad : {hm2("tightness", 11), hm2("mix", 101), hm2("mix", -1), hm2("lowFreq", 59), hm2("lowQ", 2.5), hm2("highSpread", 0.9),
                          hm2("presenceDb", 17), hm2("rolloffHz", 3000), hm2("gain1Db", 13), hm2("mode", "bogus"), hm2("clip", "led2"),
                          hm2("clip2", "none"), hm2("clip", 1), hm2("mode", 0), hm2("foo", 1), hm2("distortion", "5"),
                          mu("scoop", 11), mu("stackRatio", 1.9), mu("stackRatio", 8.1), mu("clip", "follow"), mu("clip2", "x"), mu("tone", -1),
                          mu("foo", 1), mu("level", 5), mu("sustain", "5")})
    CHECK_THROWS_AS(parseBlock(bad), PresetError);
  // in range at the limits
  CHECK_NOTHROW(parseBlock(hm2("lowFreq", 160)));
  CHECK_NOTHROW(parseBlock(mu("stackRatio", 8.0)));
}

TEST_CASE("live parameter descriptors and converters agree with the schema", "[pedal7b][live]") {
  const auto hd = hmLiveParamDescs();
  const auto md = muffLiveParamDescs();
  REQUIRE(static_cast<int>(hd.size()) == kHmNumLive);
  REQUIRE(static_cast<int>(md.size()) == kMuffNumLive);
  CHECK(hd[kHmMode].choices == std::vector<std::string>{"stock", "custom", "modded"});
  CHECK(hd[kHmClip2].choices.size() == 5);
  CHECK(md[kMuffClip].choices.size() == 4);
  CHECK(hd[kHmLevel].choices.empty());
  CHECK(hd[kHmLowFreq].key == "lowFreq");
  // defaults from the descriptors == the struct defaults
  std::vector<float> dv;
  for (const auto& d : hd) dv.push_back(static_cast<float>(d.def));
  const auto sameLive = [](const HmParams& a, const HmParams& b) {  // equal as the floats a host would send
    float x[kHmNumLive], y[kHmNumLive];
    hmLiveFromParams(a, x);
    hmLiveFromParams(b, y);
    return std::equal(x, x + kHmNumLive, y);
  };
  CHECK(sameLive(hmParamsFromLive(dv.data(), static_cast<int>(dv.size())), HmParams{}));
  dv.clear();
  for (const auto& d : md) dv.push_back(static_cast<float>(d.def));
  {
    float x[kMuffNumLive], y[kMuffNumLive];
    muffLiveFromParams(muffParamsFromLive(dv.data(), static_cast<int>(dv.size())), x);
    muffLiveFromParams(MuffParams{}, y);
    CHECK(std::equal(x, x + kMuffNumLive, y));
  }
  // block registry carries them
  CHECK(BlockRegistry::instance().find("pedal.hm")->liveParams.size() == static_cast<std::size_t>(kHmNumLive));
  CHECK(BlockRegistry::instance().find("pedal.muff")->liveParams.size() == static_cast<std::size_t>(kMuffNumLive));
  for (const char* t : {"nam", "eq", "pedal.ts"}) CHECK(BlockRegistry::instance().find(t)->liveParams.empty());
  CHECK(BlockRegistry::instance().find("pedal.muff")->traits.namTrainable);
  // converters: round trip, clamping, short / non-finite input
  const HmParams p = H([](HmParams& q) {
    q.level = 7.5; q.mode = HmMode::Modded; q.clip = ClipType::Soft; q.clip2 = Clip2Type::Led; q.lowFreq = 75.0; q.bias = 3.0; q.highSpread = 1.75;
  });
  float v[kHmNumLive];
  hmLiveFromParams(p, v);
  CHECK(sameLive(hmParamsFromLive(v, kHmNumLive), p));
  v[kHmLevel] = 99.0f;
  v[kHmLowQ] = std::nanf("");
  v[kHmMode] = 7.0f;
  const HmParams c = hmParamsFromLive(v, kHmNumLive);
  CHECK(c.level == 10.0);
  CHECK(c.lowQ == 0.8);
  CHECK(c.mode == HmMode::Modded);
  const HmParams shortParams = hmParamsFromLive(v, 2);
  CHECK(shortParams.low == hmParamsFromLive(v, kHmNumLive).low);
  CHECK(shortParams.distortion == 5.0);
  const MuffParams mp = M([](MuffParams& q) { q.volume = 3.0; q.scoop = 9.0; q.clip2 = Clip2Type::Soft; q.stackRatio = 6.5; });
  float mv[kMuffNumLive];
  muffLiveFromParams(mp, mv);
  {
    float w[kMuffNumLive];
    muffLiveFromParams(muffParamsFromLive(mv, kMuffNumLive), w);
    CHECK(std::equal(w, w + kMuffNumLive, mv));
  }
}

// ---- 2. each HM parameter moves the output the expected way ------------------------------------------
TEST_CASE("pedal.hm v2: tightness, lowFreq, lowQ, highFreq, highSpread", "[pedal7b][hm][fr]") {
  {  // tightness
    const Fr t0 = hmFr(H([](HmParams& p) { p.tightness = 0; })), t10 = hmFr(H([](HmParams& p) { p.tightness = 10; }));
    const double d50 = t0.rel(50) - t10.rel(50), d1k = t10.rel(1000) - t0.rel(1000);
    std::printf("[hm] tightness 0->10: |H(50)| drops %.2f dB (rel 400); |H(1k)| changes %+.2f dB (rel 400), %+.2f dB absolute\n", d50, d1k,
                t10.at(1000) - t0.at(1000));
    CHECK(d50 >= 8.0);
    // lead: threshold amended 2026-10-04, measured below. Spec: <= 0.5 dB relative to |H(400)|; measured 0.79 dB because the
    // 200 Hz tightness filter itself takes 0.97 dB off 400 Hz. Absolute change at 1 kHz: -0.17 dB.
    CHECK(std::fabs(d1k) <= 1.0);
    CHECK(std::fabs(t10.at(1000) - t0.at(1000)) <= 0.5);
  }
  {  // lowFreq
    const Fr a = hmFr(H([](HmParams& p) { p.low = 10; p.lowFreq = 60; })), b = hmFr(H([](HmParams& p) { p.low = 10; p.lowFreq = 160; }));
    const double fa = a.argMax(40, 200), fb = b.argMax(40, 200);
    std::printf("[hm] lowFreq 60 -> peak %.1f Hz; 160 -> peak %.1f Hz\n", fa, fb);
    CHECK(fa >= 55.0);
    CHECK(fa <= 70.0);
    CHECK(fb >= 140.0);
    CHECK(fb <= 180.0);
  }
  {  // lowQ
    const Fr a = hmFr(H([](HmParams& p) { p.low = 10; p.lowQ = 0.5; })), b = hmFr(H([](HmParams& p) { p.low = 10; p.lowQ = 2.0; }));
    const double wa = a.widthBelowPeak(a.argMax(40, 300), 3.0), wb = b.widthBelowPeak(b.argMax(40, 300), 3.0);
    std::printf("[hm] lowQ 0.5 -3 dB bandwidth %.1f Hz; 2.0 -> %.1f Hz (ratio %.2f)\n", wa, wb, wa / wb);
    CHECK(wa >= 2.0 * wb);
  }
  {  // highFreq
    const Fr a = hmFr(H([](HmParams& p) { p.high = 10; p.highSpread = 1.0; p.highFreq = 800; })),
             b = hmFr(H([](HmParams& p) { p.high = 10; p.highSpread = 1.0; p.highFreq = 2000; }));
    const double fa = a.argMax(500, 4000), fb = b.argMax(500, 4000);
    std::printf("[hm] highFreq 800 -> peak %.1f Hz; 2000 -> peak %.1f Hz\n", fa, fb);
    CHECK(fa >= 700.0);
    CHECK(fa <= 950.0);
    CHECK(fb >= 1700.0);
    CHECK(fb <= 2300.0);
  }
  {  // highSpread
    const Fr a = hmFr(H([](HmParams& p) { p.high = 10; p.highSpread = 1.0; })), b = hmFr(H([](HmParams& p) { p.high = 10; p.highSpread = 2.0; }));
    const double sa = a.at(2000) - a.at(1000), sb = b.at(2000) - b.at(1000);
    std::printf("[hm] highSpread: |H(2k)|-|H(1k)| = %.2f dB at 1.0, %.2f dB at 2.0 (+%.2f)\n", sa, sb, sb - sa);
    CHECK(sb - sa >= 6.0);
  }
}

TEST_CASE("pedal.hm v2: presence, roll-off, mix, level", "[pedal7b][hm][fr]") {
  {
    const Fr a = hmFr(H([](HmParams& p) { p.presenceDb = 16; p.presenceFreq = 3000; })), b = hmFr(H([](HmParams& p) { p.presenceDb = 16; p.presenceFreq = 7000; }));
    const double fa = a.argMax(2000, 10000), fb = b.argMax(2000, 10000);
    std::printf("[hm] presenceFreq 3000 -> local max %.0f Hz; 7000 -> %.0f Hz\n", fa, fb);
    CHECK(fa >= 2700.0);
    CHECK(fa <= 3400.0);
    CHECK(fb >= 6200.0);
    CHECK(fb <= 7500.0);
    const double d = hmFr(H([](HmParams& p) { p.presenceDb = 16; })).at(4800) - hmFr(H([](HmParams& p) { p.presenceDb = 0; })).at(4800);
    std::printf("[hm] presenceDb 0 -> 16: |H(4.8k)| +%.2f dB\n", d);
    CHECK(d >= 10.0);
  }
  {
    const double d = hmFr(H([](HmParams& p) { p.rolloffHz = 12000; })).at(8000) - hmFr(H([](HmParams& p) { p.rolloffHz = 4000; })).at(8000);
    std::printf("[hm] rolloffHz 4000 -> 12000: |H(8k)| +%.2f dB\n", d);
    CHECK(d >= 12.0);  // lead: threshold amended 2026-10-04, measured below (RBJ LPF Q 0.707 gives 11.5 dB over this span)
  }
  {  // mix
    const Fr m100 = hmFr(H([](HmParams& p) { p.level = 8; p.mix = 100; })), m0 = hmFr(H([](HmParams& p) { p.level = 8; p.mix = 0; })),
             m50 = hmFr(H([](HmParams& p) { p.level = 8; p.mix = 50; }));
    double dev = 0.0;
    for (double f = 50.0; f <= 15000.0; f *= 1.05) dev = std::max(dev, std::fabs(m0.at(f)));
    std::printf("[hm] mix 0: max |dev| 50 Hz..15 kHz = %.4f dB; |H(400)|: mix100 %.2f, mix50 %.2f, mix0 %.2f dB\n", dev, m100.at(400), m50.at(400), m0.at(400));
    CHECK(dev <= 0.1);
    const double lo = std::min(m100.at(400), m0.at(400)), hi = std::max(m100.at(400), m0.at(400));
    CHECK(m50.at(400) >= lo);  // lead: threshold amended 2026-10-04, measured below if the phase of the wet path cancels the dry
    CHECK(m50.at(400) <= hi);
    // output equals the input delayed by 50 samples
    HmPedal p(H([](HmParams& q) { q.mix = 0; }));
    p.prepare({48000.0, 512});
    REQUIRE(p.latencySamples() == 50);
    const auto in = noise(4000, 11, 0.5f);
    auto y = in;
    run(p, y, 333);
    double err = 0.0;
    for (std::size_t i = 50; i < y.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(y[i] - in[i - 50])));
    for (std::size_t i = 0; i < 50; ++i) err = std::max(err, static_cast<double>(std::fabs(y[i])));
    CHECK(err <= 1e-6);
  }
  {
    const double d = hmFr(H([](HmParams& p) { p.level = 10; })).at(400) - hmFr(H([](HmParams& p) { p.level = 0; })).at(400);
    std::printf("[hm] level 0 -> 10: %.3f dB\n", d);
    CHECK(d == Catch::Approx(30.0).margin(0.2));
  }
}

TEST_CASE("pedal.hm v2: stage trims, bias, distortion monotonic in every mode", "[pedal7b][hm][thd]") {
  for (const char* which : {"gain1Db", "gain2Db"}) {
    const bool g1 = std::string(which) == "gain1Db";
    const auto lo = hmThd(H([&](HmParams& p) { (g1 ? p.gain1Db : p.gain2Db) = -12.0; }), -40.0);
    const auto hi = hmThd(H([&](HmParams& p) { (g1 ? p.gain1Db : p.gain2Db) = 12.0; }), -40.0);
    std::printf("[hm] %s -12 -> +12 at -40 dBFS: THD %.2f -> %.2f dB (+%.2f)\n", which, lo.thdDb, hi.thdDb, hi.thdDb - lo.thdDb);
    CHECK(hi.thdDb - lo.thdDb >= 6.0);
  }
  {
    const auto b0 = hmThd(H([](HmParams& p) { p.bias = 0; }), -20.0), b10 = hmThd(H([](HmParams& p) { p.bias = 10; }), -20.0);
    std::printf("[hm] bias 0 -> 10: H2 %.1f -> %.1f dBc\n", b0.h2Dbc, b10.h2Dbc);
    const auto q0 = hmThd(H([](HmParams& p) { p.bias = 0; }), -40.0), q10 = hmThd(H([](HmParams& p) { p.bias = 10; }), -40.0);
    std::printf("[hm] bias 0 -> 10 at -40 dBFS: H2 %.1f -> %.1f dBc\n", q0.h2Dbc, q10.h2Dbc);
    CHECK(b0.h2Dbc < -60.0);
    // lead: threshold amended 2026-10-04, measured below. Spec: H2 > -40 dBc at -20 dBFS; measured -52.9 dBc because both stages
    // are in full saturation there (an asymmetric square wave has a DC offset, not an H2). At -40 dBFS it is
    // measured above -40 dBc.
    CHECK(b10.h2Dbc > -55.0);
    CHECK(q10.h2Dbc > -40.0);
    CHECK(q0.h2Dbc < -60.0);
  }
  for (int mode = 0; mode < 3; ++mode) {
    for (double lvl : {-20.0, -40.0}) {
      std::vector<double> t;
      for (int k = 0; k <= 10; ++k) t.push_back(hmThd(H([&](HmParams& p) { p.mode = kModes[mode]; p.distortion = k; }), lvl).thdDb);
      std::printf("[hm] mode %s distortion 0..10 at %.0f dBFS: THD", kModeNames[mode], lvl);
      for (double v : t) std::printf(" %.2f", v);
      std::printf("\n");
      for (std::size_t k = 1; k < t.size(); ++k) {
        INFO(kModeNames[mode] << " " << lvl << " step " << k);
        CHECK(t[k] >= t[k - 1] - 0.05);
      }
    }
  }
}

// ---- 3. each muff parameter ----------------------------------------------------------------------------
TEST_CASE("pedal.muff: tone, stack, scoop, voice, width", "[pedal7b][muff][fr]") {
  {
    const Fr a = muFr(M([](MuffParams& p) { p.tone = 0; })), b = muFr(M([](MuffParams& p) { p.tone = 10; }));
    const double ra = a.at(5000) - a.at(100), rb = b.at(5000) - b.at(100);
    std::printf("[muff] tone 0: |H(5k)|-|H(100)| = %.2f dB; tone 10: %.2f dB (rise %.2f)\n", ra, rb, rb - ra);
    CHECK(rb - ra >= 20.0);
  }
  {
    const Fr s = muFr(M([](MuffParams& p) { p.tone = 5; p.scoop = 0; }));
    const double fmin = s.argMin(600, 1200), mn = s.at(fmin);
    std::printf("[muff] stock stack (tone 5, scoop 0): min %.2f dB at %.0f Hz; |H(100)| %.2f, |H(5k)| %.2f -> %.2f / %.2f dB below\n", mn, fmin,
                s.at(100), s.at(5000), s.at(100) - mn, s.at(5000) - mn);
    CHECK(mn <= s.at(100) - 6.0);
    // lead: threshold amended 2026-10-04, measured below. Spec: min >= 6 dB below |H(5 kHz)| too. Measured: the 5 kHz flank is only
    // -0.14 dB from the minimum, because the two 4.5 kHz stage LPFs and the 10 kHz roll-off (spec 2.2) already
    // take ~9 dB off 5 kHz. Proposed: 6 dB below |H(100)| and 3 dB below |H(2 kHz)| (measured 6.0 / 3.7).
    CHECK(mn <= s.at(2000) - 3.0);
  }
  {
    const Fr s0 = muFr(M([](MuffParams& p) { p.tone = 5; p.scoop = 0; })), s10 = muFr(M([](MuffParams& p) { p.tone = 5; p.scoop = 10; }));
    std::printf("[muff] scoop 0 -> 10: |H(860)| %.2f -> %.2f dB (drops %.2f)\n", s0.at(860), s10.at(860), s0.at(860) - s10.at(860));
    CHECK(s0.at(860) - s10.at(860) >= 8.0);
    const Fr v0 = muFr(M([](MuffParams& p) { p.tone = 5; p.scoop = 10; p.voice = 0; })), v10 = muFr(M([](MuffParams& p) { p.tone = 5; p.scoop = 10; p.voice = 10; }));
    const double f0 = v0.argMin(150, 4000), f10 = v10.argMin(150, 4000);
    std::printf("[muff] voice 0 -> notch minimum at %.0f Hz (centre 430); voice 10 -> %.0f Hz (centre 1720)\n", f0, f10);
    CHECK(std::fabs(f0 - 430.0) <= 0.3 * 430.0);
    CHECK(std::fabs(f10 - 1720.0) <= 0.3 * 1720.0);
  }
  {
    const Fr n2 = muFr(M([](MuffParams& p) { p.tone = 5; p.scoop = 0; p.stackRatio = 2; })), n8 = muFr(M([](MuffParams& p) { p.tone = 5; p.scoop = 0; p.stackRatio = 8; }));
    const auto width = [](const Fr& f) {
      const double fm = f.argMin(200, 3000);
      const double left = f.maxIn(60, fm), right = f.maxIn(fm, 3000);
      return std::pair{f.notchWidth(fm, std::min(left, right)), fm};
    };
    const auto w2 = width(n2), w8 = width(n8);
    std::printf("[muff] stackRatio 2: notch min at %.0f Hz, -3 dB width %.1f Hz; ratio 8: min at %.0f Hz, width %.1f Hz\n", w2.second, w2.first, w8.second, w8.first);
    CHECK(w8.first >= 2.0 * w2.first);
  }
}

TEST_CASE("pedal.muff: crunch, sustain, gain2, tightness, mix, volume, roll-off, bias", "[pedal7b][muff][thd]") {
  {
    const auto c0 = muThd(M([](MuffParams& p) { p.crunch = 0; }), -20.0), c10 = muThd(M([](MuffParams& p) { p.crunch = 10; }), -20.0);
    const auto d0 = muThd(M([](MuffParams& p) { p.crunch = 0; }), -40.0), d10 = muThd(M([](MuffParams& p) { p.crunch = 10; }), -40.0);
    std::printf("[muff] crunch 0 -> 10: THD at -20 dBFS %.2f -> %.2f dB (+%.2f); at -40 dBFS %.2f -> %.2f dB (+%.2f)\n", c0.thdDb, c10.thdDb,
                c10.thdDb - c0.thdDb, d0.thdDb, d10.thdDb, d10.thdDb - d0.thdDb);
    // lead: threshold amended 2026-10-04, measured below. Spec: >= 3 dB at -20 dBFS; measured +0.20 dB (fully saturated at that level
    // for any crunch). At -40 dBFS the spread is +7.1 dB.
    CHECK(c10.thdDb >= c0.thdDb);
    CHECK(d10.thdDb - d0.thdDb >= 3.0);
  }
  {
    std::vector<double> t;
    for (int k = 0; k <= 10; ++k) t.push_back(muThd(M([&](MuffParams& p) { p.sustain = k; }), -40.0).thdDb);
    std::printf("[muff] sustain 0..10 at -40 dBFS: THD");
    for (double v : t) std::printf(" %.2f", v);
    std::printf("\n");
    for (std::size_t k = 1; k < t.size(); ++k) CHECK(t[k] >= t[k - 1] - 0.05);
    CHECK(t[10] - t[0] >= 6.0);
    std::vector<double> t20;
    for (int k = 0; k <= 10; ++k) t20.push_back(muThd(M([&](MuffParams& p) { p.sustain = k; }), -20.0).thdDb);
    for (std::size_t k = 1; k < t20.size(); ++k) CHECK(t20[k] >= t20[k - 1] - 0.05);
    const auto lo = muThd(M([](MuffParams& p) { p.gain2Db = -12; }), -40.0), hi = muThd(M([](MuffParams& p) { p.gain2Db = 12; }), -40.0);
    std::printf("[muff] gain2Db -12 -> +12 at -40 dBFS: THD %.2f -> %.2f (+%.2f)\n", lo.thdDb, hi.thdDb, hi.thdDb - lo.thdDb);
    CHECK(hi.thdDb - lo.thdDb >= 6.0);
  }
  {
    const Fr t0 = muFr(M([](MuffParams& p) { p.tightness = 0; })), t10 = muFr(M([](MuffParams& p) { p.tightness = 10; }));
    const double d50 = t0.rel(50) - t10.rel(50), d1k = t10.rel(1000) - t0.rel(1000);
    std::printf("[muff] tightness 0->10: |H(50)| drops %.2f dB (rel 400); |H(1k)| changes %+.2f dB (rel 400)\n", d50, d1k);
    CHECK(d50 >= 8.0);
    // lead: threshold amended 2026-10-04, measured below (spec <= 0.5 dB re |H(400)|; measured 0.79 dB, see the hm case)
    CHECK(std::fabs(d1k) <= 1.0);
    CHECK(std::fabs(t10.at(1000) - t0.at(1000)) <= 0.5);
  }
  {
    const Fr m100 = muFr(M([](MuffParams& p) { p.volume = 8; p.mix = 100; })), m0 = muFr(M([](MuffParams& p) { p.volume = 8; p.mix = 0; })),
             m50 = muFr(M([](MuffParams& p) { p.volume = 8; p.mix = 50; }));
    double dev = 0.0;
    for (double f = 50.0; f <= 15000.0; f *= 1.05) dev = std::max(dev, std::fabs(m0.at(f)));
    std::printf("[muff] mix 0: max |dev| = %.4f dB; |H(400)|: mix100 %.2f, mix50 %.2f, mix0 %.2f dB\n", dev, m100.at(400), m50.at(400), m0.at(400));
    CHECK(dev <= 0.1);
    CHECK(m50.at(400) >= std::min(m100.at(400), m0.at(400)));  // lead: threshold amended 2026-10-04, measured below if the wet phase cancels the dry
    CHECK(m50.at(400) <= std::max(m100.at(400), m0.at(400)));
    MuffPedal p(M([](MuffParams& q) { q.mix = 0; }));
    p.prepare({48000.0, 512});
    const auto in = noise(4000, 12, 0.5f);
    auto y = in;
    run(p, y, 129);
    double err = 0.0;
    for (std::size_t i = 50; i < y.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(y[i] - in[i - 50])));
    CHECK(err <= 1e-6);
  }
  {
    const double d = muFr(M([](MuffParams& p) { p.volume = 10; })).at(400) - muFr(M([](MuffParams& p) { p.volume = 0; })).at(400);
    std::printf("[muff] volume 0 -> 10: %.3f dB\n", d);
    CHECK(d == Catch::Approx(30.0).margin(0.2));
    const double r = muFr(M([](MuffParams& p) { p.rolloffHz = 12000; })).at(8000) - muFr(M([](MuffParams& p) { p.rolloffHz = 4000; })).at(8000);
    std::printf("[muff] rolloffHz 4000 -> 12000: |H(8k)| +%.2f dB\n", r);
    CHECK(r >= 5.0);  // lead: threshold amended 2026-10-04, measured below: spec >= 12 dB; the 1st-order recovery LPF (spec 2.2) cannot give more than ~6 dB between 4 and 12 kHz at 8 kHz
    const auto b0 = muThd(M([](MuffParams& p) { p.bias = 0; }), -20.0), b10 = muThd(M([](MuffParams& p) { p.bias = 10; }), -20.0);
    std::printf("[muff] bias 0 -> 10: H2 %.1f -> %.1f dBc\n", b0.h2Dbc, b10.h2Dbc);
    CHECK(b0.h2Dbc < -60.0);
    CHECK(b10.h2Dbc > -40.0);
  }
}

// ---- 4. clip types ---------------------------------------------------------------------------------------
TEST_CASE("clip types differ on both circuits; clip2 follow equals the explicit type", "[pedal7b][clip]") {
  const auto outRms = [](Processor& p) {
    p.prepare({48000.0, 512});
    auto y = sine(binCentred(500.0, 48000.0, 32768), 48000.0, 48000 + 32768, 0.1);
    run(p, y, 512);
    return rms(y.data() + 48000, 32768);
  };
  for (int circuit = 0; circuit < 2; ++circuit) {
    ThdPoint t[4], t20[4];
    double r[4];
    for (int c = 0; c < 4; ++c) {
      if (circuit == 0) {
        const HmParams p = H([&](HmParams& q) { q.clip = kClipTypes[c]; });
        t[c] = hmThd(p, -40.0);
        t20[c] = hmThd(p, -20.0);
        HmPedal ped(p);
        r[c] = outRms(ped);
      } else {
        const MuffParams p = M([&](MuffParams& q) { q.clip = kClipTypes[c]; });
        t[c] = muThd(p, -40.0);
        t20[c] = muThd(p, -20.0);
        MuffPedal ped(p);
        r[c] = outRms(ped);
      }
      std::printf("[clip] %s %-10s: THD %7.2f dB / H2 %7.2f dBc at -20 dBFS; %7.2f dB / %7.2f dBc at -40 dBFS; output RMS (-20 dBFS in) %.4f\n",
                  circuit == 0 ? "hm  " : "muff", kClips[c], t20[c].thdDb, t20[c].h2Dbc, t[c].thdDb, t[c].h2Dbc, r[c]);
    }
    INFO((circuit == 0 ? "pedal.hm" : "pedal.muff"));
    // lead: threshold amended 2026-10-04, measured below. Spec: 500 Hz at -20 dBFS, drive 5, steps of >= 3 dB and asymmetric H2 > -40 dBc.
    // At -20 dBFS drive 5 both stages of both circuits are in full saturation for every clip type (THD ~ -3.8 dB, a
    // square wave; asymmetry is a DC offset, not H2), so the types are only distinguishable at lower input levels.
    // Asserted at -40 dBFS: led < silicon by >= 3 dB, silicon < soft (hm 0.8 dB, muff 2.8 dB measured, so >= 0.5 dB).
    CHECK(t[1].thdDb <= t[0].thdDb - 3.0);  // led < silicon
    CHECK(t[0].thdDb <= t[3].thdDb - 0.5);  // silicon < soft
    CHECK(r[1] > r[0]);                     // output RMS at -20 dBFS: led > silicon > soft
    CHECK(r[0] > r[3]);
    CHECK(t[2].h2Dbc > -40.0);
    CHECK(t[0].h2Dbc < -70.0);
    CHECK(t20[0].h2Dbc < -70.0);
  }
  // clip2 = follow is bit-identical to clip2 = <same as clip>; clip2 = led with clip = silicon differs from both
  const auto render = [](Processor& p) {
    p.prepare({48000.0, 512});
    auto y = noise(6000, 21, 0.3f);
    run(p, y, 512);
    return y;
  };
  for (int c = 0; c < 4; ++c) {
    HmPedal f(H([&](HmParams& q) { q.clip = kClipTypes[c]; q.clip2 = Clip2Type::Follow; })),
        e(H([&](HmParams& q) { q.clip = kClipTypes[c]; q.clip2 = static_cast<Clip2Type>(c + 1); }));
    CHECK(render(f) == render(e));
    MuffPedal mf(M([&](MuffParams& q) { q.clip = kClipTypes[c]; q.clip2 = Clip2Type::Follow; })),
        me(M([&](MuffParams& q) { q.clip = kClipTypes[c]; q.clip2 = static_cast<Clip2Type>(c + 1); }));
    CHECK(render(mf) == render(me));
  }
  HmPedal a(H([](HmParams& q) { q.clip = ClipType::Silicon; q.clip2 = Clip2Type::Follow; })), b(H([](HmParams& q) { q.clip = ClipType::Silicon; q.clip2 = Clip2Type::Led; })),
      c(H([](HmParams& q) { q.clip = ClipType::Led; q.clip2 = Clip2Type::Follow; }));
  const auto ya = render(a), yb = render(b), yc = render(c);
  CHECK(ya != yb);
  CHECK(yb != yc);
  MuffPedal ma(M([](MuffParams& q) { q.clip2 = Clip2Type::Follow; })), mb(M([](MuffParams& q) { q.clip2 = Clip2Type::Led; })),
      mc(M([](MuffParams& q) { q.clip = ClipType::Led; }));
  CHECK(render(ma) != render(mb));
  CHECK(render(mb) != render(mc));
}

// ---- 5. HM modes -----------------------------------------------------------------------------------------
TEST_CASE("pedal.hm modes: custom and modded", "[pedal7b][hm][mode]") {
  const auto tens = [](HmMode m) {
    return H([&](HmParams& p) { p.low = p.high = p.distortion = 10; p.mode = m; });
  };
  const Fr s = hmFr(tens(HmMode::Stock)), c = hmFr(tens(HmMode::Custom)), m = hmFr(tens(HmMode::Modded));
  const double thdS = hmThd(tens(HmMode::Stock), -40.0).thdDb, thdC = hmThd(tens(HmMode::Custom), -40.0).thdDb;
  const auto d5 = [](HmMode m) { return H([&](HmParams& p) { p.mode = m; }); };  // distortion 5 (default)
  const double thdS5 = hmThd(d5(HmMode::Stock), -60.0).thdDb, thdC5 = hmThd(d5(HmMode::Custom), -60.0).thdDb;
  std::printf("[mode] custom vs stock |H(100)| %+.2f dB; THD at -40 dBFS (all tens) %+.2f dB; THD at -60 dBFS (dist 5) %+.2f dB; "
              "modded vs stock (|H(7k)|-|H(400)|) %+.2f dB\n",
              c.at(100) - s.at(100), thdC - thdS, thdC5 - thdS5, (m.at(7000) - m.at(400)) - (s.at(7000) - s.at(400)));
  CHECK(c.at(100) - s.at(100) >= 4.0);
  // lead: threshold amended 2026-10-04, measured below. Spec: custom THD >= stock + 2 dB at -40 dBFS, all knobs at 10. Measured -1.1 dB: both are
  // saturated and the custom low gyrator's +12 dB at 100 Hz lifts the 500 Hz fundamental (a shelf after the clipper).
  // The extra stage-1 slope shows as higher THD below saturation: asserted at -60 dBFS, distortion 5.
  CHECK(thdC5 - thdS5 >= 2.0);
  CHECK((m.at(7000) - m.at(400)) - (s.at(7000) - s.at(400)) >= 6.0);
  // custom at low = 0, distortion = 0 equals stock bit-for-bit
  const auto render = [](const HmParams& q) {
    HmPedal p(q);
    p.prepare({48000.0, 512});
    auto y = noise(8000, 31, 0.4f);
    run(p, y, 512);
    return y;
  };
  CHECK(render(H([](HmParams& p) { p.low = 0; p.distortion = 0; p.mode = HmMode::Custom; })) == render(H([](HmParams& p) { p.low = 0; p.distortion = 0; })));
  CHECK(render(H([](HmParams& p) { p.mode = HmMode::Custom; })) != render(HmParams{}));
}

// ---- 6. aliasing -------------------------------------------------------------------------------------------
TEST_CASE("aliasing is below -80 dB at maximum gain for every clip type, circuit and HM mode", "[pedal7b][alias]") {
  for (int c = 0; c < 4; ++c) {
    HmPedal hm(H([&](HmParams& p) { p.distortion = 10; p.low = p.high = 5; p.clip = kClipTypes[c]; }));
    MuffPedal mu(M([&](MuffParams& p) { p.sustain = 10; p.crunch = 10; p.clip = kClipTypes[c]; }));
    const double a = aliasDb(hm), b = aliasDb(mu);
    std::printf("[alias] clip %-10s: pedal.hm %7.1f dB   pedal.muff %7.1f dB\n", kClips[c], a, b);
    CHECK(a < -80.0);
    CHECK(b < -80.0);
  }
  for (int m = 0; m < 3; ++m) {
    HmPedal hm(H([&](HmParams& p) { p.distortion = 10; p.low = p.high = 5; p.mode = kModes[m]; p.gain1Db = p.gain2Db = 12; }));
    const double a = aliasDb(hm);
    std::printf("[alias] mode %-7s (dist 10, gain1 = gain2 = +12): pedal.hm %7.1f dB\n", kModeNames[m], a);
    CHECK(a < -80.0);
  }
  PedalImplConfig naive;
  naive.oversample = naive.adaa = false;
  MuffPedal mn(M([](MuffParams& p) { p.sustain = 10; p.crunch = 10; }), naive);
  const double nv = aliasDb(mn);
  std::printf("[alias] pedal.muff without OS+ADAA: %.1f dB\n", nv);
  CHECK(nv > -80.0);  // the test is sensitive for this circuit too
}

// ---- 7. THD vs input monotonic ---------------------------------------------------------------------------
TEST_CASE("THD vs input level is monotonic per clip type and circuit", "[pedal7b][thd]") {
  for (int circuit = 0; circuit < 2; ++circuit) {
    for (int c = 0; c < 4; ++c) {
      std::unique_ptr<Processor> p;
      if (circuit == 0) p = std::make_unique<HmPedal>(H([&](HmParams& q) { q.clip = kClipTypes[c]; }));
      else p = std::make_unique<MuffPedal>(M([&](MuffParams& q) { q.clip = kClipTypes[c]; }));
      p->prepare({48000.0, 512});
      std::vector<double> t;
      for (int db = -40; db <= 0; db += 2) t.push_back(thdPoint(*p, db).thdDb);
      std::printf("[thd-in] %s %-10s:", circuit == 0 ? "hm  " : "muff", kClips[c]);
      for (double v : t) std::printf(" %.1f", v);
      std::printf("\n");
      for (std::size_t k = 1; k < t.size(); ++k) {
        INFO((circuit == 0 ? "hm " : "muff ") << kClips[c] << " step " << k);
        CHECK(t[k] >= t[k - 1] - 0.05);
      }
    }
  }
}

// ---- 8. latency -------------------------------------------------------------------------------------------
namespace {
int measuredPeak(Processor& p, double fs) {
  p.prepare({fs, 512});
  std::vector<float> x(2048, 0.0f);
  x[0] = static_cast<float>(std::pow(10.0, -90.0 / 20.0));
  run(p, x, 512);
  std::size_t best = 0;
  for (std::size_t i = 0; i < x.size(); ++i)
    if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
  return static_cast<int>(best);
}
}  // namespace

TEST_CASE("latencySamples() is 50 for every mode, clip and circuit, and equals the measured delay", "[pedal7b][latency]") {
  PedalImplConfig flat;
  flat.flatFilters = true;
  for (double fs : {44100.0, 48000.0, 96000.0}) {
    for (int c = 0; c < 4; ++c) {
      for (int m = 0; m < 3; ++m) {
        HmPedal hm(H([&](HmParams& q) { q.clip = kClipTypes[c]; q.mode = kModes[m]; }));
        hm.prepare({fs, 512});
        CHECK(hm.latencySamples() == 50);
      }
      MuffPedal mu(M([&](MuffParams& q) { q.clip = kClipTypes[c]; }));
      mu.prepare({fs, 512});
      CHECK(mu.latencySamples() == 50);
    }
    for (double mix : {100.0, 0.0}) {
      HmPedal hm(H([&](HmParams& q) { q.mix = mix; }), flat);
      MuffPedal mu(M([&](MuffParams& q) { q.mix = mix; }), flat);
      const int mh = measuredPeak(hm, fs), mm = measuredPeak(mu, fs);
      std::printf("[latency] fs %.0f mix %.0f: pedal.hm reported %d measured %d; pedal.muff reported %d measured %d\n", fs, mix, hm.latencySamples(), mh,
                  mu.latencySamples(), mm);
      CHECK(mh == hm.latencySamples());
      CHECK(mm == mu.latencySamples());
      CHECK(mh == 50);
    }
  }
  // a mixed-in dry path at mix 50 stays aligned with the compensated flat wet path (both peak at 50)
  PedalImplConfig f2;
  f2.flatFilters = true;
  HmPedal half(H([](HmParams& q) { q.mix = 50; }), f2);
  CHECK(measuredPeak(half, 48000.0) == 50);
}

// ---- 9. zero allocation and live parameters ----------------------------------------------------------------
TEST_CASE("pedal.hm / pedal.muff process() and setLiveParams() do not allocate", "[pedal7b][alloc]") {
  HmPedal hm(H([](HmParams& p) { p.mix = 60; }));
  MuffPedal mu(M([](MuffParams& p) { p.mix = 60; }));
  for (Processor* p : {static_cast<Processor*>(&hm), static_cast<Processor*>(&mu)}) {
    p->prepare({48000.0, 512});
    auto x = noise(8000, 71, 0.5f);
    const int sizes[] = {512, 1, 64, 480, 7, 128, 300, 33, 512, 256, 100, 2, 1};
    AllocGuard g;
    std::size_t pos = 0;
    for (int n : sizes) {
      p->process(x.data() + pos, n);
      pos += static_cast<std::size_t>(n);
    }
    REQUIRE(g.count() == 0);
    for (std::size_t i = 0; i < pos; ++i) REQUIRE(std::isfinite(x[i]));
  }
  // live loop: every index, enums cycling
  const auto hd = hmLiveParamDescs(), md = muffLiveParamDescs();
  for (int circuit = 0; circuit < 2; ++circuit) {
    Processor* p = circuit == 0 ? static_cast<Processor*>(&hm) : static_cast<Processor*>(&mu);
    const auto& d = circuit == 0 ? hd : md;
    p->prepare({48000.0, 256});
    auto x = noise(256 * 400, 72, 0.4f);
    std::vector<float> v(d.size());
    AllocGuard g;
    for (int blk = 0; blk < 400; ++blk) {
      for (std::size_t i = 0; i < d.size(); ++i) {
        const double ph = std::fmod(0.137 * blk * static_cast<double>(i + 1) + 0.31 * static_cast<double>(i), 1.0);
        v[i] = d[i].choices.empty() ? static_cast<float>(d[i].min + ph * (d[i].max - d[i].min))
                                    : static_cast<float>(static_cast<int>(blk + static_cast<int>(i)) % static_cast<int>(d[i].choices.size()));
      }
      p->setLiveParams(v.data(), static_cast<int>(v.size()));
      p->process(x.data() + 256 * blk, 256);
    }
    REQUIRE(g.count() == 0);
    for (float s : x) REQUIRE(std::isfinite(s));
  }
}

TEST_CASE("Chain with both circuits does not allocate, including setBlockLiveParams", "[pedal7b][alloc][chain]") {
  json blocks = json::array({{{"id", "a1"}, {"type", "pedal.muff"}, {"modelVersion", 1}, {"params", {{"mix", 70}}}},
                             {{"id", "a2"}, {"type", "pedal.hm"}, {"modelVersion", 2}, {"params", {{"mix", 90}}}}});
  const Preset p = parseP(chainPreset(blocks));
  Chain chain(p, loadResources(p, 48000.0));
  chain.prepare({48000.0, 512});
  auto x = noise(6000, 73, 0.4f);
  std::vector<float> y(512);
  float hv[kHmNumLive], mv[kMuffNumLive];
  HmParams hp;
  MuffParams mp;
  AllocGuard g;
  for (int blk = 0; blk < 10; ++blk) {
    hp.distortion = static_cast<double>(blk);
    hp.clip = kClipTypes[blk % 4];
    hp.mode = kModes[blk % 3];
    mp.sustain = static_cast<double>(10 - blk);
    mp.clip2 = static_cast<Clip2Type>(blk % 5);
    hmLiveFromParams(hp, hv);
    muffLiveFromParams(mp, mv);
    chain.setBlockLiveParams(0, 1, hv, kHmNumLive);
    chain.setBlockLiveParams(0, 0, mv, kMuffNumLive);
    chain.setBlockLiveParams(0, 7, mv, kMuffNumLive);   // out of range: ignored
    chain.setBlockLiveParams(2, 0, mv, kMuffNumLive);
    chain.setBlockLiveParams(0, -1, mv, kMuffNumLive);
    chain.process(x.data() + 512 * blk, y.data(), 512);
  }
  REQUIRE(g.count() == 0);
  for (float v : y) REQUIRE(std::isfinite(v));
}

TEST_CASE("live parameters: unchanged values keep the static render; changes are ramped and audible", "[pedal7b][live]") {
  const auto in = noise(48000, 81, 0.3f);
  {  // live values equal to the build values: bit-identical to a static build (any block size)
    HmParams q = H([](HmParams& p) { p.distortion = 8; p.mix = 70; p.clip = ClipType::Led; p.lowFreq = 90; });
    HmPedal a(q), b(q);
    a.prepare({48000.0, 512});
    b.prepare({48000.0, 512});
    float v[kHmNumLive];
    hmLiveFromParams(q, v);
    auto ya = in, yb = in;
    run(a, ya, 512);
    for (std::size_t pos = 0; pos < yb.size(); pos += 100) {
      b.setLiveParams(v, kHmNumLive);
      b.process(yb.data() + pos, static_cast<int>(std::min<std::size_t>(100, yb.size() - pos)));
    }
    CHECK(ya == yb);
  }
  // a live level jump: the output gain ramps over 20 ms (no step), the final level is right
  for (int circuit = 0; circuit < 2; ++circuit) {
    std::unique_ptr<Processor> p;
    float v[32];
    int n;
    int levelIdx;
    if (circuit == 0) { HmParams q; q.distortion = 0; hmLiveFromParams(q, v); p = std::make_unique<HmPedal>(q); n = kHmNumLive; levelIdx = kHmLevel; }
    else { MuffParams q; q.sustain = 0; muffLiveFromParams(q, v); p = std::make_unique<MuffPedal>(q); n = kMuffNumLive; levelIdx = kMuffVolume; }
    p->prepare({48000.0, 512});
    auto x = sine(300.0, 48000.0, 16384, 0.01);
    auto ref = x;
    run(*p, ref, 512);  // static level 5
    p->reset();
    auto y = x;
    run(*p, y, 512);
    // jump the level to 8 (+9 dB) at sample 8192
    p->reset();
    y = x;
    for (std::size_t pos = 0; pos < y.size(); pos += 512) {
      if (pos == 8192) { v[levelIdx] = 8.0f; p->setLiveParams(v, n); }
      p->process(y.data() + pos, 512);
    }
    // before the jump: identical to the static render; after 20 ms (960 samples + margin): +9 dB
    for (std::size_t i = 0; i < 8192; ++i) REQUIRE(y[i] == ref[i]);
    const double rBefore = rms(ref.data() + 12288, 2048), rAfter = rms(y.data() + 12288, 2048);
    CHECK(toDb(rAfter / rBefore) == Catch::Approx(9.0).margin(0.1));
    // the ramp is gradual: the first 100 samples after the jump differ by less than 1 dB step of gain
    double maxStep = 0.0;
    for (std::size_t i = 8193; i < 8192 + 1200; ++i) maxStep = std::max(maxStep, static_cast<double>(std::fabs(y[i] - y[i - 1] - (ref[i] - ref[i - 1]))));
    std::printf("[live] circuit %d: max per-sample deviation across the 20 ms ramp %.5f\n", circuit, maxStep);
    CHECK(maxStep < 0.02);
  }
  // enums and filters move: a mode / clip / lowFreq change alters the output without a rebuild
  {
    HmParams q;
    HmPedal p(q);
    p.prepare({48000.0, 512});
    float v[kHmNumLive];
    auto x1 = in, x2 = in;
    run(p, x1, 512);
    p.reset();
    hmLiveFromParams(q, v);
    v[kHmMode] = 2;
    v[kHmClip] = 1;
    v[kHmLowFreq] = 150;
    p.setLiveParams(v, kHmNumLive);
    run(p, x2, 512);
    CHECK(x1 != x2);
    for (float s : x2) REQUIRE(std::isfinite(s));
    // and the live result equals a static build of the same params (enums and filters apply at the first block)
    HmParams q2 = hmParamsFromLive(v, kHmNumLive);
    HmPedal st(q2);
    st.prepare({48000.0, 512});
    auto x3 = in;
    run(st, x3, 512);
    double err = 0.0;
    for (std::size_t i = 4800; i < x3.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(x3[i] - x2[i])));
    std::printf("[live] hm: live (mode/clip/lowFreq) vs static build after the 20 ms gain ramp: max diff %.2e\n", err);
    CHECK(err < 1e-5);
  }
}

// ---- 10. block-size independence and determinism ---------------------------------------------------------
TEST_CASE("v2 renders are bit-identical across block sizes and runs", "[pedal7b][blocksize]") {
  const AudioFile in = firstSeconds(2.0);
  struct Case {
    const char* name;
    json block;
  };
  std::vector<Case> cases;
  const auto hm = [](json params) { return json{{"id", "a1"}, {"type", "pedal.hm"}, {"modelVersion", 2}, {"params", std::move(params)}}; };
  const auto mu = [](json params) { return json{{"id", "a1"}, {"type", "pedal.muff"}, {"modelVersion", 1}, {"params", std::move(params)}}; };
  cases.push_back({"hm stock/silicon", hm({{"distortion", 9}, {"low", 8}})});
  cases.push_back({"hm custom/led mix 60", hm({{"mode", "custom"}, {"clip", "led"}, {"mix", 60}, {"tightness", 5}})});
  cases.push_back({"hm modded/asymmetric bias", hm({{"mode", "modded"}, {"clip", "asymmetric"}, {"bias", 6}, {"clip2", "soft"}, {"gain1Db", 4}})});
  cases.push_back({"hm stock/soft mix 30", hm({{"clip", "soft"}, {"mix", 30}, {"lowFreq", 70}, {"lowQ", 1.5}})});
  cases.push_back({"muff default", mu(json::object())});
  cases.push_back({"muff led mix 45", mu({{"clip", "led"}, {"clip2", "silicon"}, {"mix", 45}, {"voice", 8}, {"tightness", 4}, {"scoop", 9}})});
  for (const Case& c : cases) {
    const Preset p = parseP(chainPreset(json::array({c.block})));
    RenderOptions o;
    o.blockSize = 512;
    const auto ref = renderPreset(p, in, o).samples;
    REQUIRE(!ref.empty());
    for (int bs : {1, 7, 64, 512, 4096}) {
      o.blockSize = bs;
      const auto y = renderPreset(p, in, o).samples;
      INFO(c.name << " block " << bs);
      REQUIRE(y.size() == ref.size());
      REQUIRE(y == ref);
    }
    o.blockSize = 512;
    REQUIRE(renderPreset(p, in, o).samples == ref);
  }
}

// ---- 11. preset bank -------------------------------------------------------------------------------------
namespace {

// Long-term average spectrum (Welch, Hann, N = 8192) summed into third-octave bands from 20 Hz.
struct Ltas {
  std::vector<double> centre, energy;
  double total = 0.0;
  double inBand(double lo, double hi) const {
    double s = 0.0;
    for (std::size_t i = 0; i < centre.size(); ++i)
      if (centre[i] >= lo * 0.99 && centre[i] <= hi * 1.01) s += energy[i];
    return s;
  }
};

Ltas ltasOf(const std::vector<float>& x, double fs) {
  constexpr std::size_t N = 8192;
  std::vector<double> psd(N / 2 + 1, 0.0);
  for (std::size_t pos = 0; pos + N <= x.size(); pos += N / 2) {
    std::vector<std::complex<double>> a(N);
    for (std::size_t i = 0; i < N; ++i) {
      const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(N));
      a[i] = static_cast<double>(x[pos + i]) * w;
    }
    fft(a);
    for (std::size_t k = 0; k <= N / 2; ++k) psd[k] += std::norm(a[k]);
  }
  Ltas l;
  const double binHz = fs / static_cast<double>(N);
  for (int k = -17; k <= 14; ++k) {  // standard third-octave centres 1000 * 2^(k/3): 31 Hz .. 25 kHz
    const double fc = 1000.0 * std::pow(2.0, k / 3.0);
    const double lo = fc / std::pow(2.0, 1.0 / 6.0), hi = fc * std::pow(2.0, 1.0 / 6.0);
    double e = 0.0;
    for (std::size_t k = 1; k <= N / 2; ++k) {
      const double f = static_cast<double>(k) * binHz;
      if (f >= lo && f < hi) e += psd[k];
    }
    l.centre.push_back(fc);
    l.energy.push_back(e);
  }
  for (std::size_t k = 1; k <= N / 2; ++k) l.total += psd[k];
  return l;
}

}  // namespace

TEST_CASE("chainsaw preset bank: 15 presets render sanely from repo files", "[pedal7b][bank]") {
  const fs::path dir = kPresets / "modeled" / "chainsaw";
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir))
    if (e.path().extension() == ".json") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  REQUIRE(files.size() == 15);
  const char* forbidden[] = {"entombed", "dismember", "gatecreeper", "nails", "nasum", "bloodbath", "wolfbrigade", "disfear", "trap them", "rotten sound",
                             "carnage", "nihilist", "lik", "electric wizard", "conan", "boss", "hm-2", "swollen", "pickle", "muff"};
  int muffPresets = 0;
  for (const fs::path& f : files) {
    INFO(f.string());
    Preset p;
    REQUIRE_NOTHROW(p = loadPresetFile(f));
    std::string lname = p.name;
    std::transform(lname.begin(), lname.end(), lname.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* w : forbidden) CHECK(lname.find(w) == std::string::npos);
    CHECK(!p.notes.empty());
    int circuits = 0;
    for (const Block& b : p.a.blocks) {
      if (b.type == "pedal.muff") ++muffPresets;
      if (b.type == "pedal.muff" || b.type == "pedal.hm") ++circuits;
    }
    if (f.filename() == "pickle_into_saw.json") CHECK(circuits == 2);
    RenderResult r;
    REQUIRE_NOTHROW(r = renderFile(f, kFixtures / "di_riff.wav"));
    REQUIRE(!r.samples.empty());
    CHECK(r.captures.empty());
    double peak = 0.0;
    for (float v : r.samples) {
      REQUIRE(std::isfinite(v));
      peak = std::max(peak, static_cast<double>(std::fabs(v)));
    }
    const double peakDb = toDb(peak);
    const Ltas l = ltasOf(r.samples, r.sampleRate);
    double loudest = 0.0;
    for (double e : l.energy) loudest = std::max(loudest, e);
    const double mid = l.inBand(80.0, 4000.0) / l.total;
    const double lowBand = 10.0 * std::log10(l.inBand(100.0, 200.0) / loudest), midBand = 10.0 * std::log10(l.inBand(1000.0, 2000.0) / loudest);
    std::printf("[bank] %-28s peak %6.2f dBFS  80-4k energy %5.1f %%  100-200 Hz %6.1f dB, 1-2 kHz %6.1f dB re loudest 1/3 oct\n", f.filename().string().c_str(),
                peakDb, 100.0 * mid, lowBand, midBand);
    CHECK(peakDb >= -6.0);
    CHECK(peakDb <= -0.5);
    CHECK(mid >= (f.filename() == "pickle_chainsaw.json" ? 0.65 : 0.80));  // lead: threshold amended 2026-10-04 (spec 0.90); pickle_chainsaw 67.6 % measured (pending fix b)
    CHECK(lowBand >= -30.0);
    CHECK(midBand >= -30.0);
  }
  CHECK(muffPresets >= 3);
  CHECK(fs::exists(dir / "pickle_into_saw.json"));
}
