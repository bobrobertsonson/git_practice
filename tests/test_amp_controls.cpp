// v0.2 Task A: per-path amp controls (GAIN / BASS / MID / TREBLE / PRESENCE / LEVEL): the AmpStage, its
// place in the Chain, and the preset schema v2 (docs/PRESET_SCHEMA.md "Amp controls").
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <vector>

#include "alloc_guard.h"
#include "latency_stub.h"
#include "sawblade/amp_controls.h"
#include "sawblade/block_registry.h"
#include "sawblade/chain.h"
#include "sawblade/render.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;
const fs::path kPresets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

// Steady-state gain (dB) of the stage's tone stack + LEVEL at `freq`: a sine through processPost,
// RMS of the second half over RMS of the input.
double stageGainDb(const AmpKnobs& k, double freq, bool pre = false) {
  AmpStage s;
  s.prepare({kFs, 4096});
  s.setKnobsNow(k);
  auto x = sine(freq, kFs, 48000, 0.1);
  auto y = x;
  for (std::size_t pos = 0; pos < y.size(); pos += 480) {
    if (pre) s.processPre(y.data() + pos, 480);
    else s.processPost(y.data() + pos, 480);
  }
  return toDb(rms(y.data() + 24000, 24000) / rms(x.data() + 24000, 24000));
}

AmpKnobs knobs(double gain = 5, double bass = 5, double mid = 5, double treble = 5, double presence = 5, double level = 5) {
  AmpKnobs k;
  k.gain = gain; k.bass = bass; k.mid = mid; k.treble = treble; k.presence = presence; k.level = level;
  return k;
}

json identityBlock(const std::string& id, const char* slot = nullptr) {
  json b = {{"id", id}, {"type", "nam"}, {"model", {{"file", "../nam/linear_identity.nam"}}}};
  if (slot) b["slot"] = slot;
  return b;
}

// Two identity paths (blend 0.5 of two equal paths is the identity), a flat post EQ, impulse cab.
json mk(const json& ampA = nullptr, const json& ampB = nullptr) {
  json a = {{"blocks", json::array({identityBlock("a1")})}}, b = {{"blocks", json::array({identityBlock("b1")})}};
  if (!ampA.is_null()) a["ampControls"] = ampA;
  if (!ampB.is_null()) b["ampControls"] = ampB;
  return {{"schema", "sawblade.preset"}, {"version", 2}, {"name", "amp"},
          {"paths", {{"a", a}, {"b", b}}},
          {"align", {{"mode", "off"}}},
          {"blend", 0.5},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}}};
}

std::unique_ptr<Chain> build(const json& j, int maxBlock = 512) {
  const Preset p = parsePreset(j, kPresets);
  auto c = std::make_unique<Chain>(p, loadResources(p, kFs));
  c->prepare({kFs, maxBlock});
  return c;
}

std::vector<float> run(Chain& c, const std::vector<float>& x, int block) {
  std::vector<float> y(x.size());
  for (std::size_t pos = 0; pos < x.size();) {
    const auto n = std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos);
    c.process(x.data() + pos, y.data() + pos, static_cast<int>(n));
    pos += n;
  }
  return y;
}

}  // namespace

// ---- mapping and responses -------------------------------------------------------------------------
TEST_CASE("Amp controls: knob to dB mapping", "[amp]") {
  CHECK(ampKnobDb(5.0) == 0.0);
  CHECK(ampKnobDb(0.0) == Catch::Approx(-12.0));
  CHECK(ampKnobDb(10.0) == Catch::Approx(12.0));
  CHECK(ampKnobDb(7.5) == Catch::Approx(6.0));
  CHECK(ampPresenceDb(5.0) == 0.0);
  CHECK(ampPresenceDb(0.0) == Catch::Approx(-9.0));
  CHECK(ampPresenceDb(10.0) == Catch::Approx(9.0));
  CHECK(AmpKnobs{}.isDefault());
  CHECK_FALSE(knobs(5, 5, 5, 5, 5, 5.001).isDefault());
}

