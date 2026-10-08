// v0.8 I1: input calibration wired through the Chain (docs/specs/v0_8-I1-chain_wiring.md).
//
// Linear identity fixtures with invented dBu metadata (tests/fixtures/nam/cal_*.nam, make_cal_fixtures.py) let a test read the
// gain a block applies straight from the rendered signal; wavenet.nam / lstm.nam (both input_level_dbu 18.3, output_level_dbu
// 12.3) are the nonlinear stand-ins. Everything here is synthetic.
#include <algorithm>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/capture_cache.h"
#include "sawblade/chain.h"
#include "sawblade/gate.h"
#include "sawblade/loudness.h"
#include "sawblade/reference_di.h"
#include "sawblade/render.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Approx;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;
const fs::path kNam = fs::path(SAWBLADE_FIXTURES_DIR) / "nam";
const fs::path kPresets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

json blk(const std::string& id, const char* file, double in = 0.0, double out = 0.0, bool norm = false, double makeup = 0.0) {
  json b = {{"id", id}, {"type", "nam"}, {"model", {{"file", (kNam / file).string()}}}};
  if (in != 0.0) b["inputGainDb"] = in;
  if (out != 0.0) b["outputGainDb"] = out;
  if (norm) b["normalizeLoudness"] = true;
  if (makeup != 0.0) b["makeupDb"] = makeup;
  return b;
}

// Path A = `a` (blend 0: only A is heard), path B disabled, no alignment, no cab.
json mk(std::vector<json> a, double inputDb = 0.0) {
  json pa = {{"blocks", json::array()}};
  for (auto& b : a) pa["blocks"].push_back(std::move(b));
  json pb = {{"enabled", false}, {"blocks", json::array({blk("b1", "linear_identity.nam")})}};
  json j = {{"schema", "sawblade.preset"}, {"version", 2}, {"name", "cal"},
            {"paths", {{"a", pa}, {"b", pb}}},
            {"align", {{"mode", "off"}}}, {"blend", 0.0},
            {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}};
  if (inputDb != 0.0) j["input"] = {{"gainDb", inputDb}};
  return j;
}

ChainCalibration cal(double deviceDbu) {
  ChainCalibration c;
  c.enabled = true;
  c.device.dbu = deviceDbu;
  return c;
}
ChainCalibration calAssumed() {
  ChainCalibration c;
  c.enabled = true;
  return c;
}

std::unique_ptr<Chain> build(const json& j, const ChainCalibration* c = nullptr, int maxBlock = 512) {
  const Preset p = parsePreset(j, kPresets);
  auto ch = std::make_unique<Chain>(p, loadResources(p, kFs));
  if (c) ch->setCalibration(*c);
  ch->prepare({kFs, maxBlock});
  return ch;
}

std::vector<float> run(Chain& c, const std::vector<float>& x, int block = 256) {
  std::vector<float> y(x.size());
  for (std::size_t pos = 0; pos < x.size();) {
    const auto n = std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - pos);
    c.process(x.data() + pos, y.data() + pos, static_cast<int>(n));
    pos += n;
  }
  return y;
}

// Gain (dB) of a chain on a 300 Hz sine, from the settled second half.
double measuredGainDb(Chain& c, double amp = 0.01) {
  const auto x = sine(300.0, kFs, 48000, amp);
  const auto y = run(c, x);
  return toDb(rms(y.data() + 24000, 24000) / rms(x.data() + 24000, 24000));
}
double gainOf(const json& j, const ChainCalibration* c = nullptr) {
  auto ch = build(j, c);
  return measuredGainDb(*ch);
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}

}  // namespace

// ---- 1. off is identical ---------------------------------------------------------------------------------------
TEST_CASE("Calibration I1: off (never set, set disabled, enabled then disabled) renders bit-identically", "[calibration][chain]") {
  const json j = mk({blk("a1", "wavenet.nam", 1.5, -2.0, true, 1.0), blk("a2", "lstm.nam", 0.0, 0.5, true)});
  const auto x = noise(24000, 3, 0.1f);
  auto ref = build(j);
  const auto yRef = run(*ref, x, 100);

  ChainCalibration off;  // enabled false, with a device set: still off
  off.device.dbu = 15.0;
  auto c1 = build(j, &off);
  SAWBLADE_REQUIRE_SAME_SAMPLES(yRef, run(*c1, x, 100));
  CHECK_FALSE(c1->calibrationPlan().enabled);

  // On, then off again before any audio: the plain gains come back exactly.
  auto c2 = build(j);
  c2->setCalibration(cal(15.0));
  c2->setCalibration(off);
  SAWBLADE_REQUIRE_SAME_SAMPLES(yRef, run(*c2, x, 100));

  // And the on render really differs (the test would be vacuous otherwise).
  auto c3 = build(j, nullptr);
  c3->setCalibration(cal(15.0));
  CHECK(maxAbsDiff(yRef, run(*c3, x, 100)) > 1e-3);
}

