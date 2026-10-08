// v0.3 Task B: level-matched auditioning in core - the reference DI, the trim and its staleness hash, the schema (v3), the chain's
// trim gain, the capture-swap make-up, and the acceptance tests over the committed presets.
#include <algorithm>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/auto_trim.h"
#include "sawblade/capture_cache.h"
#include "sawblade/chain.h"
#include "sawblade/loudness.h"
#include "sawblade/reference_di.h"
#include "sawblade/render.h"
#include "sawblade/sha256.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = SAWBLADE_FIXTURES_DIR;
const fs::path kPresets = kFixtures / "presets";
const fs::path kFactory = SAWBLADE_PRESETS_DIR;
const fs::path kGolden = fs::path(SAWBLADE_GOLDEN_DIR) / "preset_render_hashes.json";
constexpr double kFs = 48000.0;

json nam(const std::string& id, const std::string& file, const char* slot = nullptr) {
  json b = {{"id", id}, {"type", "nam"}, {"model", {{"file", "../nam/" + file}}}};
  if (slot) b["slot"] = slot;
  return b;
}

// Two paths with one fixture model each, align off, impulse cab, linear blend `blend`.
// `inputDb` keeps the fixture rigs (linear models: the reference DI is about -31 LUFS through them) loud enough that their trims stay
// under the +12 dB limit.
json twoPaths(const std::string& aFile, const std::string& bFile, double blend = 0.5, double inputDb = 10.0) {
  return {{"schema", "sawblade.preset"}, {"version", 3}, {"name", "trim"}, {"input", {{"gainDb", inputDb}}},
          {"paths", {{"a", {{"blocks", json::array({nam("a1", aFile, "amp")})}}},
                     {"b", {{"blocks", json::array({nam("b1", bFile, "amp")})}}}}},
          {"align", {{"mode", "off"}}},
          {"blend", blend},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", "../ir/impulse.wav"}}}}}};
}

Preset parse(const json& j) { return parsePreset(j, kPresets); }

double lufsOfSamples(const std::vector<float>& x) {
  const std::vector<float> zeros(x.size(), 0.0f);
  const auto l = integratedLoudnessLufs(x.data(), zeros.data(), static_cast<std::int64_t>(x.size()), kFs);
  REQUIRE(l.has_value());
  return *l;
}

// The reference DI through `p` with the trim applied (LEVEL MATCH on), as the plugin and tonerender --level-match do.
double lufsWithTrim(const Preset& p, CaptureCache* cache = nullptr) {
  const auto l = measureReferenceLufs(p, cache, /*applyTrim=*/true);
  REQUIRE(l.has_value());
  return *l;
}

bool updateGolden() {
  const char* e = std::getenv("SAWBLADE_UPDATE_GOLDEN");
  return e && std::string(e) == "1";
}

std::vector<fs::path> factoryPresets() {
  std::vector<fs::path> v;
  for (const auto& e : fs::recursive_directory_iterator(kFactory))
    if (e.path().extension() == ".json") v.push_back(e.path());
  std::sort(v.begin(), v.end());
  return v;
}

std::string relToFactory(const fs::path& p) { return fs::relative(p, kFactory).generic_string(); }

std::string sampleHash(const std::vector<float>& x) { return sha256Hex(x.data(), x.size() * sizeof(float)); }

}  // namespace

// ---- the reference DI ---------------------------------------------------------------------------
TEST_CASE("Reference DI: 10 s, 48 kHz, peak -10 dBFS, finite, and the same bytes every time", "[autotrim][refdi]") {
  const std::vector<float>& x = referenceDi();
  REQUIRE(x.size() == static_cast<std::size_t>(kReferenceDiSeconds * kReferenceDiRate));
  double peak = 0.0;
  bool finite = true;
  for (float v : x) {
    peak = std::max(peak, std::fabs(static_cast<double>(v)));
    finite = finite && std::isfinite(v);
  }
  CHECK(finite);
  CHECK(toDb(peak) == Approx(kReferenceDiPeakDbfs).margin(1e-4));
  CHECK(&referenceDi() == &x);  // generated once
  // Guitar-shaped: not silent, sparse enough to have gaps (palm-muted chugs), loud enough for the BS.1770 gates.
  const double l = lufsOfSamples(x);
  INFO("reference DI loudness " << l << " LUFS");
  CHECK(l > -35.0);
  CHECK(l < -15.0);
  // Bit-reproducible: pure +, -, *, / on doubles (no libm), so this is the same on every machine and compiler. A change here is a
  // new kReferenceDiVersion (and every stored autoTrimHash goes stale with it).
  static_assert(kReferenceDiVersion == 1);
  CHECK(sampleHash(x) == "9a365efeec001aa1533cc1abc4f45cab0fb1c94f4028e0e4e6d21a6ad2f30928");
}