TEST_CASE("Amp controls: GAIN and LEVEL map 0 / 5 / 10 to -12 / 0 / +12 dB", "[amp]") {
  for (const double k : {0.0, 2.5, 5.0, 7.5, 10.0}) {
    CAPTURE(k);
    const double want = (k - 5.0) * 2.4;
    CHECK(stageGainDb(knobs(k), 1000.0, true) == Catch::Approx(want).margin(0.01));
    CHECK(stageGainDb(knobs(5, 5, 5, 5, 5, k), 1000.0) == Catch::Approx(want).margin(0.01));
  }
}

TEST_CASE("Amp controls: tone stack responses at the control frequencies, max and min", "[amp][tone]") {
  // MID: a peak, so its gain at 650 Hz is the full +-12 dB.
  CHECK(stageGainDb(knobs(5, 5, 10), 650.0) == Catch::Approx(12.0).margin(0.1));
  CHECK(stageGainDb(knobs(5, 5, 0), 650.0) == Catch::Approx(-12.0).margin(0.1));
  // The shelves: at their corner frequency a shelf is exactly half its gain (BASS +-6 dB at 100 Hz,
  // TREBLE +-6 dB at 3 kHz, PRESENCE +-4.5 dB at 5.5 kHz) and it reaches the full gain on its plateau.
  CHECK(stageGainDb(knobs(5, 10), 100.0) == Catch::Approx(6.0).margin(0.1));
  CHECK(stageGainDb(knobs(5, 0), 100.0) == Catch::Approx(-6.0).margin(0.1));
  CHECK(stageGainDb(knobs(5, 5, 5, 10), 3000.0) == Catch::Approx(6.0).margin(0.1));
  CHECK(stageGainDb(knobs(5, 5, 5, 0), 3000.0) == Catch::Approx(-6.0).margin(0.1));
  CHECK(stageGainDb(knobs(5, 5, 5, 5, 10), 5500.0) == Catch::Approx(4.5).margin(0.1));
  CHECK(stageGainDb(knobs(5, 5, 5, 5, 0), 5500.0) == Catch::Approx(-4.5).margin(0.1));
  // Plateaus (analytic response of the designed biquads): BASS to DC, the treble shelves to Nyquist.
  const auto db = [](int band, double g, double f) {
    return biquadMagnitudeDb(designBiquad(ampToneBand(band, g), kFs), f, kFs);
  };
  CHECK(db(0, 12.0, 1.0) == Catch::Approx(12.0).margin(0.1));
  CHECK(db(0, -12.0, 1.0) == Catch::Approx(-12.0).margin(0.1));
  CHECK(db(2, 12.0, 23900.0) == Catch::Approx(12.0).margin(0.1));
  CHECK(db(2, -12.0, 23900.0) == Catch::Approx(-12.0).margin(0.1));
  CHECK(db(3, 9.0, 23900.0) == Catch::Approx(9.0).margin(0.1));
  CHECK(db(3, -9.0, 23900.0) == Catch::Approx(-9.0).margin(0.1));
  // Each shelf is flat on the far side of its corner (BASS leaves 3 kHz+ alone, TREBLE leaves 100 Hz alone).
  CHECK(stageGainDb(knobs(5, 10), 5000.0) == Catch::Approx(0.0).margin(0.2));
  CHECK(stageGainDb(knobs(5, 5, 5, 10), 100.0) == Catch::Approx(0.0).margin(0.2));
  // The tone knobs at 5 are exactly flat even when LEVEL is moved: the stage runs only what it must.
  CHECK(stageGainDb(knobs(5, 5, 5, 5, 5, 10), 100.0) == Catch::Approx(12.0).margin(0.01));
}

TEST_CASE("Amp controls: the stage runs at 44.1 / 96 kHz and tolerates low rates", "[amp]") {
  for (const double fs : {8000.0, 22050.0, 44100.0, 96000.0, 192000.0}) {
    CAPTURE(fs);
    AmpStage s;
    s.prepare({fs, 512});
    s.setKnobsNow(knobs(10, 10, 10, 10, 10, 10));
    std::vector<float> x(4096, 0.1f);
    s.processPre(x.data(), 4096);
    s.processPost(x.data(), 4096);
    for (float v : x) REQUIRE(std::isfinite(v));
  }
}