// ---- 2. single amp ----------------------------------------------------------------------------------------------
TEST_CASE("Calibration I1: single amp input gain = device - captureInputDbu + block.inputGainDb, measured by rendering", "[calibration][chain]") {
  // cal_amp_hi: input_level_dbu 12. Device 18 dBu: planned +6 dB. Block intent +2 dB.
  const json j = mk({blk("a1", "cal_amp_hi.nam", 2.0)});
  CHECK(gainOf(j) == Approx(2.0).margin(1e-3));  // off: the intent only
  const ChainCalibration c = cal(18.0);
  CHECK(gainOf(j, &c) == Approx(6.0 + 2.0).margin(1e-3));
  auto ch = build(j, &c);
  const CalibrationPlan& plan = ch->calibrationPlan();
  REQUIRE(plan.blocks[0].size() == 1);
  CHECK(plan.blocks[0][0].gainInDb == Approx(6.0).margin(1e-12));
  CHECK(plan.blocks[0][0].refBeforeDbu == 18.0);
  CHECK_FALSE(plan.blocks[0][0].feedsNam);
  CHECK_FALSE(plan.anyUncalibrated);
  CHECK_FALSE(plan.deviceAssumed);

  // The same on a nonlinear capture: calibration on == calibration off with the planned gain typed in as intent.
  const double planned = 10.0 - 18.3;  // device 10 dBu, wavenet.nam input_level_dbu 18.3
  const auto x = noise(24000, 5, 0.2f);
  const ChainCalibration c10 = cal(10.0);
  auto on = build(mk({blk("a1", "wavenet.nam", 1.5)}), &c10);
  auto manual = build(mk({blk("a1", "wavenet.nam", 1.5 + planned)}));
  CHECK(on->calibrationPlan().blocks[0][0].gainInDb == Approx(planned).margin(1e-12));
  CHECK(maxAbsDiff(run(*on, x, 64), run(*manual, x, 64)) < 1e-5);
}

TEST_CASE("Calibration I1: INPUT, the block intent and live changes add on top of the plan", "[calibration][chain]") {
  const ChainCalibration c = cal(18.0);
  auto ch = build(mk({blk("a1", "cal_amp_hi.nam", 2.0)}, /*INPUT*/ 3.0), &c);
  CHECK(measuredGainDb(*ch) == Approx(3.0 + 6.0 + 2.0).margin(1e-3));
  LiveParams lp = ch->liveParams();
  lp.blocks[0][0].inputGainDb = -4.0;  // the user/matcher moves the block's own gain: calibration is still added
  lp.inputGainDb = 1.0;
  ch->setLiveParams(lp);
  (void)measuredGainDb(*ch);  // let the 20 ms ramps finish
  CHECK(measuredGainDb(*ch) == Approx(1.0 + 6.0 - 4.0).margin(1e-3));
}

// ---- 3. amp swap ------------------------------------------------------------------------------------------------
TEST_CASE("Calibration I1: amp swap changes the planned gain by exactly the metadata difference", "[calibration][chain]") {
  // cal_amp_hi input 12 dBu, cal_amp_lo input 18 dBu: a 6.0 dB difference. (wavenet.nam and lstm.nam carry the same 18.3 dBu.)
  const ChainCalibration c = cal(15.0);
  const double hi = gainOf(mk({blk("a1", "cal_amp_hi.nam", 2.0)}, 3.0), &c);
  const double lo = gainOf(mk({blk("a1", "cal_amp_lo.nam", 2.0)}, 3.0), &c);
  CHECK(hi == Approx(3.0 + (15.0 - 12.0) + 2.0).margin(1e-3));  // INPUT and intent preserved
  CHECK(lo == Approx(3.0 + (15.0 - 18.0) + 2.0).margin(1e-3));
  CHECK(hi - lo == Approx(6.0).margin(1e-3));
  auto a = build(mk({blk("a1", "cal_amp_hi.nam")}), &c);
  auto b = build(mk({blk("a1", "cal_amp_lo.nam")}), &c);
  CHECK(a->calibrationPlan().blocks[0][0].gainInDb - b->calibrationPlan().blocks[0][0].gainInDb == 6.0);  // exactly
}

TEST_CASE("Calibration I1: wavenet <-> lstm swap: planned gain unchanged (same metadata), last block keeps normalise and make-up",
          "[calibration][chain]") {
  const ChainCalibration c = cal(12.0);
  auto w = build(mk({blk("a1", "wavenet.nam", 0.0, 0.0, true)}), &c);
  auto l = build(mk({blk("a1", "lstm.nam", 0.0, 0.0, true)}), &c);
  CHECK(w->calibrationPlan().blocks[0][0].gainInDb == l->calibrationPlan().blocks[0][0].gainInDb);
  CHECK(w->calibrationPlan().blocks[0][0].gainInDb == Approx(12.0 - 18.3).margin(1e-12));

  // The make-up flow of a capture swap still works on the last block with calibration on: measure the level before and after
  // (reference DI, BS.1770 as auto_trim.h does), fold the difference into makeupDb, and the loudness is back within 0.5 dB.
  const auto lufs = [&](const json& j) {
    auto ch = build(j, &c);
    const auto& di = referenceDi();
    std::vector<float> x(di.begin(), di.end());
    const auto y = run(*ch, x, 512);
    const std::vector<float> silent(y.size(), 0.0f);
    const auto v = integratedLoudnessLufs(y.data(), silent.data(), static_cast<std::int64_t>(y.size()), kFs);
    REQUIRE(v.has_value());
    return *v;
  };
  const double before = lufs(mk({blk("a1", "wavenet.nam", 0.0, 0.0, true)}));
  const double unmatched = lufs(mk({blk("a1", "lstm.nam", 0.0, 0.0, true)}));
  const double makeup = std::clamp(before - unmatched, -24.0, 24.0);
  INFO("before " << before << " LUFS, swapped without make-up " << unmatched << ", make-up " << makeup << " dB");
  CHECK(std::fabs(before - unmatched) > 1.0);  // the swap changed the level, so the make-up has work to do
  const double after = lufs(mk({blk("a1", "lstm.nam", 0.0, 0.0, true, makeup)}));
  CHECK(std::fabs(after - before) < 0.5);
}