TEST_CASE("Auto trim: a set cancel flag skips the measurement after the render", "[autotrim]") {
  const Preset p = parse(twoPaths("linear_identity.nam", "linear_05_025.nam"));
  std::atomic<bool> cancel{false};
  CHECK(measureReferenceLufs(p, nullptr, false, &cancel).has_value());
  cancel.store(true);
  CHECK_FALSE(measureReferenceLufs(p, nullptr, false, &cancel).has_value());
  CHECK_FALSE(computeAutoTrim(p, nullptr, &cancel).has_value());
  CHECK_FALSE(slotMakeupDb(p, p, 0, nullptr, &cancel).has_value());
}

// ---- schema v3 ----------------------------------------------------------------------------------
TEST_CASE("Preset v3: output.autoTrim.db / autoTrimHash and nam makeupDb round-trip; v1 and v2 files still read", "[autotrim][preset]") {
  json j = twoPaths("linear_identity.nam", "linear_identity.nam");
  for (int ver : {1, 2}) {
    j["version"] = ver;
    const Preset p = parse(j);
    CHECK(p.autoTrim.hash.empty());
    CHECK(p.autoTrim.db == 0.0);
    CHECK(slotMakeupOf(p, 0, 0) == 0.0);
    const json out = toJson(p);
    CHECK(out["version"] == kPresetVersion);
    CHECK_FALSE(out["output"].contains("autoTrimDb"));  // absent until measured
    CHECK_FALSE(out["paths"]["a"]["blocks"][0].contains("makeupDb"));
  }
  j["version"] = 3;
  j["output"] = {{"gainDb", -2.0}, {"autoTrimDb", -7.5}, {"autoTrimHash", "abc"}};
  j["paths"]["a"]["blocks"][0]["makeupDb"] = 3.25;
  const Preset p = parse(j);
  CHECK(p.autoTrim.db == -7.5);
  CHECK(p.autoTrim.hash == "abc");
  CHECK(slotMakeupOf(p, 0, 0) == 3.25);
  CHECK(slotMakeupOf(p, 1, 0) == 0.0);
  const json out = toJson(p);
  CHECK(out["output"]["autoTrimDb"] == -7.5);
  CHECK(out["output"]["autoTrimHash"] == "abc");
  CHECK(out["paths"]["a"]["blocks"][0]["makeupDb"] == 3.25);
  CHECK(parse(out) == p);
  // A trim with no hash cannot be checked: read as not measured.
  j["output"] = {{"gainDb", 0.0}, {"autoTrimDb", -7.5}};
  const Preset q = parse(j);
  CHECK(q.autoTrim.hash.empty());
  CHECK(q.autoTrim.db == 0.0);
  j["version"] = kPresetVersion + 1;
  CHECK_THROWS_WITH(parse(j), ContainsSubstring("unsupported preset version"));
}

// ---- the staleness hash -------------------------------------------------------------------------
TEST_CASE("Auto trim hash: covers what affects level, ignores names, notes, stored trims and capture metadata", "[autotrim][hash]") {
  const Preset base = parse(twoPaths("linear_identity.nam", "linear_05_025.nam"));
  const std::string h = autoTrimHash(base);
  CHECK(h.size() == 64);
  CHECK_FALSE(autoTrimFresh(base));  // nothing stored
  {
    Preset p = base;
    p.name = "other";
    p.notes = "n";
    p.category = "c";
    p.version = 1;
    p.autoTrim.db = -3.0;
    p.autoTrim.hash = "stale";
    CHECK(autoTrimHash(p) == h);
  }
  const auto differs = [&](const std::function<void(json&)>& edit) {
    json j = twoPaths("linear_identity.nam", "linear_05_025.nam");
    edit(j);
    return autoTrimHash(parse(j)) != h;
  };
  CHECK(differs([](json& j) { j["blend"] = 0.6; }));
  // OUTPUT is the user's persistent offset: it is not in the hash (and the trim is measured with it at 0 dB).
  CHECK_FALSE(differs([](json& j) { j["output"] = {{"gainDb", -1.0}}; }));
  CHECK(differs([](json& j) { j["input"] = {{"gainDb", 2.0}}; }));
  CHECK(differs([](json& j) { j["paths"]["b"]["levelDb"] = -2.0; }));
  CHECK(differs([](json& j) { j["paths"]["a"]["ampControls"] = {{"gain", 7.0}}; }));
  CHECK(differs([](json& j) { j["paths"]["a"]["blocks"][0]["makeupDb"] = 1.0; }));
  CHECK(differs([](json& j) { j["paths"]["a"]["blocks"][0]["model"]["file"] = "../nam/wavenet.nam"; }));
  CHECK(differs([](json& j) { j["postEq"] = json::array({{{"type", "peak"}, {"freq", 800.0}, {"gainDb", 4.0}}}); }));
  CHECK(differs([](json& j) { j["cab"]["enabled"] = false; }));
  // A TONE3000 capture is its ids: title / url / creator / licence / local path do not count.
  const auto sourced = [&](const std::string& file, const std::string& title, const std::string& modelId) {
    json j = twoPaths("linear_identity.nam", "linear_05_025.nam");
    j["paths"]["a"]["blocks"][0]["model"] = {{"file", file},
                                             {"source", {{"provider", "tone3000"}, {"id", "T1"}, {"modelId", modelId}, {"title", title}, {"creator", title}, {"license", "t3k"}}}};
    return autoTrimHash(parse(j));
  };
  CHECK(sourced("../nam/linear_identity.nam", "A", "m1") == sourced("/elsewhere/x.nam", "B", "m1"));
  CHECK(sourced("../nam/linear_identity.nam", "A", "m1") != sourced("../nam/linear_identity.nam", "A", "m2"));
}