// ---- neutral = exact skip -----------------------------------------------------------------------------
TEST_CASE("Amp controls: all knobs at 5 leave audio bit-identical, for any block size", "[amp][neutral]") {
  const auto x = noise(20000, 5);
  for (const int block : {1, 7, 32, 100, 512, 20000}) {
    CAPTURE(block);
    AmpStage s;
    s.prepare({kFs, 512});
    CHECK(s.neutral());
    auto y = x;
    for (std::size_t pos = 0; pos < y.size(); pos += static_cast<std::size_t>(block)) {
      const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), y.size() - pos));
      s.processPre(y.data() + pos, n);
      s.processPost(y.data() + pos, n);
    }
    CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);
  }
}

TEST_CASE("Amp controls: a ramp that returns to 5 makes the stage neutral again", "[amp][neutral]") {
  AmpStage s;
  s.prepare({kFs, 512});
  s.setKnobs(knobs(8, 8, 3, 7, 2, 9), 960);
  CHECK_FALSE(s.neutral());
  auto x = noise(4800, 2);
  s.processPre(x.data(), 4800);
  s.processPost(x.data(), 4800);
  s.setKnobs(knobs(), 960);
  for (int i = 0; i < 20; ++i) {
    auto y = noise(480, 3);
    s.processPre(y.data(), 480);
    s.processPost(y.data(), 480);
  }
  CHECK(s.neutral());
  const auto in = noise(2000, 4);
  auto y = in;
  s.processPre(y.data(), 2000);
  s.processPost(y.data(), 2000);
  CHECK(std::memcmp(in.data(), y.data(), in.size() * sizeof(float)) == 0);
}

TEST_CASE("Amp controls: invalid live knob values are clamped or ignored", "[amp]") {
  AmpStage s;
  s.prepare({kFs, 512});
  AmpKnobs k = knobs(5, 5, 5, 5, 5, 5);
  k.gain = 99.0;
  k.bass = -4.0;
  k.mid = std::nan("");
  s.setKnobs(k, 480);
  CHECK(s.knobs().gain == 10.0);
  CHECK(s.knobs().bass == 0.0);
  CHECK(s.knobs().mid == 5.0);
}

// ---- smoothing / block size -------------------------------------------------------------------------------
TEST_CASE("Amp controls: a knob move is smoothed, with no zipper", "[amp]") {
  AmpStage s;
  s.prepare({kFs, 512});
  auto x = sine(100.0, kFs, 48000, 0.05);
  const auto dry = x;
  const std::size_t moveAt = 9600;
  for (std::size_t pos = 0; pos < x.size(); pos += 64) {
    if (pos == moveAt) s.setKnobs(knobs(10, 8, 5, 5, 5, 7), static_cast<int>(0.02 * kFs));
    s.processPre(x.data() + pos, 64);
    s.processPost(x.data() + pos, 64);
  }
  // Before the move: untouched. After: a 100 Hz sine whose largest possible step is its final amplitude
  // (about 0.05 * 4 * 2.2 * 1.7) times 2 pi f / fs, with the ramp adding only a sliver; a zipper would jump by ~0.3.
  for (std::size_t i = 0; i < moveAt; ++i) REQUIRE(x[i] == dry[i]);
  double maxStep = 0.0, peak = 0.0;
  for (std::size_t i = moveAt; i < x.size(); ++i) {
    maxStep = std::max(maxStep, static_cast<double>(std::fabs(x[i] - x[i - 1])));
    peak = std::max(peak, static_cast<double>(std::fabs(x[i])));
  }
  CHECK(peak > 0.3);  // the move really happened
  CHECK(maxStep < 0.012);
}