TEST_CASE("Calibration I1: the gain is applied once: hops drop normalise and make-up, only the last block keeps them", "[calibration][chain]") {
  // linear_identity_loud24.nam: loudness -24 dB -> normalizeLoudness adds +6 dB. No calibration metadata (neutral, flagged).
  const json hop = mk({blk("a1", "linear_identity_loud24.nam", 0.0, 1.0, true, 5.0), blk("a2", "cal_amp_hi.nam")});
  // Off: today's behaviour, the pedal's output gain + make-up + normalise all count: 1 + 5 + 6.
  CHECK(gainOf(hop) == Approx(12.0).margin(1e-3));
  // On (device 12): the first block feeds a NAM: outputGainDb (1) stays, normalise (+6) and make-up (+5) are not applied.
  const ChainCalibration c = cal(12.0);
  CHECK(gainOf(hop, &c) == Approx(1.0).margin(1e-3));
  auto ch = build(hop, &c);
  CHECK(ch->calibrationPlan().blocks[0][0].feedsNam);
  CHECK_FALSE(ch->calibrationPlan().blocks[0][1].feedsNam);
  // The last block keeps both: 1 + 5 + 6 (no metadata: planned 0).
  const json last = mk({blk("a1", "linear_identity_loud24.nam", 0.0, 1.0, true, 5.0)});
  CHECK(gainOf(last, &c) == Approx(12.0).margin(1e-3));
  // Bypassed blocks do not take part: a bypassed NAM after the first leaves it as the last active NAM block.
  json byp = hop;
  byp["paths"]["a"]["blocks"][1]["bypass"] = true;
  CHECK(gainOf(byp, &c) == Approx(12.0).margin(1e-3));
}

// ---- 4. pedal -> amp hop ----------------------------------------------------------------------------------------
TEST_CASE("Calibration I1: pedal->amp hop = pedal.outputDbu - amp.inputDbu; a pedal swap moves the drive by the metadata difference only",
          "[calibration][chain]") {
  // pedal_a: in 6, out 10. pedal_b: in 6, out 4. amp_hi: in 12. Device 12.
  const ChainCalibration c = cal(12.0);
  const auto chainWith = [](const char* pedal) {
    return mk({blk("p1", pedal, 1.0, 2.0, /*norm*/ true, /*makeup*/ 3.0), blk("a1", "cal_amp_hi.nam", 0.5)});
  };
  auto a = build(chainWith("cal_pedal_a.nam"), &c);
  const auto& pa = a->calibrationPlan().blocks[0];
  CHECK(pa[0].gainInDb == Approx(12.0 - 6.0).margin(1e-12));
  CHECK(pa[1].refBeforeDbu == 10.0);                          // the pedal's output level
  CHECK(pa[1].gainInDb == Approx(10.0 - 12.0).margin(1e-12));  // pedal.outputDbu - amp.inputDbu
  // Rendered: pedal planned +6, intent in +1, out +2 (no make-up, no normalise); amp planned -2, intent +0.5.
  const double ga = measuredGainDb(*a);
  CHECK(ga == Approx(6.0 + 1.0 + 2.0 - 2.0 + 0.5).margin(1e-3));
  // Swap the pedal: the amp's drive changes by the output-level difference (10 - 4 = 6 dB) and nothing else, whatever the
  // swapped pedal's make-up says.
  auto b = build(chainWith("cal_pedal_b.nam"), &c);
  CHECK(b->calibrationPlan().blocks[0][1].gainInDb - pa[1].gainInDb == Approx(-6.0).margin(1e-12));
  CHECK(measuredGainDb(*b) - ga == Approx(-6.0).margin(1e-3));
  const json big = mk({blk("p1", "cal_pedal_b.nam", 1.0, 2.0, true, 17.0), blk("a1", "cal_amp_hi.nam", 0.5)});
  CHECK(gainOf(big, &c) == Approx(measuredGainDb(*b)).margin(1e-3));

  // Nonlinear amp: calibration on == off with the planned gains typed in as intent (pedal +6, amp 10 - 18.3).
  const auto x = noise(24000, 9, 0.2f);
  auto on = build(mk({blk("p1", "cal_pedal_a.nam"), blk("a1", "wavenet.nam")}), &c);
  auto manual = build(mk({blk("p1", "cal_pedal_a.nam", 6.0), blk("a1", "wavenet.nam", 10.0 - 18.3)}));
  CHECK(maxAbsDiff(run(*on, x, 64), run(*manual, x, 64)) < 1e-5);
}