// ---- computing and applying -----------------------------------------------------------------------
TEST_CASE("Auto trim: brings a preset to -18 LUFS on the reference DI, only when asked for, and goes stale on edits", "[autotrim]") {
  Preset p = parse(twoPaths("linear_05_025.nam", "linear_identity.nam"));
  const auto raw = measureReferenceLufs(p);
  REQUIRE(raw.has_value());
  CHECK(*raw != Approx(kAutoTrimTargetLufs).margin(0.5));  // the test would be vacuous otherwise
  REQUIRE(ensureAutoTrim(p));
  CHECK(autoTrimFresh(p));
  CHECK(p.autoTrim.db == Approx(kAutoTrimTargetLufs - *raw).margin(1e-9));
  CHECK(lufsWithTrim(p) == Approx(kAutoTrimTargetLufs).margin(0.01));
  // Not applied unless asked for: the default render ignores the stored trim, bit for bit.
  AudioFile in{kFs, 1, referenceDi()};
  const RenderResult plain = renderPreset(p, in);
  Preset noTrim = p;
  noTrim.autoTrim.db = 0.0;
  noTrim.autoTrim.hash.clear();
  const RenderResult plain2 = renderPreset(noTrim, in);
  SAWBLADE_REQUIRE_SAME_SAMPLES(plain2.samples, plain.samples);
  CHECK(plain.autoTrimDb == 0.0);
  RenderOptions o;
  o.applyAutoTrim = true;
  const RenderResult trimmed = renderPreset(p, in, o);
  CHECK(trimmed.autoTrimDb == p.autoTrim.db);
  CHECK(lufsOfSamples(trimmed.samples) - lufsOfSamples(plain.samples) == Approx(p.autoTrim.db).margin(0.01));
  // An edit that changes the level makes it stale; ensureAutoTrim measures again.
  p.blend = 0.9;
  CHECK_FALSE(autoTrimFresh(p));
  const double old = p.autoTrim.db;
  REQUIRE(ensureAutoTrim(p));
  CHECK(autoTrimFresh(p));
  CHECK(lufsWithTrim(p) == Approx(kAutoTrimTargetLufs).margin(0.01));
  CHECK(p.autoTrim.db != old);
  // OUTPUT is a persistent offset on top of the match: the trim is measured with it at 0 dB, so it does not depend on it, and the
  // preset as stored plays at -18 LUFS + its output gain.
  Preset g = parse(twoPaths("linear_05_025.nam", "linear_identity.nam"));
  Preset g0 = g;
  g.outputGainDb = -4.0;
  CHECK(autoTrimHash(g) == autoTrimHash(g0));
  REQUIRE(ensureAutoTrim(g));
  REQUIRE(ensureAutoTrim(g0));
  CHECK(g.autoTrim.db == Approx(g0.autoTrim.db).margin(1e-9));
  CHECK(lufsWithTrim(g) == Approx(kAutoTrimTargetLufs).margin(0.01));  // measured with OUTPUT forced to 0 dB
  RenderOptions ao;
  ao.applyAutoTrim = true;
  CHECK(lufsOfSamples(renderPreset(g, in, ao).samples) == Approx(kAutoTrimTargetLufs - 4.0).margin(0.05));  // as stored
}

