// v0.3 Task B: level-matched auditioning in core - the reference DI, the trim and its staleness hash, the schema (v3), the chain's
// trim gain, the capture-swap make-up, and the acceptance tests over the committed presets.
#include <algorithm>
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
json twoPaths(const std::string& aFile, const std::string& bFile, double blend = 0.5) {
  return {{"schema", "sawblade.preset"}, {"version", 3}, {"name", "trim"},
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

// ---- schema v3 ----------------------------------------------------------------------------------
TEST_CASE("Preset v3: output.autoTrimDb / autoTrimHash and nam makeupDb round-trip; v1 and v2 files still read", "[autotrim][preset]") {
  json j = twoPaths("linear_identity.nam", "linear_identity.nam");
  for (int ver : {1, 2}) {
    j["version"] = ver;
    const Preset p = parse(j);
    CHECK(p.autoTrimHash.empty());
    CHECK(p.autoTrimDb == 0.0);
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
  CHECK(p.autoTrimDb == -7.5);
  CHECK(p.autoTrimHash == "abc");
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
  CHECK(q.autoTrimHash.empty());
  CHECK(q.autoTrimDb == 0.0);
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
    p.autoTrimDb = -3.0;
    p.autoTrimHash = "stale";
    CHECK(autoTrimHash(p) == h);
  }
  const auto differs = [&](const std::function<void(json&)>& edit) {
    json j = twoPaths("linear_identity.nam", "linear_05_025.nam");
    edit(j);
    return autoTrimHash(parse(j)) != h;
  };
  CHECK(differs([](json& j) { j["blend"] = 0.6; }));
  CHECK(differs([](json& j) { j["output"] = {{"gainDb", -1.0}}; }));
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
  CHECK(p.autoTrimDb == Approx(kAutoTrimTargetLufs - *raw).margin(1e-9));
  CHECK(lufsWithTrim(p) == Approx(kAutoTrimTargetLufs).margin(0.01));
  // Not applied unless asked for: the default render ignores the stored trim, bit for bit.
  AudioFile in{kFs, 1, referenceDi()};
  const RenderResult plain = renderPreset(p, in);
  Preset noTrim = p;
  noTrim.autoTrimDb = 0.0;
  noTrim.autoTrimHash.clear();
  const RenderResult plain2 = renderPreset(noTrim, in);
  SAWBLADE_REQUIRE_SAME_SAMPLES(plain2.samples, plain.samples);
  CHECK(plain.autoTrimDb == 0.0);
  RenderOptions o;
  o.applyAutoTrim = true;
  const RenderResult trimmed = renderPreset(p, in, o);
  CHECK(trimmed.autoTrimDb == p.autoTrimDb);
  CHECK(lufsOfSamples(trimmed.samples) - lufsOfSamples(plain.samples) == Approx(p.autoTrimDb).margin(0.01));
  // An edit that changes the level makes it stale; ensureAutoTrim measures again.
  p.blend = 0.9;
  CHECK_FALSE(autoTrimFresh(p));
  const double old = p.autoTrimDb;
  REQUIRE(ensureAutoTrim(p));
  CHECK(autoTrimFresh(p));
  CHECK(lufsWithTrim(p) == Approx(kAutoTrimTargetLufs).margin(0.01));
  CHECK(p.autoTrimDb != old);
  // The OUTPUT knob is on top of the match: the preset as stored (its gain included) plays at the target.
  Preset g = parse(twoPaths("linear_05_025.nam", "linear_identity.nam"));
  g.outputGainDb = -4.0;
  REQUIRE(ensureAutoTrim(g));
  CHECK(lufsWithTrim(g) == Approx(kAutoTrimTargetLufs).margin(0.01));
}

TEST_CASE("Auto trim: a silent preset has no trim", "[autotrim]") {
  json j = twoPaths("linear_identity.nam", "linear_identity.nam");
  j["input"] = {{"gainDb", -120.0}};
  Preset p = parse(j);
  CHECK_FALSE(computeAutoTrim(p).has_value());
  CHECK_FALSE(stampAutoTrim(p));
  CHECK(p.autoTrimHash.empty());
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
  Preset a = parse(twoPaths("linear_identity.nam", "linear_identity.nam", 0.0));
  Preset b = parse(twoPaths("wavenet.nam", "linear_05_025.nam", 0.7));
  b.outputGainDb = 9.0;
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
  if (!p.autoTrimHash.empty()) {
    // A stored trim must belong to the preset as committed: edit a preset, rerun scripts/compute_trims.py.
    CHECK(autoTrimFresh(p));
  }
  CaptureCache cache;
  REQUIRE(ensureAutoTrim(p, &cache));  // a preset without a stored trim is measured, as the plugin does at load
  const double lu = lufsWithTrim(p, &cache);
  INFO("trim " << p.autoTrimDb << " dB, loudness with trim " << lu << " LUFS");
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
// level so that platforms whose libm rounds differently (macOS) are still checked, to 0.01 dB.
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
#if defined(__linux__) && defined(__x86_64__)
    CHECK(sampleHash(r.samples) == want["sha256"].get<std::string>());
#else
    CHECK(r.output.rmsDbfs == Approx(want["rmsDbfs"].get<double>()).margin(0.01));
    CHECK(r.output.peakDbfs == Approx(want["peakDbfs"].get<double>()).margin(0.01));
#endif
    ++checked;
  }
  CHECK(checked >= 30);
}