TEST_CASE("Calibration I1: a live make-up edit on a hop block leaves the amp's input gain unchanged", "[calibration][chain]") {
  const ChainCalibration c = cal(12.0);
  const json j = mk({blk("p1", "cal_pedal_a.nam", 1.0, 2.0, false, 3.0), blk("a1", "cal_amp_hi.nam", 0.5)});
  auto on = build(j, &c);
  const double g0 = measuredGainDb(*on);
  CHECK(g0 == Approx(6.0 + 1.0 + 2.0 - 2.0 + 0.5).margin(1e-3));  // make-up (3) not applied on the hop
  LiveParams lp = on->liveParams();
  CHECK(lp.blocks[0][0].makeupDb == 3.0);
  lp.blocks[0][0].makeupDb = 9.0;  // the user/plugin edits the make-up live
  on->setLiveParams(lp);
  (void)measuredGainDb(*on);
  CHECK(measuredGainDb(*on) == Approx(g0).margin(1e-3));
  // Control, calibration off: the same edit does move the gain, by exactly the make-up difference.
  auto off = build(j);
  const double h0 = measuredGainDb(*off);
  lp = off->liveParams();
  lp.blocks[0][0].makeupDb = 9.0;
  off->setLiveParams(lp);
  (void)measuredGainDb(*off);
  CHECK(measuredGainDb(*off) - h0 == Approx(6.0).margin(1e-3));
  // The last block keeps a live make-up with calibration on.
  auto last = build(mk({blk("a1", "cal_amp_hi.nam", 0.0, 0.0, false, 3.0)}), &c);
  const double l0 = measuredGainDb(*last);
  lp = last->liveParams();
  lp.blocks[0][0].makeupDb = 7.0;
  last->setLiveParams(lp);
  (void)measuredGainDb(*last);
  CHECK(measuredGainDb(*last) - l0 == Approx(4.0).margin(1e-3));
}

TEST_CASE("Calibration I1: a hop into a block without input metadata keeps normalise and make-up", "[calibration][chain]") {
  const ChainCalibration c = cal(12.0);
  const json j = mk({blk("a1", "linear_identity_loud24.nam", 0.0, 1.0, true, 5.0), blk("a2", "cal_amp_nometa.nam")});
  CHECK(gainOf(j) == Approx(12.0).margin(1e-3));      // off: 1 + 5 + 6
  CHECK(gainOf(j, &c) == Approx(12.0).margin(1e-3));  // on: the same, the hop cannot be planned
  auto ch = build(j, &c);
  CHECK_FALSE(ch->calibrationPlan().blocks[0][0].feedsNam);
  CHECK(ch->calibrationPlan().blocks[0][1].inputMissing);
}

// ---- 5. missing metadata ----------------------------------------------------------------------------------------
TEST_CASE("Calibration I1: a capture without input_level_dbu is neutral and flagged; an uncalibrated device uses +12", "[calibration][chain]") {
  const ChainCalibration c = cal(15.0);
  const json j = mk({blk("a1", "cal_amp_nometa.nam", 2.0)});
  CHECK(gainOf(j, &c) == Approx(2.0).margin(1e-3));  // today's behaviour
  auto ch = build(j, &c);
  const auto& b = ch->calibrationPlan().blocks[0][0];
  CHECK(b.gainInDb == 0.0);
  CHECK(b.inputMissing);
  CHECK(b.outputMissing);
  CHECK_FALSE(b.captureInputDbu.has_value());
  CHECK(ch->calibrationPlan().anyUncalibrated);

  // Uncalibrated device: +12 is used, deviceAssumed set; amp_lo (input 18) -> -6 dB.
  const ChainCalibration assumed = calAssumed();
  const json lo = mk({blk("a1", "cal_amp_lo.nam")});
  auto d = build(lo, &assumed);
  CHECK(d->calibrationPlan().deviceAssumed);
  CHECK(d->calibrationPlan().deviceDbu == 12.0);
  CHECK(measuredGainDb(*d) == Approx(12.0 - 18.0).margin(1e-3));
  ChainCalibration nan = assumed;
  nan.device.dbu = std::nan("");
  CHECK(build(lo, &nan)->calibrationPlan().deviceAssumed);

  // A missing output (input present) is flagged on its side only.
  CHECK_FALSE(build(mk({blk("a1", "cal_amp_hi.nam")}), &c)->calibrationPlan().anyUncalibrated);
}