TEST_CASE("Amp controls: knobs moving mid-stream are bit-identical for any block size", "[amp]") {
  const auto x = noise(30000, 8);
  std::vector<float> ref;
  for (const int block : {1, 7, 32, 33, 64, 512, 100000}) {
    CAPTURE(block);
    AmpStage s;
    s.prepare({kFs, 512});
    auto y = x;
    // Knob moves at fixed sample positions (3000 and 12345), block boundaries split there.
    const std::size_t moves[] = {3000, 12345, 20000};
    int mv = 0;
    for (std::size_t pos = 0; pos < y.size();) {
      if (mv < 3 && pos == moves[mv]) {
        s.setKnobs(mv == 0 ? knobs(8, 9, 2, 7, 3, 6) : mv == 1 ? knobs(2, 1, 8, 3, 9, 4) : knobs(), 1000);
        ++mv;
      }
      std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(block), y.size() - pos);
      if (mv < 3 && pos < moves[mv] && pos + n > moves[mv]) n = moves[mv] - pos;
      s.processPre(y.data() + pos, static_cast<int>(n));
      s.processPost(y.data() + pos, static_cast<int>(n));
      pos += n;
    }
    if (ref.empty()) ref = y;
    CHECK(y == ref);
  }
}

// ---- in the Chain ---------------------------------------------------------------------------------------------
TEST_CASE("Amp controls: Chain applies GAIN / LEVEL / tone around the amp block", "[amp][chain]") {
  const auto x = sine(1000.0, kFs, 24000, 0.05);
  const json k = {{"gain", 10.0}, {"level", 7.5}};  // +12 dB, +6 dB
  auto c = build(mk(k, k));
  const auto y = run(*c, x, 256);
  const double got = toDb(rms(y.data() + 12000, 12000) / rms(x.data() + 12000, 12000));
  CHECK(got == Catch::Approx(18.0).margin(0.05));

  // One path only: half of the sum is unchanged, half is +18 dB.
  auto c1 = build(mk(k));
  const auto y1 = run(*c1, x, 256);
  const double g1 = toDb(rms(y1.data() + 12000, 12000) / rms(x.data() + 12000, 12000));
  CHECK(g1 == Catch::Approx(toDb(0.5 + 0.5 * std::pow(10.0, 18.0 / 20.0))).margin(0.05));
}

TEST_CASE("Amp controls: the stage sits right after the amp block (slot amp), not after later blocks", "[amp][chain]") {
  // [amp (slot amp), nonlinear-free flat EQ block] vs [flat EQ block, amp]: both identity, both +12 dB: but
  // the knob dB must apply exactly once, to the path with an amp block.
  json j = mk(json{{"level", 10.0}});
  j["paths"]["a"]["blocks"] = json::array({identityBlock("a1", "amp"), {{"id", "e"}, {"type", "eq"}, {"bands", json::array()}}});
  const Preset p = parsePreset(j, kPresets);
  CHECK(ampIndex(p.a) == 0);
  CHECK(ampIndex(p.b) == 0);
  auto c = std::make_unique<Chain>(p, loadResources(p, kFs));
  c->prepare({kFs, 512});
  const auto x = sine(1000.0, kFs, 24000, 0.05);
  const auto y = run(*c, x, 128);
  const double g = toDb(rms(y.data() + 12000, 12000) / rms(x.data() + 12000, 12000));
  CHECK(g == Catch::Approx(toDb(0.5 + 0.5 * std::pow(10.0, 12.0 / 20.0))).margin(0.05));
}

TEST_CASE("Amp controls: ampIndex rule (last slot amp, else last nam, else none)", "[amp]") {
  PathPreset p;
  CHECK(ampIndex(p) == -1);
  const Preset q = parsePreset(mk(), kPresets);
  CHECK(ampIndex(q.a) == 0);
  json j = mk();
  j["paths"]["a"]["blocks"] = json::array({identityBlock("x", "amp"), identityBlock("y"), {{"id", "e"}, {"type", "eq"}, {"bands", json::array()}}});
  CHECK(ampIndex(parsePreset(j, kPresets).a) == 0);  // slot amp wins over a later nam
  j["paths"]["a"]["blocks"] = json::array({identityBlock("x"), identityBlock("y"), {{"id", "e"}, {"type", "eq"}, {"bands", json::array()}}});
  CHECK(ampIndex(parsePreset(j, kPresets).a) == 1);  // else the last nam
  j["paths"]["a"]["blocks"] = json::array({{{"id", "e"}, {"type", "eq"}, {"bands", json::array()}}});
  CHECK(ampIndex(parsePreset(j, kPresets).a) == -1);
}