TEST_CASE("Auto trim: positive trims stop at +12 dB; rigs with no active non-linear block get 0", "[autotrim]") {
  // A quiet non-linear rig wants more than +12 dB: it gets +12.
  Preset quiet = parse(twoPaths("linear_identity.nam", "linear_identity.nam", 0.5, -20.0));
  const auto r = computeAutoTrim(quiet);
  REQUIRE(r.has_value());
  CHECK(kAutoTrimTargetLufs - r->lufs > kMaxPositiveTrimDb);
  CHECK(r->trimDb == kMaxPositiveTrimDb);
  // A hot one is attenuated, down to the (larger) negative limit.
  Preset hot = parse(twoPaths("linear_identity.nam", "linear_identity.nam", 0.5, 40.0));
  const auto h = computeAutoTrim(hot);
  REQUIRE(h.has_value());
  CHECK(h->trimDb == Approx(kAutoTrimTargetLufs - h->lufs).margin(1e-9));
  CHECK(h->trimDb < -kMaxPositiveTrimDb);
  // No NAM, no pedal model: Init (empty), EQ and cab only, or every block bypassed / on a disabled path -> trim 0.
  Preset init = parse({{"schema", "sawblade.preset"}, {"version", 3}, {"name", "empty"},
                       {"paths", {{"a", {{"blocks", json::array()}}}, {"b", {{"blocks", json::array()}}}}},
                       {"align", {{"mode", "off"}}}, {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}});
  CHECK_FALSE(hasNonlinearBlock(init));
  const auto ri = computeAutoTrim(init);
  REQUIRE(ri.has_value());
  CHECK(ri->lufs < -25.0);  // it would have wanted a big boost
  CHECK(ri->trimDb == 0.0);
  Preset eqOnly = init;
  eqOnly.postEq.push_back({EqType::Peak, 800.0, 3.0, 1.0, true});
  CHECK(computeAutoTrim(eqOnly)->trimDb == 0.0);
  Preset bypassed = quiet;
  for (PathPreset* pp : {&bypassed.a, &bypassed.b})
    for (Block& b : pp->blocks) b.bypass = true;
  CHECK_FALSE(hasNonlinearBlock(bypassed));
  CHECK(computeAutoTrim(bypassed)->trimDb == 0.0);
  Preset oneDisabled = quiet;
  oneDisabled.b.enabled = false;
  CHECK(hasNonlinearBlock(oneDisabled));
  oneDisabled.a.enabled = false;
  CHECK_FALSE(hasNonlinearBlock(oneDisabled));
  CHECK(hasNonlinearBlock(quiet));
  // A modeled pedal counts.
  const Preset pedal = loadPresetFile(kFactory / "modeled" / "hm_chainsaw.json");
  CHECK(hasNonlinearBlock(pedal));
}

TEST_CASE("Auto trim: a silent preset has no trim", "[autotrim]") {
  json j = twoPaths("linear_identity.nam", "linear_identity.nam");
  j["input"] = {{"gainDb", -120.0}};
  Preset p = parse(j);
  CHECK_FALSE(computeAutoTrim(p).has_value());
  CHECK_FALSE(stampAutoTrim(p));
  CHECK(p.autoTrim.hash.empty());
}

// ---- the chain's trim gain ----------------------------------------------------------------------------
namespace {
std::unique_ptr<Chain> buildChain(const Preset& p, int maxBlock = 256) {
  auto c = std::make_unique<Chain>(p, loadResources(p, kFs));
  c->prepare({kFs, maxBlock});
  return c;
}
}  // namespace

TEST_CASE("Chain::setAutoTrimDb: the first value is immediate, later ones ramp over >= 200 ms, nothing allocates", "[autotrim][chain][rt]") {
  const Preset p = parse(twoPaths("linear_identity.nam", "linear_identity.nam"));
  auto c = buildChain(p);
  const std::vector<float> in(static_cast<std::size_t>(kFs), 0.1f);
  std::vector<float> out(in.size());
  const auto run = [&](Chain& ch, std::size_t from, std::size_t count) {
    for (std::size_t pos = from; pos < from + count; pos += 256) ch.process(in.data() + pos, out.data() + pos, 256);
  };
  run(*c, 0, 4096);  // settle the cab / blend
  const float unity = out[4095];
  CHECK(unity != 0.0f);
  c->setAutoTrimDb(-6.0);  // the first call: at once
  run(*c, 4096, 256);
  CHECK(out[4096] == Approx(unity * std::pow(10.0f, -6.0f / 20.0f)).epsilon(1e-4));
  {
    AllocGuard guard;
    c->setAutoTrimDb(+6.0);  // a change: ramps
    c->setAutoTrimDb(+6.0);  // unchanged: nothing
    c->setAutoTrimDb(std::nan(""));  // ignored
    run(*c, 4352, 256 * 20);
    CHECK(guard.count() == 0);
  }
  const float lo = unity * std::pow(10.0f, -6.0f / 20.0f), hi = unity * std::pow(10.0f, 6.0f / 20.0f);
  const std::size_t base = 4352;
  const auto at = [&](double seconds) { return out[base + static_cast<std::size_t>(seconds * kFs)]; };
  CHECK(at(0.001) < lo * 1.05f);          // the ramp has hardly started
  CHECK(at(0.100) > lo * 1.05f);          // it is on its way ...
  CHECK(at(0.100) < hi * 0.95f);          // ... and not there after 100 ms (>= 200 ms ramp)
  run(*c, 4352 + 256 * 20, 256 * 100);
  CHECK(out[4352 + 256 * 20 + 256 * 99] == Approx(hi).epsilon(1e-4));
  CHECK(c->autoTrimDb() == 6.0);
  static_assert(Chain::kAutoTrimRampMs >= 200.0);
}