// ---- report -----------------------------------------------------------------------------------------------------
TEST_CASE("Calibration I1: the render report carries the calibration object (additive), off and on", "[calibration][render]") {
  const Preset p = parsePreset(mk({blk("p1", "cal_pedal_a.nam"), blk("a1", "cal_amp_nometa.nam")}), kPresets);
  AudioFile in;
  in.sampleRate = kFs;
  in.channels = 1;
  in.interleaved = noise(4800, 2, 0.1f);

  RenderOptions o;
  o.renderRate = kFs;
  const json off = reportJson(renderPreset(p, in, o))["calibration"];
  CHECK_FALSE(off["enabled"].get<bool>());
  CHECK(off["deviceAssumed"].get<bool>());
  CHECK(off["paths"]["a"].empty());

  o.calibration.enabled = true;  // no device: +12 assumed
  const json rep = reportJson(renderPreset(p, in, o));
  const json& on = rep["calibration"];
  CHECK(on["enabled"].get<bool>());
  CHECK(on["deviceDbu"].get<double>() == 12.0);
  CHECK(on["deviceAssumed"].get<bool>());
  CHECK(on["anyUncalibrated"].get<bool>());
  REQUIRE(on["paths"]["a"].size() == 2);
  const json& pedal = on["paths"]["a"][0];
  CHECK(pedal["id"] == "p1");
  CHECK(pedal["gainInDb"].get<double>() == Approx(12.0 - 6.0));
  CHECK(pedal["captureInputDbu"].get<double>() == 6.0);
  CHECK(pedal["captureOutputDbu"].get<double>() == 10.0);
  CHECK_FALSE(pedal["inputMissing"].get<bool>());
  const json& amp = on["paths"]["a"][1];
  CHECK(amp["inputMissing"].get<bool>());
  CHECK(amp["outputMissing"].get<bool>());
  CHECK(amp["captureInputDbu"].is_null());
  CHECK(amp["gainInDb"].get<double>() == 0.0);
  CHECK(on["paths"]["b"].empty());  // path B is disabled

  // Calibration really changed the render (pedal +6 dB), and off equals the no-option render.
  RenderOptions plain;
  plain.renderRate = kFs;
  const auto offR = renderPreset(p, in, plain);
  o.calibration.device.dbu = 12.0;
  const auto onR = renderPreset(p, in, o);
  CHECK(maxAbsDiff(offR.samples, onR.samples) > 0.01);
}