TEST_CASE("Amp controls: a path with no amp block ignores its knobs (bit-identical)", "[amp][chain]") {
  json base = mk();
  base["paths"]["a"]["blocks"] = json::array({{{"id", "e"}, {"type", "eq"}, {"bands", json::array()}}});
  json knobbed = base;
  knobbed["paths"]["a"]["ampControls"] = {{"gain", 10}, {"bass", 0}, {"level", 9}};
  const auto x = noise(8000, 6);
  auto c0 = build(base), c1 = build(knobbed);
  CHECK(run(*c0, x, 256) == run(*c1, x, 256));
  LiveParams p = c1->liveParams();
  p.amp[0].gain = 0.0;
  c1->setLiveParams(p);
  CHECK(run(*c0, x, 256) == run(*c1, x, 256));
}

TEST_CASE("Amp controls: default knobs leave the chain bit-identical to a preset without them", "[amp][chain][neutral]") {
  const auto x = noise(30000, 12);
  auto none = build(mk());
  auto explicitDefault = build(mk(json{{"gain", 5}, {"bass", 5}, {"mid", 5}, {"treble", 5}, {"presence", 5}, {"level", 5}},
                                  json{{"gain", 5.0}}));
  const auto a = run(*none, x, 100);
  CHECK(run(*explicitDefault, x, 333) == a);
  // And the identity chain really is the identity: nothing ran.
  for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(a[i] == x[i]);
}

TEST_CASE("Amp controls: latency is unchanged by the knobs", "[amp][chain][latency]") {
  registerLatencyStub();
  json j0 = mk();
  json j1 = mk(json{{"gain", 9}, {"bass", 1}, {"mid", 8}, {"treble", 2}, {"presence", 9}, {"level", 3}},
               json{{"gain", 1}, {"level", 8}});
  for (json* j : {&j0, &j1}) {
    (*j)["paths"]["a"]["blocks"].push_back({{"id", "as"}, {"type", "test.latency"}, {"latency", 137}});
    (*j)["paths"]["b"]["blocks"].push_back({{"id", "bs"}, {"type", "test.latency"}, {"latency", 50}});
  }
  auto c0 = build(j0), c1 = build(j1);
  CHECK(c0->latencySamples() == 137);
  CHECK(c1->latencySamples() == c0->latencySamples());
  CHECK(c1->info().pathLatency == c0->info().pathLatency);
  CHECK(c1->info().compensationDelay == c0->info().compensationDelay);
  LiveParams p = c1->liveParams();
  p.amp[0].gain = 10.0;
  c1->setLiveParams(p);
  const auto x = noise(2000, 3);
  (void)run(*c1, x, 64);
  CHECK(c1->latencySamples() == 137);
}

TEST_CASE("Amp controls: traits are declared (latency 0, NAM-trainable) and add no warning", "[amp][traits]") {
  static_assert(kAmpControlsTraits.namTrainable);
  static_assert(kAmpControlsLatencySamples == 0);
  CHECK(kAmpControlsTraits.namTrainable);
  auto c = build(mk(json{{"gain", 9}, {"treble", 8}}, json{{"level", 2}}));
  for (const auto& w : c->info().warnings) CHECK_THAT(w, !ContainsSubstring("NAM-trainable"));
  // Linear and time-invariant at fixed knobs: scaling the input scales the output exactly (no state-dependent
  // nonlinearity) and a delayed input gives the delayed output.
  const auto x = noise(6000, 21, 0.05f);
  auto xs = x;
  for (auto& v : xs) v *= 2.0f;
  auto ca = build(mk(json{{"gain", 9}, {"bass", 8}, {"treble", 2}, {"level", 3}}, json{{"gain", 9}, {"bass", 8}, {"treble", 2}, {"level", 3}}));
  auto cb = build(mk(json{{"gain", 9}, {"bass", 8}, {"treble", 2}, {"level", 3}}, json{{"gain", 9}, {"bass", 8}, {"treble", 2}, {"level", 3}}));
  const auto ya = run(*ca, x, 128), yb = run(*cb, xs, 128);
  for (std::size_t i = 0; i < x.size(); ++i) REQUIRE(yb[i] == Catch::Approx(2.0 * ya[i]).margin(1e-5));
}