TEST_CASE("Chain: a nam block's makeupDb is a gain on its output, in the build and in the live parameters", "[autotrim][chain]") {
  const Preset base = parse(twoPaths("linear_identity.nam", "linear_identity.nam", 0.0));
  const Preset up = withSlotMakeup(base, 0, 0, 6.0);
  CHECK(slotMakeupOf(up, 0, 0) == 6.0);
  CHECK(slotMakeupOf(base, 0, 0) == 0.0);  // the original is untouched
  const std::vector<float> x = noise(24000, 3, 0.1f);
  const auto render = [&](Chain& ch) {
    std::vector<float> y(x.size());
    for (std::size_t pos = 0; pos < x.size(); pos += 256) ch.process(x.data() + pos, y.data() + pos, static_cast<int>(std::min<std::size_t>(256, x.size() - pos)));
    return y;
  };
  auto a = buildChain(base);
  auto b = buildChain(up);
  const auto ya = render(*a), yb = render(*b);
  CHECK(lufsOfSamples(yb) - lufsOfSamples(ya) == Approx(6.0).margin(0.01));
  // Live: the preset's own makeup is part of LiveParams::fromPreset, so a live republish keeps it.
  auto c = buildChain(base);
  c->setLiveParams(LiveParams::fromPreset(up));
  render(*c);  // let the 20 ms ramp finish
  const auto yc = render(*c);
  std::vector<float> tail_b(yb.end() - 8000, yb.end()), tail_c(yc.end() - 8000, yc.end());
  CHECK(lufsOfSamples(yc) - lufsOfSamples(ya) == Approx(6.0).margin(0.05));
  double maxd = 0.0;
  for (std::size_t i = 0; i < tail_b.size(); ++i) maxd = std::max(maxd, std::fabs(static_cast<double>(tail_b[i]) - tail_c[i]));
  CHECK(maxd < 1e-5);
  // 0 dB make-up is the same sound bit for bit.
  const Preset same = withSlotMakeup(base, 0, 0, 0.0);
  auto d = buildChain(same);
  SAWBLADE_REQUIRE_SAME_SAMPLES(ya, render(*d));
}

// ---- capture swap make-up -------------------------------------------------------------------------------
TEST_CASE("Capture swap make-up keeps the slot's path loudness on the reference DI within 0.5 LU", "[autotrim][swap]") {
  // Single-path rig: swap the amp's capture for ones of a different level / tilt.
  for (const char* swapTo : {"linear_05_025.nam", "wavenet.nam", "lstm.nam", "linear_identity_loud24.nam"}) {
    INFO("swap to " << swapTo);
    const Preset before = parse(twoPaths("linear_identity.nam", "linear_identity.nam", 0.0));
    json j = twoPaths(swapTo, "linear_identity.nam", 0.0);
    const Preset after = parse(j);
    const auto lb = measurePathLufs(before, 0), la = measurePathLufs(after, 0);
    REQUIRE(lb.has_value());
    REQUIRE(la.has_value());
    const auto mk = slotMakeupDb(before, after, 0);
    REQUIRE(mk.has_value());
    const Preset fixed = withSlotMakeup(after, 0, 0, *mk);
    const auto lf = measurePathLufs(fixed, 0);
    REQUIRE(lf.has_value());
    CHECK(*lf == Approx(*lb).margin(0.5));
    if (std::fabs(*lb - *la) > 1.0) CHECK(std::fabs(*mk) > 0.5);  // it did something
    // The whole preset as heard (here A is all of it): within 0.5 LU too.
    const auto wb = measureReferenceLufs(before), wf = measureReferenceLufs(fixed);
    REQUIRE(wb.has_value());
    REQUIRE(wf.has_value());
    CHECK(*wf == Approx(*wb).margin(0.5));
  }
  // Blend: swapping path B's capture keeps B's own loudness (and so the A / B balance and the blend).
  const Preset before = parse(twoPaths("linear_identity.nam", "linear_05_025.nam", 0.5));
  const Preset after = parse(twoPaths("linear_identity.nam", "wavenet.nam", 0.5));
  const auto mk = slotMakeupDb(before, after, 1);
  REQUIRE(mk.has_value());
  const Preset fixed = withSlotMakeup(after, 1, 0, *mk);
  CHECK(*measurePathLufs(fixed, 1) == Approx(*measurePathLufs(before, 1)).margin(0.5));
  CHECK(*measurePathLufs(fixed, 0) == Approx(*measurePathLufs(before, 0)).margin(0.01));  // path A is untouched
  // A disabled path has nothing to measure.
  Preset off = before;
  off.b.enabled = false;
  CHECK_FALSE(measurePathLufs(off, 1).has_value());
  CHECK_FALSE(slotMakeupDb(before, off, 1).has_value());
}