// ---- 6. gain ladder ---------------------------------------------------------------------------------------------
namespace {
struct CacheDir {
  fs::path dir;
  CacheDir() {
    static int n = 0;
    dir = fs::temp_directory_path() / ("sawblade_cal_cache_" + std::to_string(std::random_device{}()) + "_" + std::to_string(n++));
    fs::create_directories(dir / "T1");
    setCaptureCacheRootOverride(dir);
  }
  ~CacheDir() {
    setCaptureCacheRootOverride(std::nullopt);
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void put(const std::string& model, const char* fixture) const { fs::copy_file(kNam / fixture, dir / "T1" / (model + ".nam"), fs::copy_options::overwrite_existing); }
};

// Path A = one ladder block: rung m1 (gain 2, position 0, own capture = cal_amp_hi, input 12 dBu), rung m2 (gain 8, position 10,
// cal_amp_lo, input 18 dBu).
json ladderPreset() {
  json model = {{"file", (kNam / "cal_amp_hi.nam").string()},
                {"source", {{"provider", "tone3000"}, {"id", "T1"}, {"modelId", "m1"}}},
                {"ladder", json::array({{{"modelId", "m1"}, {"gain", 2.0}, {"name", "Gain 2"}},
                                        {{"modelId", "m2"}, {"gain", 8.0}, {"name", "Gain 8"}}})}};
  json j = mk({blk("a1", "cal_amp_hi.nam")});
  j["paths"]["a"]["blocks"][0] = {{"id", "a1"}, {"type", "nam"}, {"slot", "amp"}, {"model", model}};
  return j;
}

void publishRung(Chain& c, const Preset& p, int rung) {
  const auto& nam = static_cast<const NamBlockParams&>(*p.a.blocks[0].params);
  std::string why;
  auto proc = buildRungProcessor(nam, rung, {kFs, 512}, nullptr, &why);
  INFO(why);
  REQUIRE(proc != nullptr);
  std::vector<LadderBlock::Entry> es;
  es.push_back({rung, std::move(proc)});
  REQUIRE(c.ladderBlock(0) != nullptr);
  c.ladderBlock(0)->publishRungs(std::move(es));
}
}  // namespace

TEST_CASE("Calibration I1: a gain-ladder rung swap changes the planned gain by the rungs' metadata difference", "[calibration][ladder]") {
  const CacheDir cache;
  cache.put("m1", "cal_amp_hi.nam");
  cache.put("m2", "cal_amp_lo.nam");
  const json j = ladderPreset();
  const Preset p = parsePreset(j, kPresets);
  const ChainCalibration c = cal(15.0);
  auto ch = build(j, &c);
  CHECK(measuredGainDb(*ch) == Approx(15.0 - 12.0).margin(1e-3));  // rung m1: 15 - 12
  publishRung(*ch, p, 1);
  LiveParams lp = ch->liveParams();
  lp.amp[0].gain = 10.0;  // the knob asks for rung m2 (position 10): no residual drive
  ch->setLiveParams(lp);
  (void)measuredGainDb(*ch);  // hand-over, warm-up and crossfade
  REQUIRE(ch->ladderState(0).active == 1);
  CHECK(measuredGainDb(*ch) == Approx(15.0 - 18.0).margin(1e-3));  // rung m2: 15 - 18, each rung from its own metadata
  // Back down.
  lp.amp[0].gain = 0.0;
  ch->setLiveParams(lp);
  (void)measuredGainDb(*ch);
  CHECK(measuredGainDb(*ch) == Approx(15.0 - 12.0).margin(1e-3));
}

// ---- 7. real time -----------------------------------------------------------------------------------------------
TEST_CASE("Calibration I1: process() allocates nothing with calibration on, including during a plan swap and a rung swap", "[calibration][rt]") {
  const CacheDir cache;
  cache.put("m1", "cal_amp_hi.nam");
  cache.put("m2", "cal_amp_lo.nam");
  const json j = ladderPreset();
  const Preset p = parsePreset(j, kPresets);
  const ChainCalibration c = cal(15.0);
  auto ch = build(j, &c);
  publishRung(*ch, p, 1);  // consumed inside the guard
  const auto x = noise(512, 2);
  std::vector<float> y(512);
  LiveParams lp = ch->liveParams();
  ch->publishCalibration(cal(9.0));  // planned here (producer), applied inside the guard
  {
    AllocGuard g;
    for (int i = 0; i < 300; ++i) {
      lp.amp[0].gain = 5.0 + 5.0 * std::sin(i * 0.05);  // sweeps across the rungs
      ch->setLiveParams(lp);
      ch->process(x.data(), y.data(), 1 + (i * 37) % 512);
    }
    CHECK(g.count() == 0);
  }
  for (float v : y) REQUIRE(std::isfinite(v));

  // A chain of NAMs (wavenet -> lstm hop) with plan swaps from a producer thread racing process().
  auto hop = build(mk({blk("a1", "wavenet.nam"), blk("a2", "lstm.nam", 0.0, 0.0, true, 2.0)}), &c);
  std::atomic<bool> stop{false};
  std::thread producer([&] {
    for (int i = 0; !stop.load(); ++i) {
      ChainCalibration cc = cal(6.0 + (i % 5));
      if (i % 7 == 0) cc.enabled = false;
      hop->publishCalibration(cc);
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
  });
  {
    AllocGuard g;
    for (int i = 0; i < 400; ++i) hop->process(x.data(), y.data(), 1 + (i * 53) % 512);
    CHECK(g.count() == 0);
  }
  stop = true;
  producer.join();
  for (float v : y) REQUIRE(std::isfinite(v));
}

TEST_CASE("Calibration I1: the result does not depend on the block size", "[calibration][chain]") {
  const ChainCalibration c = cal(12.0);
  const json lin = mk({blk("p1", "cal_pedal_a.nam", 1.0, 2.0), blk("a1", "cal_amp_hi.nam", 0.5)});
  const json nl = mk({blk("a1", "wavenet.nam", 0.5), blk("a2", "lstm.nam", 0.0, 0.0, true, 1.0)});
  const auto x = noise(30000, 4, 0.2f);
  for (const json* j : {&lin, &nl}) {
    auto ref = build(*j, &c, 512);
    const auto yRef = run(*ref, x, 512);
    for (int bs : {1, 7, 64, 333}) {
      auto ch = build(*j, &c, 512);
      INFO("block size " << bs);
      CHECK(maxAbsDiff(yRef, run(*ch, x, bs)) < 1e-6);
    }
  }
  // A plan published at the start of a block ramps over 20 ms by absolute samples: same result for any later split.
  const auto once = [&](int bs) {
    auto ch = build(lin, &c, 512);
    std::vector<float> y(x.size());
    ch->process(x.data(), y.data(), 512);  // first block with the old plan
    ch->publishCalibration(cal(6.0));
    for (std::size_t pos = 512; pos < x.size();) {
      const auto n = std::min<std::size_t>(static_cast<std::size_t>(bs), x.size() - pos);
      ch->process(x.data() + pos, y.data() + pos, static_cast<int>(n));
      pos += n;
    }
    return y;
  };
  CHECK(maxAbsDiff(once(512), once(37)) < 1e-6);
}

// ---- 8. SYNTHETIC stand-in for the user's matched chain ---------------------------------------------------------
namespace {
struct Sweep {
  std::vector<double> inDb, outDb;
  double slope = 0.0;
};
Sweep sweep(Chain& ch) {
  Sweep s;
  for (int k = 0; k < 7; ++k) {  // 18 dB in 3 dB steps, sine RMS
    const double lvl = -30.0 + 3.0 * k;
    const double amp = std::sqrt(2.0) * std::pow(10.0, lvl / 20.0);
    const auto x = sine(220.0, kFs, 48000, amp);
    ch.reset();
    const auto y = run(ch, x);
    s.inDb.push_back(lvl);
    s.outDb.push_back(toDb(rms(y.data() + 24000, 24000)));
  }
  double mx = 0, my = 0;
  for (std::size_t i = 0; i < s.inDb.size(); ++i) { mx += s.inDb[i]; my += s.outDb[i]; }
  mx /= 7.0; my /= 7.0;
  double sxy = 0, sxx = 0;
  for (std::size_t i = 0; i < s.inDb.size(); ++i) { sxy += (s.inDb[i] - mx) * (s.outDb[i] - my); sxx += (s.inDb[i] - mx) * (s.inDb[i] - mx); }
  s.slope = sxy / sxx;
  return s;
}
}  // namespace

TEST_CASE("Calibration I1 SYNTHETIC - redo on the user's L_ubr_quick preset: pedal NAM -> amp NAM stand-in", "[calibration][report]") {
  // wavenet.nam as the front (pedal) stage, lstm.nam as the amp; both carry input 18.3 / output 12.3 dBu. Device: the assumed
  // +12 dBu (the user's interface is not calibrated yet). normalizeLoudness / make-up off so on and off differ only by calibration.
  const json both = mk({blk("pedal", "wavenet.nam"), blk("amp", "lstm.nam")});
  const json pedalOnly = mk({blk("pedal", "wavenet.nam")});
  const ChainCalibration c = calAssumed();

  auto off = build(both);
  auto on = build(both, &c);
  const auto& plan = on->calibrationPlan();
  REQUIRE(plan.blocks[0].size() == 2);
  const double gPedal = plan.blocks[0][0].gainInDb, gAmp = plan.blocks[0][1].gainInDb;
  CHECK(gPedal == Approx(12.0 - 18.3).margin(1e-12));
  CHECK(gAmp == Approx(12.3 - 18.3).margin(1e-12));  // pedal.outputDbu - amp.inputDbu

  // Level at each model's input and the offset against its capture's calibrated drive. The ideal digital level at a model's
  // input is (analog level there, dBu) - input_level_dbu; the offset is the gain the block applies minus the gain that would
  // put the signal at exactly that level: offset = gainApplied - (refBefore - input_level_dbu).
  const auto offsetRe = [](double applied, double ref, double captureIn) { return applied - (ref - captureIn); };
  const double pedalOffOff = offsetRe(0.0, 12.0, 18.3), pedalOffOn = offsetRe(gPedal, 12.0, 18.3);
  const double ampOffOff = offsetRe(0.0, 12.3, 18.3), ampOffOn = offsetRe(gAmp, 12.3, 18.3);

  // The amp's input level, by rendering: the pedal-only chain's output level times the amp's input gain.
  const auto x = sine(220.0, kFs, 48000, std::sqrt(2.0) * std::pow(10.0, -24.0 / 20.0));  // -24 dBFS RMS
  const auto pedalOut = [&](const ChainCalibration* cc) {
    auto ch = build(pedalOnly, cc);
    const auto y = run(*ch, x);
    return toDb(rms(y.data() + 24000, 24000));
  };
  const double ampInOff = pedalOut(nullptr) + 0.0, ampInOn = pedalOut(&c) + gAmp;
  const double driveMove = ampInOn - ampInOff;

  const Sweep sOff = sweep(*off), sOn = sweep(*on);
  // Sanity on the rendering: the whole-chain render with calibration equals the manual-gain render without it.
  auto manual = build(mk({blk("pedal", "wavenet.nam", gPedal), blk("amp", "lstm.nam", gAmp)}));
  const auto xs = noise(24000, 11, 0.2f);
  auto on2 = build(both, &c);
  CHECK(maxAbsDiff(run(*on2, xs, 64), run(*manual, xs, 64)) < 1e-5);
  // The planned drive change and the measured one agree.
  CHECK(driveMove == Approx(gAmp + (pedalOut(&c) - pedalOut(nullptr))).margin(1e-9));

  std::printf("\nI1 SYNTHETIC stand-in (redo on the user's L_ubr_quick preset): wavenet.nam (pedal stage) -> lstm.nam (amp), device +12 dBu (assumed)\n");
  std::printf("  block   capture in/out dBu   planned gain (on)   offset re capture drive, off -> on (dB)\n");
  std::printf("  pedal   18.3 / 12.3          %+6.2f dB           %+6.2f -> %+6.2f\n", gPedal, pedalOffOff, pedalOffOn);
  std::printf("  amp     18.3 / 12.3          %+6.2f dB           %+6.2f -> %+6.2f\n", gAmp, ampOffOff, ampOffOn);
  std::printf("  amp input level at pedal-in -24 dBFS RMS: off %.2f dBFS, on %.2f dBFS, moved %+.2f dB\n", ampInOff, ampInOn, driveMove);
  std::printf("  sweep (sine 220 Hz, -30..-12 dBFS RMS in 3 dB steps) out dBFS, off:");
  for (double v : sOff.outDb) std::printf(" %.2f", v);
  std::printf("\n  sweep out dBFS, on: ");
  for (double v : sOn.outDb) std::printf(" %.2f", v);
  std::printf("\n  slope (dB out per dB in), least squares over 18 dB: off %.3f, on %.3f\n", sOff.slope, sOn.slope);
  std::printf("  amp drive moved by %.2f dB: %s\n", std::fabs(driveMove), std::fabs(driveMove) >= 3.0 ? ">= 3 dB" : "less than 3 dB");
  // The numbers in the REPORT (docs/specs/v0_8-input_calibration_REPORT.md, I1): pinned so it cannot drift from the code.
  CHECK(gPedal == Approx(-6.30).margin(1e-9));
  CHECK(gAmp == Approx(-6.00).margin(1e-9));
  CHECK(driveMove == Approx(-12.16).margin(0.02));
  CHECK(std::fabs(driveMove) >= 3.0);
  CHECK(sOff.slope == Approx(0.486).margin(0.005));
  CHECK(sOn.slope == Approx(0.111).margin(0.005));
  CHECK(sOff.slope > sOn.slope);
}

// ---- 9. gate -----------------------------------------------------------------------------------------------------
namespace {
std::vector<float> gaussianNoise(double rmsDb, std::size_t n, std::uint32_t seed) {
  std::mt19937 g(seed);
  std::normal_distribution<double> d(0.0, std::pow(10.0, rmsDb / 20.0));
  std::vector<float> x(n);
  for (auto& v : x) v = static_cast<float>(d(g));
  return x;
}
// Fraction of 10 ms windows (after 0.5 s) in which the gate passes the noise (window gain, with the known linear calibration gain
// divided out, above -3 dB). The chain here is a linear identity (+ the calibration gain), so y/x is the gate's gain times it.
double openFraction(const std::vector<float>& x, const std::vector<float>& y, double calLinear) {
  const std::size_t w = 480;
  std::size_t open = 0, total = 0;
  for (std::size_t s = 24000; s + w <= x.size(); s += w, ++total) {
    double xy = 0, xx = 0;
    for (std::size_t i = s; i < s + w; ++i) { xy += static_cast<double>(x[i]) * y[i]; xx += static_cast<double>(x[i]) * x[i]; }
    if (xy / xx / calLinear > 0.708) ++open;
  }
  return static_cast<double>(open) / static_cast<double>(total);
}
}  // namespace

TEST_CASE("Calibration I1: calibration does not open the gate on a -49.5 dBFS DI noise floor", "[calibration][gate][report]") {
  const auto x = gaussianNoise(-49.5, 10 * 48000, 17);
  const double floorDb = peakFloorDb(x, kFs);  // v0.4M Task H.1: the 92.5th percentile of the gate's own peak envelope
  const json gate = {{"enabled", true}, {"thresholdDb", floorDb + 10.0}, {"hysteresisDb", 6.0}};  // the matcher's default record cell
  json j = mk({blk("a1", "cal_amp_hi.nam")});
  j["gate"] = gate;
  // Device 24 dBu with cal_amp_hi (input 12 dBu): planned +12 dB. A gate keyed AFTER that gain would see a peak floor near
  // -30 dBFS, above the open threshold (floor + 10 = about -32 dBFS), and open.
  const ChainCalibration c = cal(24.0);
  auto off = build(j);
  auto on = build(j, &c);
  CHECK(on->calibrationPlan().blocks[0][0].gainInDb == Approx(12.0));
  const auto yOff = run(*off, x), yOn = run(*on, x);
  const double lin12 = std::pow(10.0, 12.0 / 20.0);
  const double fOff = openFraction(x, yOff, 1.0), fOn = openFraction(x, yOn, lin12);
  // Control: the same +12 dB as INPUT, which sits before the gate, does open it.
  json jc = mk({blk("a1", "cal_amp_hi.nam")}, 12.0);
  jc["gate"] = gate;
  auto ctl = build(jc);
  const double fCtl = openFraction(x, run(*ctl, x), lin12);
  std::printf("\nI1 gate check: noise -49.5 dBFS RMS (Gaussian, 10 s), peakFloor %.2f dBFS, open %.2f / close %.2f dBFS\n", floorDb,
              floorDb + 10.0, floorDb + 4.0);
  std::printf("  gate open fraction on noise-only (after 0.5 s): calibration off %.4f, calibration on (+12 dB planned) %.4f, control INPUT +12 dB (before the gate) %.4f\n",
              fOff, fOn, fCtl);
  CHECK(floorDb == Approx(-42.3).margin(0.5));
  CHECK(fOn == fOff);  // the gate is keyed on the DI before any calibration gain
  CHECK(fOff < 0.05);
  CHECK(fCtl > 0.5);  // discriminating: a gain before the gate would have opened it
}

TEST_CASE("Calibration I1: a downstream NAM block does not follow a rung swap of a ladder upstream", "[calibration][ladder]") {
  const CacheDir cache;
  cache.put("m1", "cal_pedal_a.nam");  // output 10 dBu
  cache.put("m2", "cal_pedal_b.nam");  // output 4 dBu
  json j = ladderPreset();
  j["paths"]["a"]["blocks"][0]["model"]["file"] = (kNam / "cal_pedal_a.nam").string();
  j["paths"]["a"]["blocks"].push_back(blk("a2", "cal_amp_hi.nam"));
  const Preset p = parsePreset(j, kPresets);
  const ChainCalibration c = cal(12.0);
  auto ch = build(j, &c);
  const double before = measuredGainDb(*ch);
  CHECK(ch->calibrationPlan().blocks[0][1].refBeforeDbu == 10.0);
  publishRung(*ch, p, 1);
  LiveParams lp = ch->liveParams();
  lp.amp[0].gain = 10.0;
  ch->setLiveParams(lp);
  (void)measuredGainDb(*ch);
  REQUIRE(ch->ladderState(0).active == 1);
  CHECK(measuredGainDb(*ch) == Approx(before).margin(1e-3));  // documented limit: the amp stays planned from the starting rung's 10 dBu
}