TEST_CASE("Amp controls: the export render path (renderPreset) goes through the stage", "[amp][render]") {
  AudioFile in;
  in.sampleRate = kFs;
  in.channels = 1;
  in.interleaved = sine(1000.0, kFs, 24000, 0.05);
  const json k = {{"gain", 10.0}};
  const RenderResult r0 = renderPreset(parsePreset(mk(), kPresets), in);
  const RenderResult r1 = renderPreset(parsePreset(mk(k, k), kPresets), in);
  const double g = toDb(rms(r1.samples.data() + 12000, 12000) / rms(r0.samples.data() + 12000, 12000));
  CHECK(g == Catch::Approx(12.0).margin(0.05));
  CHECK(r1.info.latencySamples == r0.info.latencySamples);
}

TEST_CASE("Amp controls: live knob moves are allocation-free and the end state equals a chain built there", "[amp][chain][live][rt]") {
  const json tgt = {{"gain", 8.0}, {"bass", 7.0}, {"mid", 2.0}, {"treble", 9.0}, {"presence", 1.0}, {"level", 6.5}};
  auto c = build(mk(json{{"gain", 3.0}, {"mid", 8.0}}, json{{"treble", 2.0}}), 512);
  const auto x = noise(512, 2);
  std::vector<float> y(512);
  LiveParams p = c->liveParams();
  {
    AllocGuard guard;
    for (int i = 0; i < 400; ++i) {
      for (std::size_t k = 0; k < 2; ++k) {
        p.amp[k].gain = 5.0 + 5.0 * std::sin(i * 0.1 + static_cast<double>(k));
        p.amp[k].bass = 5.0 + 5.0 * std::cos(i * 0.07);
        p.amp[k].mid = 5.0 + 5.0 * std::sin(i * 0.05);
        p.amp[k].treble = 5.0 + 5.0 * std::cos(i * 0.11 + static_cast<double>(k));
        p.amp[k].presence = 5.0 + 5.0 * std::sin(i * 0.13);
        p.amp[k].level = 5.0 + 5.0 * std::cos(i * 0.03);
      }
      if (i == 250) p.amp[0].gain = std::nan("");  // ignored
      c->setLiveParams(p);
      c->process(x.data(), y.data(), 1 + (i * 53) % 512);
    }
    CHECK(guard.count() == 0);
  }
  for (std::size_t k = 0; k < 2; ++k) {
    p.amp[k] = knobs(tgt["gain"].get<double>(), tgt["bass"].get<double>(), tgt["mid"].get<double>(),
                     tgt["treble"].get<double>(), tgt["presence"].get<double>(), tgt["level"].get<double>());
  }
  c->setLiveParams(p);
  auto ref = build(mk(tgt, tgt), 512);
  const auto sig = noise(48000, 9, 0.1f);
  const auto a = run(*c, sig, 256), b = run(*ref, sig, 256);
  double d = 0.0;
  for (std::size_t i = 24000; i < sig.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a[i] - b[i])));
  CHECK(d < 1e-4);
}

TEST_CASE("Amp controls: Chain output with knobs moving is bit-identical for any block size", "[amp][chain][live]") {
  const auto x = noise(20000, 31, 0.2f);
  std::vector<float> ref;
  for (const int block : {1, 7, 64, 512, 100000}) {
    CAPTURE(block);
    auto c = build(mk(json{{"mid", 7}}, json{{"bass", 3}}), 512);
    std::vector<float> y(x.size());
    LiveParams p = c->liveParams();
    for (std::size_t pos = 0; pos < x.size();) {
      if (pos == 4000) {
        p.amp[0] = knobs(9, 2, 8, 6, 1, 7);
        p.amp[1].treble = 0.5;
        c->setLiveParams(p);
      }
      if (pos == 9000) {
        p.amp[0].gain = 5.0;
        p.amp[1] = knobs(2, 8, 4, 9, 9, 3);
        c->setLiveParams(p);
      }
      std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos);
      for (const std::size_t at : {std::size_t{4000}, std::size_t{9000}})
        if (pos < at && pos + n > at) n = at - pos;
      c->process(x.data() + pos, y.data() + pos, static_cast<int>(n));
      pos += n;
    }
    if (ref.empty()) ref = y;
    CHECK(y == ref);
  }
}