TEST_CASE("withSlotMakeup: only nam blocks carry it, and it is clamped", "[autotrim][swap]") {
  const Preset p = parse(twoPaths("linear_identity.nam", "linear_identity.nam"));
  CHECK(slotMakeupOf(withSlotMakeup(p, 0, 0, 100.0), 0, 0) == kMaxSlotMakeupDb);
  CHECK(slotMakeupOf(withSlotMakeup(p, 0, 0, -100.0), 0, 0) == -kMaxSlotMakeupDb);
  CHECK(withSlotMakeup(p, 0, 5, 3.0) == p);  // no such block
  CHECK(slotMakeupOf(p, 0, 5) == 0.0);
}

// ---- A/B at matched loudness ----------------------------------------------------------------------------
TEST_CASE("A/B pair: two different presets, both at their trims, are within 0.5 LU of each other", "[autotrim][ab]") {
  Preset a = parse(twoPaths("linear_identity.nam", "linear_identity.nam", 0.0, 6.0));
  Preset b = parse(twoPaths("wavenet.nam", "linear_05_025.nam", 0.7, 20.0));
  const double rawA = *measureReferenceLufs(a), rawB = *measureReferenceLufs(b);
  CHECK(std::fabs(rawA - rawB) > 3.0);  // audibly different without the match
  REQUIRE(ensureAutoTrim(a));
  REQUIRE(ensureAutoTrim(b));
  const double la = lufsWithTrim(a), lb = lufsWithTrim(b);
  CHECK(std::fabs(la - lb) <= 0.5);
  CHECK(la == Approx(kAutoTrimTargetLufs).margin(0.5));
  CHECK(lb == Approx(kAutoTrimTargetLufs).margin(0.5));
}

// ---- the committed presets ------------------------------------------------------------------------------
TEST_CASE("Committed presets: every one that renders here is within 0.5 LU of -18 LUFS with the trim applied", "[autotrim][presets]") {
  const fs::path file = GENERATE(from_range(factoryPresets()));
  INFO(relToFactory(file));
  Preset p = loadPresetFile(file);
  if (const auto missing = missingCaptures(p); !missing.empty())
    SKIP("skipped: capture not cached (" << missing.front() << "); its trim is computed in the plugin at load, or run "
         "scripts/compute_trims.py on a machine that has the captures");
  if (!p.autoTrim.hash.empty()) {
    // A stored trim must belong to the preset as committed: edit a preset, rerun scripts/compute_trims.py.
    CHECK(autoTrimFresh(p));
  }
  CaptureCache cache;
  REQUIRE(ensureAutoTrim(p, &cache));  // a preset without a stored trim is measured, as the plugin does at load
  const double lu = lufsWithTrim(p, &cache);
  INFO("trim " << p.autoTrim.db << " dB, loudness with trim " << lu << " LUFS");
  CHECK(lu == Approx(kAutoTrimTargetLufs).margin(0.5));
}

TEST_CASE("Committed presets: every one that renders here has its trim committed (the capture-less ones are never left without)", "[autotrim][presets]") {
  int renderable = 0, withTrim = 0;
  for (const fs::path& f : factoryPresets()) {
    const Preset p = loadPresetFile(f);
    if (!missingCaptures(p).empty()) continue;
    ++renderable;
    if (autoTrimFresh(p)) ++withTrim;
    else WARN(relToFactory(f) << " has no fresh trim: run scripts/compute_trims.py");
  }
  CHECK(renderable >= 30);
  // Presets that need TONE3000 captures may legitimately lack one until they are computed on a machine with the cache; every
  // preset that rendered here when the trims were generated has one.
  CHECK(withTrim >= 30);
}

// With LEVEL MATCH off (the default of every render) nothing about a committed preset changes: each one renders bit-identically to
// what cdb4b9a rendered (the v0.2 merge, before level matching). The hashes were recorded with that core; the file also holds the
// level so that other platforms and compilers (macOS, clang: libm and constant folding may round differently) are still checked, to
// 0.01 dB.
TEST_CASE("Committed presets render bit-identically to cdb4b9a with LEVEL MATCH off", "[golden][golden-presets]") {
  const fs::path di = kFixtures / "di_riff.wav";
  json doc = fs::exists(kGolden) ? json::parse(std::ifstream(kGolden)) : json{{"presets", json::object()}};
  if (updateGolden()) {
    json presets = json::object();
    for (const fs::path& f : factoryPresets()) {
      if (!missingCaptures(loadPresetFile(f)).empty()) continue;
      const RenderResult r = renderFile(f, di);
      presets[relToFactory(f)] = {{"sha256", sampleHash(r.samples)}, {"frames", r.samples.size()},
                                  {"rmsDbfs", std::round(r.output.rmsDbfs * 1e4) / 1e4}, {"peakDbfs", std::round(r.output.peakDbfs * 1e4) / 1e4}};
    }
    doc["presets"] = presets;
    std::ofstream(kGolden) << doc.dump(2) << "\n";
    WARN("SAWBLADE_UPDATE_GOLDEN=1: rewrote " << kGolden.string());
    return;
  }
  REQUIRE(doc.contains("presets"));
  int checked = 0;
  for (const fs::path& f : factoryPresets()) {
    const std::string rel = relToFactory(f);
    INFO(rel);
    const Preset p = loadPresetFile(f);
    if (!missingCaptures(p).empty()) continue;  // needs TONE3000 captures that are not here
    if (!doc["presets"].contains(rel)) {
      WARN(rel << ": no baseline recorded (its captures were not available when the hashes were recorded)");
      continue;
    }
    const json& want = doc["presets"][rel];
    const RenderResult r = renderFile(f, di);  // default options: no trim
    CHECK(r.autoTrimDb == 0.0);
    REQUIRE(r.samples.size() == want["frames"].get<std::size_t>());
#if defined(__linux__) && defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)  // the compiler the hashes were recorded with
    CHECK(sampleHash(r.samples) == want["sha256"].get<std::string>());
#else
    CHECK(r.output.rmsDbfs == Approx(want["rmsDbfs"].get<double>()).margin(0.01));
    CHECK(r.output.peakDbfs == Approx(want["peakDbfs"].get<double>()).margin(0.01));
#endif
    ++checked;
  }
  CHECK(checked >= 30);
}

// ---- v0.8 I2 Part 1: calibration-aware measurements --------------------------------------------------------------
namespace {
ChainCalibration calOn(double dbu = 12.0) {
  ChainCalibration c;
  c.enabled = true;
  c.device.dbu = dbu;
  return c;
}
// Path A = pedal NAM -> amp NAM (both with dBu metadata), path B = identity, blend 0 (only A is heard).
json hopRig(const char* pedal, const char* amp, double inputDb = 10.0) {
  json j = twoPaths("linear_identity.nam", "linear_identity.nam", 0.0, inputDb);
  j["paths"]["a"]["blocks"] = json::array({nam("p1", pedal, "pedal"), nam("a1", amp, "amp")});
  return j;
}
}  // namespace

TEST_CASE("Calibration I2: with calibration off the measurements are bit-identical to the pre-I2 calls", "[autotrim][calibration]") {
  const Preset p = parse(hopRig("cal_pedal_a.nam", "wavenet.nam"));
  const Preset q = parse(hopRig("cal_pedal_b.nam", "wavenet.nam"));
  ChainCalibration offWithDevice = calOn(20.0);
  offWithDevice.enabled = false;  // a device record alone changes nothing
  for (const ChainCalibration& off : {ChainCalibration{}, offWithDevice}) {
    CHECK(measureReferenceLufs(p, nullptr, false, nullptr, off) == measureReferenceLufs(p));
    CHECK(measureReferenceLufs(p, nullptr, true, nullptr, off) == measureReferenceLufs(p, nullptr, true));
    CHECK(measurePathLufs(p, 0, nullptr, nullptr, off) == measurePathLufs(p, 0));
    const auto a = computeAutoTrim(p, nullptr, nullptr, off), b = computeAutoTrim(p);
    REQUIRE((a && b));
    CHECK(a->trimDb == b->trimDb);
    CHECK(a->hash == b->hash);
    CHECK(slotMakeupDb(p, q, 0, nullptr, nullptr, off, 0) == slotMakeupDb(p, q, 0));
    const auto m = slotMakeup(p, q, 0, 0, off);
    REQUIRE(m.has_value());
    CHECK_FALSE(m->skippedHop);
    CHECK(m->makeupDb == *slotMakeupDb(p, q, 0));
    CHECK_FALSE(blockFeedsNam(p, 0, 0, off));
  }
}