// ---- preset schema ------------------------------------------------------------------------------------------------
TEST_CASE("Amp controls schema: v1 presets read with default controls; the writer emits v2 without them", "[amp][preset]") {
  json j = mk();
  j["version"] = 1;
  const Preset p = parsePreset(j, kPresets);
  CHECK(p.a.ampControls.isDefault());
  CHECK(p.b.ampControls.isDefault());
  const json out = toJson(p);
  CHECK(out["version"] == 2);
  CHECK_FALSE(out["paths"]["a"].contains("ampControls"));
  CHECK_FALSE(out["paths"]["b"].contains("ampControls"));
  CHECK(parsePreset(out, kPresets) == p);
  static_assert(kPresetVersion == 2);
  j["version"] = 3;
  CHECK_THROWS_WITH(parsePreset(j, kPresets), ContainsSubstring("unsupported preset version 3"));
  j["version"] = 0;
  CHECK_THROWS_AS(parsePreset(j, kPresets), PresetError);
}

TEST_CASE("Amp controls schema: v2 round trip", "[amp][preset]") {
  json j = mk(json{{"gain", 7.25}, {"bass", 3.0}, {"mid", 6.5}, {"treble", 8.125}, {"presence", 0.0}, {"level", 10.0}, {"gainStep", "model-123"}},
              json{{"gain", 6.0}});
  const Preset p = parsePreset(j, kPresets);
  CHECK(p.a.ampControls.gain == 7.25);
  CHECK(p.a.ampControls.bass == 3.0);
  CHECK(p.a.ampControls.mid == 6.5);
  CHECK(p.a.ampControls.treble == 8.125);
  CHECK(p.a.ampControls.presence == 0.0);
  CHECK(p.a.ampControls.level == 10.0);
  CHECK(p.a.ampControls.gainStep == "model-123");
  CHECK(p.b.ampControls.gain == 6.0);
  CHECK(p.b.ampControls.bass == 5.0);  // omitted knobs are at the default
  CHECK(p.b.ampControls.gainStep.empty());
  const json out = toJson(p);
  CHECK(out["version"] == 2);
  CHECK(out["paths"]["a"]["ampControls"]["gainStep"] == "model-123");
  CHECK(out["paths"]["b"]["ampControls"]["gain"] == 6.0);
  CHECK_FALSE(out["paths"]["b"]["ampControls"].contains("gainStep"));
  const Preset q = parsePreset(out, kPresets);
  CHECK(q == p);
  CHECK(toJson(q) == out);
  CHECK_FALSE(p == parsePreset(mk(), kPresets));
}

TEST_CASE("Amp controls schema: gainStep alone is kept; all-default controls are omitted", "[amp][preset]") {
  const Preset p = parsePreset(mk(json{{"gainStep", "m9"}}), kPresets);
  CHECK(p.a.ampControls.knobs().isDefault());
  CHECK_FALSE(p.a.ampControls.isDefault());
  const json out = toJson(p);
  CHECK(out["paths"]["a"]["ampControls"]["gainStep"] == "m9");
  const Preset q = parsePreset(mk(json::object(), json{{"gain", 5}}), kPresets);  // spelled-out defaults
  CHECK(q.a.ampControls.isDefault());
  CHECK_FALSE(toJson(q)["paths"]["a"].contains("ampControls"));
  CHECK_FALSE(toJson(q)["paths"]["b"].contains("ampControls"));
}

TEST_CASE("Amp controls schema: out-of-range and malformed values are PresetErrors naming the field", "[amp][preset]") {
  const auto err = [](const json& j) -> std::string {
    try {
      parsePreset(j, kPresets);
    } catch (const PresetError& e) {
      return e.jsonPath();
    }
    return "(no error)";
  };
  for (const char* knob : {"gain", "bass", "mid", "treble", "presence", "level"}) {
    CAPTURE(knob);
    CHECK(err(mk(json{{knob, 10.001}})) == std::string("paths.a.ampControls.") + knob);
    CHECK(err(mk(nullptr, json{{knob, -0.001}})) == std::string("paths.b.ampControls.") + knob);
    CHECK(err(mk(json{{knob, "5"}})) == std::string("paths.a.ampControls.") + knob);
    CHECK(err(mk(json{{knob, 0.0}})) == "(no error)");
    CHECK(err(mk(json{{knob, 10.0}})) == "(no error)");
  }
  CHECK(err(mk(json{{"gainStep", 7}})) == "paths.a.ampControls.gainStep");
  CHECK(err(mk(nullptr, json{{"gainStep", json::array()}})) == "paths.b.ampControls.gainStep");
  CHECK(err(mk(json{{"gainStep", ""}})) == "paths.a.ampControls.gainStep");
  CHECK(err(mk(json{{"drive", 5}})) == "paths.a.ampControls.drive");  // unknown key
  CHECK(err(mk(json::array())) == "paths.a.ampControls");
  CHECK_THROWS_WITH(parsePreset(mk(json{{"mid", 11}}), kPresets), ContainsSubstring("paths.a.ampControls.mid"));
}

TEST_CASE("Amp controls schema: every committed preset reads at neutral defaults and writes none", "[amp][preset][neutral]") {
  int n = 0;
  for (const auto& e : fs::recursive_directory_iterator(SAWBLADE_PRESETS_DIR)) {
    if (e.path().extension() != ".json") continue;
    CAPTURE(e.path().string());
    const Preset p = loadPresetFile(e.path());  // parses (captures are not opened)
    CHECK(p.a.ampControls.isDefault());
    CHECK(p.b.ampControls.isDefault());
    const json out = toJson(p);
    CHECK_FALSE(out["paths"]["a"].contains("ampControls"));
    CHECK_FALSE(out["paths"]["b"].contains("ampControls"));
    CHECK(out["version"] == 2);
    CHECK(parsePreset(out, e.path().parent_path()) == p);
    ++n;
  }
  CHECK(n >= 40);
}

TEST_CASE("Amp controls: every committed preset renders bit-identically with and without explicit default ampControls", "[amp][preset][neutral][render]") {
  // Every JSON under presets/, its NAM captures swapped for the linear_identity fixture and its IRs for a fixture IR
  // (the real captures are not in the repo), rendered as committed and with `ampControls` spelled out at the defaults on
  // both paths: the neutral stage is skipped entirely, so the two renders must be identical bit for bit. (Against a build
  // without the feature the same holds: verified by rendering all presets with the pre-v0.2 tonerender, see the report.)
  const fs::path fx = fs::path(SAWBLADE_FIXTURES_DIR);
  AudioFile in;
  in.sampleRate = kFs;
  in.channels = 1;
  in.interleaved = noise(16000, 77, 0.25f);
  const auto standIn = [&](json& cap, bool ir) {
    if (!cap.is_object()) return;
    cap["file"] = (fx / (ir ? "ir/impulse.wav" : "nam/linear_identity.nam")).string();
    cap.erase("sha256");
  };
  int n = 0;
  for (const auto& e : fs::recursive_directory_iterator(SAWBLADE_PRESETS_DIR)) {
    if (e.path().extension() != ".json") continue;
    CAPTURE(e.path().string());
    std::ifstream f(e.path());
    json j = json::parse(f);
    for (const char* k : {"a", "b"})
      for (auto& b : j["paths"][k]["blocks"])
        if (b["type"] == "nam") standIn(b["model"], false);
    for (const char* key : {"ir", "irA", "irB"})
      if (j["cab"].contains(key)) standIn(j["cab"][key], true);
    json explicitDefaults = j;
    for (const char* k : {"a", "b"})
      explicitDefaults["paths"][k]["ampControls"] = {{"gain", 5}, {"bass", 5}, {"mid", 5}, {"treble", 5}, {"presence", 5}, {"level", 5}};
    const Preset p0 = parsePreset(j, e.path().parent_path());
    const Preset p1 = parsePreset(explicitDefaults, e.path().parent_path());
    REQUIRE(p0 == p1);  // spelled-out defaults are the same preset
    const RenderResult r0 = renderPreset(p0, in);
    const RenderResult r1 = renderPreset(p1, in);
    REQUIRE(r0.samples.size() == in.interleaved.size());
    CHECK(r0.samples == r1.samples);
    CHECK(r0.info.latencySamples == r1.info.latencySamples);
    ++n;
  }
  CHECK(n >= 40);
}