TEST_CASE("Calibration I2: the auto trim measured with calibration on lands the calibrated chain at -18 LUFS", "[autotrim][calibration]") {
  const ChainCalibration c = calOn();
  Preset p = parse(hopRig("cal_pedal_a.nam", "wavenet.nam"));
  const auto offLufs = measureReferenceLufs(p);
  const auto onLufs = measureReferenceLufs(p, nullptr, false, nullptr, c);
  REQUIRE((offLufs && onLufs));
  CHECK(std::fabs(*offLufs - *onLufs) > 1.0);  // calibration moves the level, so measuring with it is not vacuous
  REQUIRE(ensureAutoTrim(p, nullptr, c));
  // Played through the calibrated chain with the trim applied (as the plugin will): -18 LUFS within the usual 0.01.
  const auto played = measureReferenceLufs(p, nullptr, /*applyTrim=*/true, nullptr, c);
  REQUIRE(played.has_value());
  CHECK(*played == Approx(kAutoTrimTargetLufs).margin(0.01));
  CHECK(p.autoTrim.db == Approx(kAutoTrimTargetLufs - *onLufs).margin(1e-9));
  // The uncalibrated measurement would have missed.
  Preset u = p;
  REQUIRE(stampAutoTrim(u));
  CHECK(u.autoTrim.db != Approx(p.autoTrim.db).margin(0.5));
  // The level-match probe inside Chain::prepare sees the same calibration: a render with LEVEL MATCH + calibration still matches.
  AudioFile in{kFs, 1, referenceDi()};
  RenderOptions o;
  o.applyAutoTrim = true;
  o.calibration = c;
  CHECK(lufsOfSamples(renderPreset(p, in, o).samples) == Approx(kAutoTrimTargetLufs).margin(0.05));
}

TEST_CASE("Calibration I2: swapping the last block keeps the monitoring level within 0.5 dB through the real make-up path", "[autotrim][calibration][swap]") {
  const ChainCalibration c = calOn();
  for (const char* swapTo : {"lstm.nam", "wavenet.nam"}) {
    INFO("swap last block to " << swapTo);
    const Preset before = parse(hopRig("cal_pedal_a.nam", "wavenet.nam"));
    const Preset after = parse(hopRig("cal_pedal_a.nam", swapTo));
    const auto m = slotMakeup(before, after, 0, /*blockIndex=*/1, c);
    REQUIRE(m.has_value());
    CHECK_FALSE(m->skippedHop);  // the last block of the path keeps its make-up
    const Preset fixed = withSlotMakeup(after, 0, 1, m->makeupDb);
    const auto lb = measureReferenceLufs(before, nullptr, false, nullptr, c);
    const auto lf = measureReferenceLufs(fixed, nullptr, false, nullptr, c);
    REQUIRE((lb && lf));
    CHECK(std::fabs(*lf - *lb) < 0.5);
    if (std::string(swapTo) == "lstm.nam") {
      const auto lu = measureReferenceLufs(after, nullptr, false, nullptr, c);
      REQUIRE(lu.has_value());
      CHECK(std::fabs(*lu - *lb) > 1.0);  // without the make-up the swap does change the level
      CHECK(std::fabs(m->makeupDb) > 0.5);
    }
  }
}

TEST_CASE("Calibration I2: a make-up on a block that feeds a NAM is skipped, without rendering", "[autotrim][calibration][swap]") {
  const ChainCalibration c = calOn();
  const Preset before = parse(hopRig("cal_pedal_a.nam", "wavenet.nam"));
  const Preset after = parse(hopRig("cal_pedal_b.nam", "wavenet.nam"));
  CHECK(blockFeedsNam(after, 0, 0, c));
  CHECK_FALSE(blockFeedsNam(after, 0, 1, c));  // the amp is last
  CHECK_FALSE(blockFeedsNam(after, 0, 5, c));  // out of range
  CHECK_FALSE(blockFeedsNam(after, 1, 0, c));  // path B has a single block
  CHECK_FALSE(blockFeedsNam(after, 2, 0, c));
  CHECK_FALSE(blockFeedsNam(after, 0, -1, c));
  const auto m = slotMakeup(before, after, 0, 0, c);
  REQUIRE(m.has_value());
  CHECK(m->skippedHop);
  CHECK(m->makeupDb == 0.0);
  // It does not even need the path to be measurable: a set cancel flag does not turn the skip into nullopt.
  std::atomic<bool> cancel{true};
  const auto sk = slotMakeup(before, after, 0, 0, c, nullptr, &cancel);
  REQUIRE(sk.has_value());
  CHECK(sk->skippedHop);
  // The dB wrapper returns 0 for it.
  CHECK(slotMakeupDb(before, after, 0, nullptr, nullptr, c, 0) == 0.0);
  // Without a block index (or with calibration off) nothing is skipped: today's measurement.
  const auto noIdx = slotMakeup(before, after, 0, -1, c);
  REQUIRE(noIdx.has_value());
  CHECK_FALSE(noIdx->skippedHop);
  const auto off = slotMakeup(before, after, 0, 0, ChainCalibration{});
  REQUIRE(off.has_value());
  CHECK_FALSE(off->skippedHop);
  // A hop into a block with no input metadata is not planned, so it is not skipped.
  const Preset nometa = parse(hopRig("cal_pedal_b.nam", "cal_amp_nometa.nam"));
  CHECK_FALSE(blockFeedsNam(nometa, 0, 0, c));
}
