// v0.8 I4a: preset schema v5 (calibration.mode, calibrated trim), calibration following the preset in the render path,
// tonerender --device-dbu / --di-channel, and the offline stereo-DI rule (docs/specs/v0_8-I4-default_on.md).
//
// Synthetic only: the linear identity fixtures with invented dBu metadata (tests/fixtures/nam/cal_*.nam). cal_pedal_a has
// input_level_dbu 6, so at the assumed +12 dBu device the planned gain is +6 dB and at a +18 dBu device +12 dB.
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
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "sawblade/auto_trim.h"
#include "sawblade/capture_cache.h"
#include "sawblade/render.h"
#include "sawblade/wav_io.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;
const fs::path kNam = fs::path(SAWBLADE_FIXTURES_DIR) / "nam";
const fs::path kPresets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

struct TempDir {
  fs::path dir;
  TempDir() {
    static int counter = 0;
    dir = fs::temp_directory_path() / ("sawblade_i4a_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  fs::path operator/(const std::string& f) const { return dir / f; }
};

std::string q(const fs::path& p) { return "'" + p.string() + "'"; }
std::string slurp(const fs::path& p) {
  std::ifstream f(p);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
int runCli(const std::string& args, const fs::path& errFile) {
  const std::string cmd = q(SAWBLADE_TONERENDER_EXE) + " " + args + " >/dev/null 2>" + q(errFile);
  const int st = std::system(cmd.c_str());
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
void writeText(const fs::path& p, const std::string& s) { std::ofstream(p) << s; }

// Pedal NAM (in 6 dBu / out 10 dBu) -> amp NAM (in 12 dBu / out 0 dBu) on path A; path B off. Version 5 with the given mode
// ("" = no calibration member).
json mkPreset(const std::string& mode, int version = 5, double blend = 0.0) {
  const auto blk = [](const std::string& id, const char* file) {
    return json{{"id", id}, {"type", "nam"}, {"model", {{"file", (kNam / file).string()}}}};
  };
  json j = {{"schema", "sawblade.preset"}, {"version", version}, {"name", "i4a"},
            {"paths", {{"a", {{"blocks", json::array({blk("p1", "cal_pedal_a.nam"), blk("a1", "cal_amp_hi.nam")})}}},
                       {"b", {{"enabled", false}, {"blocks", json::array({blk("b1", "linear_identity.nam")})}}}}},
            {"align", {{"mode", "off"}}}, {"blend", blend},
            {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}};
  if (!mode.empty()) j["calibration"] = {{"mode", mode}};
  return j;
}
Preset parse(const json& j) { return parsePreset(j, kPresets); }

AudioFile monoFile(const std::vector<float>& x) { return AudioFile{kFs, 1, x}; }
AudioFile stereoFile(const std::vector<float>& l, const std::vector<float>& r) {
  AudioFile a;
  a.sampleRate = kFs;
  a.channels = 2;
  a.interleaved.resize(l.size() * 2);
  for (std::size_t i = 0; i < l.size(); ++i) {
    a.interleaved[2 * i] = l[i];
    a.interleaved[2 * i + 1] = r[i];
  }
  return a;
}
RenderOptions opts(int block = 256) {
  RenderOptions o;
  o.renderRate = kFs;
  o.blockSize = block;
  o.calibrationFromPreset = true;
  return o;
}

}  // namespace

// ---- schema v5 -----------------------------------------------------------------------------------------------------
TEST_CASE("I4a schema v5: calibration.mode and the calibrated trim round-trip", "[i4a][preset]") {
  CHECK(kPresetVersion == 5);
  Preset p = parse(mkPreset("calibrated"));
  CHECK(p.calibrationMode == CalibrationMode::Calibrated);
  p.autoTrim.db = -3.5;
  p.autoTrim.hash = "aaaa";
  p.autoTrimCal.db = -9.25;
  p.autoTrimCal.hash = "bbbb";
  const json out = toJson(p);
  CHECK(out["version"] == 5);
  CHECK(out["calibration"]["mode"] == "calibrated");
  CHECK(out["output"]["autoTrimDb"] == -3.5);
  CHECK(out["output"]["autoTrimCalDb"] == -9.25);
  CHECK(out["output"]["autoTrimCalHash"] == "bbbb");
  const Preset back = parse(out);
  CHECK(back.calibrationMode == CalibrationMode::Calibrated);
  CHECK(back.autoTrim.db == -3.5);
  CHECK(back.autoTrimCal.db == -9.25);
  CHECK(back.autoTrimCal.hash == "bbbb");
  CHECK(back == p);
  CHECK(toJson(back) == out);  // writing is stable

  const Preset leg = parse(mkPreset("legacy"));
  CHECK(leg.calibrationMode == CalibrationMode::Legacy);
  const json lo = toJson(leg);
  CHECK(lo["calibration"]["mode"] == "legacy");
  CHECK_FALSE(lo["output"].contains("autoTrimCalDb"));  // not measured: not written
  CHECK_FALSE(lo["output"].contains("autoTrimCalHash"));
  CHECK_FALSE(leg == p);  // the mode is an edit; the stamps are not

  // A trim without its hash cannot be checked: read as not measured (like autoTrimDb).
  json noHash = mkPreset("calibrated");
  noHash["output"] = {{"autoTrimCalDb", -4.0}};
  CHECK(parse(noHash).autoTrimCal.hash.empty());
  json bigTrim = mkPreset("calibrated");
  bigTrim["output"] = {{"autoTrimCalDb", 99.0}, {"autoTrimCalHash", "x"}};
  CHECK_THROWS_AS(parse(bigTrim), PresetError);
}

TEST_CASE("I4a schema v5: v1-v4 files load as legacy, a v5 file with no member is legacy, bad values are rejected", "[i4a][preset]") {
  for (int ver : {1, 2, 3, 4}) {
    INFO("version " << ver);
    const Preset p = parse(mkPreset("", ver));
    CHECK(p.calibrationMode == CalibrationMode::Legacy);
    const json o = toJson(p);
    CHECK(o["version"] == kPresetVersion);  // re-written as the current version, still legacy
    CHECK(o["calibration"]["mode"] == "legacy");
    CHECK(parse(o).calibrationMode == CalibrationMode::Legacy);
    // Even a (hand-written) member in an old file does not switch calibration on.
    CHECK(parse(mkPreset("calibrated", ver)).calibrationMode == CalibrationMode::Legacy);
  }
  CHECK(parse(mkPreset("", 5)).calibrationMode == CalibrationMode::Legacy);
  CHECK(Preset{}.calibrationMode == CalibrationMode::Legacy);  // until the I4c flip

  json bad = mkPreset("sometimes");
  try {
    parse(bad);
    FAIL("expected PresetError");
  } catch (const PresetError& e) {
    CHECK(e.jsonPath() == "calibration.mode");
  }
  bad = mkPreset("calibrated");
  bad["calibration"]["deviceDbu"] = 12;  // device levels are never stored in a preset
  CHECK_THROWS_AS(parse(bad), PresetError);
  bad = mkPreset("calibrated");
  bad["version"] = kPresetVersion + 1;
  CHECK_THROWS_AS(parse(bad), PresetError);
}

// ---- calibrated trim and staleness ---------------------------------------------------------------------------------
TEST_CASE("I4a trim: the calibrated trim is measured at the assumed +12 dBu, is independent of the legacy one, and goes stale with the rig", "[i4a][autotrim]") {
  Preset p = parse(mkPreset("calibrated"));
  REQUIRE(ensureAutoTrim(p));
  const double legacyDb = p.autoTrim.db;
  const std::string legacyHash = p.autoTrim.hash;
  CHECK_FALSE(autoTrimCalFresh(p));
  REQUIRE(stampAutoTrimCal(p));
  CHECK(autoTrimCalFresh(p));
  CHECK(p.autoTrim.db == legacyDb);  // the two stamps are independent
  CHECK(p.autoTrim.hash == legacyHash);
  CHECK(p.autoTrimCal.hash != p.autoTrim.hash);

  // The pedal is planned at +12 - 6 = +6 dB, so the calibrated chain is 6 dB hotter and its trim 6 dB lower (the amp, 12 dBu in
  // with the pedal's 10 dBu out, plans -2 dB: net +4 dB). Measure it instead of guessing: calibrated trim = legacy - net gain.
  const ChainCalibration at12 = assumedDeviceCalibration();
  CHECK(at12.enabled);
  CHECK(*at12.device.dbu == 12.0);
  const auto direct = computeAutoTrim(p, nullptr, nullptr, at12);
  REQUIRE(direct.has_value());
  CHECK(p.autoTrimCal.db == direct->trimDb);  // exactly what the helper measured
  CHECK(std::fabs(p.autoTrimCal.db - legacyDb) > 1.0);  // calibration changed the level, so the trims differ
  // At another device the trim is a re-measure, not an offset of the stored one.
  ChainCalibration at18 = at12;
  at18.device.dbu = 18.0;
  const auto other = computeAutoTrim(p, nullptr, nullptr, at18);
  REQUIRE(other.has_value());
  CHECK(std::fabs(other->trimDb - p.autoTrimCal.db) > 1.0);

  // Staleness: any level-affecting edit invalidates it, labels and the mode do not.
  Preset q = p;
  q.notes = "different notes";
  q.name = "another name";
  q.category = "x";
  CHECK(autoTrimCalFresh(q));
  q.calibrationMode = CalibrationMode::Legacy;
  CHECK(autoTrimCalFresh(q));  // measured with calibration forced on, whatever the preset's mode
  CHECK(autoTrimFresh(q));     // and the legacy stamp is not disturbed by the new member (hash unchanged)
  q.outputGainDb = 3.0;
  CHECK(autoTrimCalFresh(q));  // OUTPUT is a persistent offset on top, like the legacy trim
  q.blend = 0.25;
  CHECK_FALSE(autoTrimCalFresh(q));
  CHECK_FALSE(autoTrimFresh(q));
  q = p;
  q.inputGainDb = 2.0;
  CHECK_FALSE(autoTrimCalFresh(q));

  // The stamp survives the file.
  const Preset back = parse(toJson(p));
  CHECK(autoTrimCalFresh(back));
  CHECK(autoTrimFresh(back));
  CHECK(back.autoTrimCal.db == p.autoTrimCal.db);
  Preset r = back;
  CHECK(ensureAutoTrimCal(r));  // fresh: nothing re-measured, nothing changed
  CHECK(r.autoTrimCal.hash == back.autoTrimCal.hash);
}

// ---- the render follows the preset ---------------------------------------------------------------------------------
TEST_CASE("I4a render: legacy mode is bit-identical to a render with calibration off; calibrated matches the explicit setting", "[i4a][render]") {
  const auto x = noise(24000, 5, 0.1f);
  const AudioFile in = monoFile(x);

  RenderOptions plain;
  plain.renderRate = kFs;
  const Preset legacy = parse(mkPreset("legacy"));
  const RenderResult ref = renderPreset(legacy, in, plain);

  const RenderResult viaPreset = renderPreset(legacy, in, opts());
  SAWBLADE_REQUIRE_SAME_SAMPLES(ref.samples, viaPreset.samples);
  CHECK_FALSE(viaPreset.calibration.enabled);
  CHECK(viaPreset.calibrationMode == "legacy");
  CHECK(viaPreset.calibrationSource == "preset");

  // A v4 file (no member) is the same render.
  SAWBLADE_REQUIRE_SAME_SAMPLES(ref.samples, renderPreset(parse(mkPreset("", 4)), in, opts()).samples);

  // Calibrated: equal to the explicit option on the same preset, and different from legacy.
  const Preset cal = parse(mkPreset("calibrated"));
  const RenderResult onPreset = renderPreset(cal, in, opts());
  RenderOptions explicitOn = plain;
  explicitOn.calibration.enabled = true;
  const RenderResult onOpts = renderPreset(legacy, in, explicitOn);
  SAWBLADE_REQUIRE_SAME_SAMPLES(onOpts.samples, onPreset.samples);
  CHECK(onPreset.calibration.enabled);
  CHECK(onPreset.calibrationMode == "calibrated");
  double d = 0.0;
  for (std::size_t i = 0; i < ref.samples.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(ref.samples[i] - onPreset.samples[i])));
  CHECK(d > 0.01);

  // Not following the preset (the default of RenderOptions): the preset's mode is ignored, calibration stays as set.
  RenderOptions notFollowing;
  notFollowing.renderRate = kFs;
  SAWBLADE_REQUIRE_SAME_SAMPLES(ref.samples, renderPreset(cal, in, notFollowing).samples);
}

TEST_CASE("I4a render: deviceDbu changes the planned gains exactly and the report records assumed or given", "[i4a][render]") {
  const AudioFile in = monoFile(noise(4800, 2, 0.1f));
  const Preset cal = parse(mkPreset("calibrated"));

  const json assumed = reportJson(renderPreset(cal, in, opts()));
  const json& ca = assumed["calibration"];
  CHECK(ca["mode"] == "calibrated");
  CHECK(ca["modeSource"] == "preset");
  CHECK(ca["enabled"].get<bool>());
  CHECK(ca["deviceDbu"].get<double>() == 12.0);
  CHECK(ca["deviceAssumed"].get<bool>());
  REQUIRE(ca["paths"]["a"].size() == 2);
  CHECK(ca["paths"]["a"][0]["gainInDb"].get<double>() == Approx(12.0 - 6.0));

  for (const double dev : {12.0, 15.5, 18.0, 6.0}) {
    INFO("device " << dev);
    RenderOptions o = opts();
    o.deviceDbu = dev;
    const json rep = reportJson(renderPreset(cal, in, o));
    const json& c = rep["calibration"];
    CHECK(c["deviceDbu"].get<double>() == dev);
    CHECK_FALSE(c["deviceAssumed"].get<bool>());  // given, even when it equals the assumed level
    CHECK(c["paths"]["a"][0]["gainInDb"].get<double>() == Approx(dev - 6.0).margin(1e-9));  // pedal: device - captureInputDbu
    // The amp hop: pedal.outputDbu - amp.inputDbu, independent of the device.
    CHECK(c["paths"]["a"][1]["gainInDb"].get<double>() == Approx(10.0 - 12.0).margin(1e-9));
    CHECK(c["paths"]["a"][0]["refBeforeDbu"].get<double>() == Approx(dev));
  }

  // A device level on a legacy preset is recorded, changes nothing and says so.
  const Preset legacy = parse(mkPreset("legacy"));
  RenderOptions o = opts();
  o.deviceDbu = 18.0;
  const RenderResult r = renderPreset(legacy, in, o);
  CHECK_FALSE(r.calibration.enabled);
  CHECK(r.calibration.deviceDbu == 18.0);
  CHECK(std::any_of(r.warnings.begin(), r.warnings.end(), [](const std::string& w) { return w.find("has no effect") != std::string::npos; }));
  RenderOptions none = opts();
  CHECK(renderPreset(legacy, in, none).samples == r.samples);
}

TEST_CASE("I4a render: same input, same output, bit for bit, across runs and block sizes", "[i4a][render]") {
  const AudioFile in = stereoFile(noise(30000, 1, 0.02f), noise(30000, 2, 0.2f));
  for (const char* mode : {"legacy", "calibrated"}) {
    INFO(mode);
    const Preset p = parse(mkPreset(mode));
    const RenderResult a = renderPreset(p, in, opts(256));
    const RenderResult b = renderPreset(p, in, opts(256));
    SAWBLADE_REQUIRE_SAME_SAMPLES(a.samples, b.samples);
    for (const int block : {1, 64, 333, 4096}) {
      INFO("block " << block);
      SAWBLADE_REQUIRE_SAME_SAMPLES(a.samples, renderPreset(p, in, opts(block)).samples);
    }
  }
}

// ---- the offline stereo-DI rule ------------------------------------------------------------------------------------
TEST_CASE("I4a stereo DI: default is the louder channel by whole-file RMS; L, R and mix are explicit; the rule is reported", "[i4a][stereo]") {
  const auto quiet = noise(24000, 3, 0.02f), loud = noise(24000, 4, 0.2f);
  const Preset p = parse(mkPreset("calibrated"));
  const auto monoOf = [&](const std::vector<float>& x) { return renderPreset(p, monoFile(x), opts()).samples; };

  // R louder: Auto picks R.
  {
    const RenderResult r = renderPreset(p, stereoFile(quiet, loud), opts());
    SAWBLADE_REQUIRE_SAME_SAMPLES(monoOf(loud), r.samples);
    CHECK(r.diChannel.rule == "auto");
    CHECK(r.diChannel.used == "R");
    CHECK(r.diChannel.fileChannels == 2);
    CHECK(r.diChannel.rmsDbfsR > r.diChannel.rmsDbfsL + 15.0);
    const json rep = reportJson(r);
    CHECK(rep["diChannel"]["rule"] == "auto");
    CHECK(rep["diChannel"]["used"] == "R");
    CHECK(rep["diChannel"]["fileChannels"] == 2);
    CHECK(rep["diChannel"]["rmsDbfsR"].get<double>() == Approx(rep["diChannel"]["rmsDbfsL"].get<double>() + 20.0).margin(1.0));
    CHECK(std::any_of(r.warnings.begin(), r.warnings.end(), [](const std::string& w) { return w.find("louder channel") != std::string::npos; }));
  }
  // L louder: Auto picks L.
  {
    const RenderResult r = renderPreset(p, stereoFile(loud, quiet), opts());
    SAWBLADE_REQUIRE_SAME_SAMPLES(monoOf(loud), r.samples);
    CHECK(r.diChannel.used == "L");
  }
  // One side digital silence.
  {
    const std::vector<float> zeros(24000, 0.0f);
    const RenderResult r = renderPreset(p, stereoFile(zeros, loud), opts());
    CHECK(r.diChannel.used == "R");
    CHECK(reportJson(r)["diChannel"]["rmsDbfsL"].is_null());  // -inf is not JSON
    SAWBLADE_REQUIRE_SAME_SAMPLES(monoOf(loud), r.samples);
  }
  // A tie picks L.
  {
    const RenderResult r = renderPreset(p, stereoFile(loud, loud), opts());
    CHECK(r.diChannel.used == "L");
  }
  // Explicit overrides win over the louder rule.
  {
    RenderOptions o = opts();
    o.diChannel = DiChannel::Left;
    const RenderResult l = renderPreset(p, stereoFile(quiet, loud), o);
    SAWBLADE_REQUIRE_SAME_SAMPLES(monoOf(quiet), l.samples);
    CHECK(l.diChannel.rule == "L");
    CHECK(l.diChannel.used == "L");
    o.diChannel = DiChannel::Right;
    SAWBLADE_REQUIRE_SAME_SAMPLES(monoOf(loud), renderPreset(p, stereoFile(quiet, loud), o).samples);
    o.diChannel = DiChannel::Mix;
    std::vector<float> mean(quiet.size());
    for (std::size_t i = 0; i < mean.size(); ++i) mean[i] = 0.5f * (quiet[i] + loud[i]);
    const RenderResult m = renderPreset(p, stereoFile(quiet, loud), o);
    SAWBLADE_REQUIRE_SAME_SAMPLES(monoOf(mean), m.samples);
    CHECK(m.diChannel.rule == "mix");
    CHECK(m.diChannel.used == "mix");
  }
  // Mono files are used as they are.
  {
    const RenderResult r = renderPreset(p, monoFile(loud), opts());
    CHECK(r.diChannel.rule == "mono");
    CHECK(r.diChannel.used == "mono");
    CHECK(r.diChannel.fileChannels == 1);
    const json rep = reportJson(r);
    CHECK(rep["diChannel"]["rmsDbfsL"].is_null());
    CHECK(rep["diChannel"]["rule"] == "mono");
  }
}

TEST_CASE("I4a stereo DI: the channel choice is made on the whole file, so it does not depend on the block size", "[i4a][stereo]") {
  // The louder channel changes half way: a block-wise rule would switch, the whole-file rule picks one channel throughout.
  std::vector<float> l = noise(40000, 8, 0.05f), r = noise(40000, 9, 0.05f);
  for (std::size_t i = 0; i < 20000; ++i) l[i] *= 4.0f;   // L loud in the first half
  for (std::size_t i = 20000; i < 40000; ++i) r[i] *= 5.0f;  // R louder in the second
  const Preset p = parse(mkPreset("calibrated"));
  const AudioFile in = stereoFile(l, r);
  const RenderResult ref = renderPreset(p, in, opts(256));
  for (const int block : {1, 7, 100, 4096, 65536}) {
    INFO("block " << block);
    const RenderResult x = renderPreset(p, in, opts(block));
    CHECK(x.diChannel.used == ref.diChannel.used);
    SAWBLADE_REQUIRE_SAME_SAMPLES(ref.samples, x.samples);
  }
}

// ---- tonerender ----------------------------------------------------------------------------------------------------
TEST_CASE("I4a CLI: --device-dbu sets the planned gains, absent means the assumed +12 dBu and says so", "[i4a][cli]") {
  TempDir t;
  writeText(t / "cal.json", mkPreset("calibrated").dump(2));
  writeText(t / "legacy.json", mkPreset("legacy").dump(2));
  writeWavFloat32(t / "di.wav", kFs, noise(24000, 5, 0.1f));

  const auto run = [&](const std::string& preset, const std::string& extra, const std::string& tag) {
    const int rc = runCli("--preset " + q(t / preset) + " --in " + q(t / "di.wav") + " --out " + q(t / (tag + ".wav")) + " --report " +
                              q(t / (tag + ".json")) + " --render-rate 48000 " + extra,
                          t / (tag + ".err"));
    INFO(slurp(t / (tag + ".err")));
    REQUIRE(rc == 0);
    return json::parse(slurp(t / (tag + ".json")));
  };

  const json a = run("cal.json", "", "assumed");
  CHECK(a["calibration"]["mode"] == "calibrated");
  CHECK(a["calibration"]["deviceDbu"] == 12.0);
  CHECK(a["calibration"]["deviceAssumed"] == true);
  CHECK(a["calibration"]["paths"]["a"][0]["gainInDb"].get<double>() == Approx(6.0));
  CHECK(a["diChannel"]["rule"] == "mono");

  const json g = run("cal.json", "--device-dbu 18", "given");
  CHECK(g["calibration"]["deviceDbu"] == 18.0);
  CHECK(g["calibration"]["deviceAssumed"] == false);
  CHECK(g["calibration"]["paths"]["a"][0]["gainInDb"].get<double>() == Approx(12.0));
  // The pedal is 6 dB hotter, nothing else moved: the output level follows.
  CHECK(g["output"]["rmsDbfs"].get<double>() - a["output"]["rmsDbfs"].get<double>() == Approx(6.0).margin(0.05));

  const json same = run("cal.json", "--device-dbu 12", "explicit12");  // equals the assumed level, but is not assumed
  CHECK(same["calibration"]["deviceAssumed"] == false);
  CHECK(same["output"]["rmsDbfs"] == a["output"]["rmsDbfs"]);
  const AudioFile oa = readWav(t / "assumed.wav"), ob = readWav(t / "explicit12.wav");
  SAWBLADE_REQUIRE_SAME_SAMPLES(oa.interleaved, ob.interleaved);

  // Legacy: calibration off whatever the device; the flag is recorded and warned about.
  const json l = run("legacy.json", "--device-dbu 18", "legacy");
  CHECK(l["calibration"]["mode"] == "legacy");
  CHECK(l["calibration"]["enabled"] == false);
  CHECK(l["calibration"]["deviceDbu"] == 18.0);
  CHECK(slurp(t / "legacy.err").find("has no effect") != std::string::npos);
  CHECK(l["output"]["rmsDbfs"].get<double>() == Approx(a["output"]["rmsDbfs"].get<double>() - 4.0).margin(0.05));  // calibrated net gain: pedal +6, amp hop -2

  // Usage errors.
  for (const char* bad : {"--device-dbu 61", "--device-dbu -61", "--device-dbu abc", "--device-dbu", "--di-channel left", "--di-channel"}) {
    INFO(bad);
    CHECK(runCli("--preset " + q(t / "cal.json") + " --in " + q(t / "di.wav") + " --out " + q(t / "x.wav") + " " + bad, t / "bad.err") == 2);
  }
}

TEST_CASE("I4a CLI: --di-channel and the default louder-channel rule, bit-identical across runs and --block", "[i4a][cli][stereo]") {
  TempDir t;
  writeText(t / "cal.json", mkPreset("calibrated").dump(2));
  const auto quiet = noise(24000, 3, 0.02f), loud = noise(24000, 4, 0.2f);
  writeWavFloat32Stereo(t / "st.wav", kFs, quiet, loud);
  writeWavFloat32(t / "loud.wav", kFs, loud);
  writeWavFloat32(t / "quiet.wav", kFs, quiet);

  int n = 0;
  const auto run = [&](const std::string& in, const std::string& extra) {
    const std::string tag = "r" + std::to_string(n++);
    const int rc = runCli("--preset " + q(t / "cal.json") + " --in " + q(t / in) + " --out " + q(t / (tag + ".wav")) + " --report " +
                              q(t / (tag + ".json")) + " --render-rate 48000 " + extra,
                          t / (tag + ".err"));
    INFO(slurp(t / (tag + ".err")));
    REQUIRE(rc == 0);
    return std::pair{readWav(t / (tag + ".wav")).interleaved, json::parse(slurp(t / (tag + ".json")))};
  };

  const auto [def, defRep] = run("st.wav", "");
  CHECK(defRep["diChannel"]["rule"] == "auto");
  CHECK(defRep["diChannel"]["used"] == "R");
  CHECK(defRep["diChannel"]["fileChannels"] == 2);
  SAWBLADE_REQUIRE_SAME_SAMPLES(run("loud.wav", "").first, def);

  const auto [l, lRep] = run("st.wav", "--di-channel L");
  CHECK(lRep["diChannel"]["rule"] == "L");
  CHECK(lRep["diChannel"]["used"] == "L");
  SAWBLADE_REQUIRE_SAME_SAMPLES(run("quiet.wav", "").first, l);

  const auto [r, rRep] = run("st.wav", "--di-channel R");
  CHECK(rRep["diChannel"]["used"] == "R");
  SAWBLADE_REQUIRE_SAME_SAMPLES(def, r);

  const auto [m, mRep] = run("st.wav", "--di-channel mix");
  CHECK(mRep["diChannel"]["used"] == "mix");
  CHECK(m != def);

  for (const char* blk : {"--block 1", "--block 64", "--block 5000"}) {
    INFO(blk);
    const auto [b, bRep] = run("st.wav", blk);
    SAWBLADE_REQUIRE_SAME_SAMPLES(def, b);
    CHECK(bRep["diChannel"]["used"] == "R");
  }
  SAWBLADE_REQUIRE_SAME_SAMPLES(def, run("st.wav", "").first);  // a second run
}

TEST_CASE("I4a CLI: --level-match on a calibrated preset uses the stored calibrated trim, or re-measures at the given device", "[i4a][cli][autotrim]") {
  TempDir t;
  Preset p = parse(mkPreset("calibrated"));
  REQUIRE(stampAutoTrimCal(p));
  writeText(t / "cal.json", toJson(p).dump(2));
  Preset noStamp = parse(mkPreset("calibrated"));
  writeText(t / "nostamp.json", toJson(noStamp).dump(2));
  writeWavFloat32(t / "di.wav", kFs, noise(24000, 5, 0.1f));

  const auto run = [&](const std::string& preset, const std::string& extra, const std::string& tag) {
    const int rc = runCli("--preset " + q(t / preset) + " --in " + q(t / "di.wav") + " --out " + q(t / (tag + ".wav")) + " --report " +
                              q(t / (tag + ".json")) + " --render-rate 48000 --level-match " + extra,
                          t / (tag + ".err"));
    INFO(slurp(t / (tag + ".err")));
    REQUIRE(rc == 0);
    return json::parse(slurp(t / (tag + ".json")));
  };

  const json stored = run("cal.json", "", "stored");
  CHECK(stored["levelMatchTrim"]["source"] == "stored autoTrimCalDb");
  CHECK(stored["autoTrimDb"].get<double>() == Approx(p.autoTrimCal.db));

  const json measured = run("nostamp.json", "", "measured");
  CHECK(measured["levelMatchTrim"]["source"] == "measured at +12 dBu");
  CHECK(measured["autoTrimDb"].get<double>() == Approx(p.autoTrimCal.db));

  const json at18 = run("cal.json", "--device-dbu 18", "at18");
  CHECK(at18["levelMatchTrim"]["source"] == "measured at the given device level");
  CHECK(at18["levelMatchTrim"]["deviceDbu"] == 18.0);
  CHECK(std::fabs(at18["autoTrimDb"].get<double>() - p.autoTrimCal.db) > 1.0);  // a re-measure, not the stored value

  // Whatever the device, the leveled render lands on the same loudness target: the trim absorbs the 6 dB.
  CHECK(at18["output"]["rmsDbfs"].get<double>() == Approx(stored["output"]["rmsDbfs"].get<double>()).margin(0.5));
}

// ---- the committed presets, legacy vs calibrated --------------------------------------------------------------------
namespace {

std::vector<fs::path> factoryPresets() {
  std::vector<fs::path> v;
  for (const auto& e : fs::recursive_directory_iterator(SAWBLADE_PRESETS_DIR))
    if (e.path().extension() == ".json") v.push_back(e.path());
  std::sort(v.begin(), v.end());
  return v;
}
std::string relToFactory(const fs::path& p) { return fs::relative(p, SAWBLADE_PRESETS_DIR).generic_string(); }
// Non-bypassed NAM blocks on enabled paths: with none, calibration cannot change a render.
int namBlockCount(const Preset& p) {
  int n = 0;
  for (const PathPreset* pp : {&p.a, &p.b})
    if (pp->enabled)
      for (const Block& b : pp->blocks)
        if (!b.bypass && b.type == "nam") ++n;
  return n;
}

}  // namespace

TEST_CASE("I4a committed presets: all load as legacy and render bit-identically with calibration following the preset", "[i4a][presets]") {
  const fs::path file = GENERATE(from_range(factoryPresets()));
  INFO(relToFactory(file));
  const Preset p = loadPresetFile(file);
  CHECK(p.calibrationMode == CalibrationMode::Legacy);  // restamping them is I4c's job
  if (const auto missing = missingCaptures(p); !missing.empty())
    SKIP("skipped: capture not cached (" << missing.front() << ")");
  const AudioFile di = readWav(fs::path(SAWBLADE_FIXTURES_DIR) / "di_riff.wav");
  CaptureCache cache;
  RenderOptions plain;
  plain.cache = &cache;
  RenderOptions follow = plain;
  follow.calibrationFromPreset = true;
  SAWBLADE_REQUIRE_SAME_SAMPLES(renderPreset(p, di, plain).samples, renderPreset(p, di, follow).samples);
}

// Hidden: renders every committed preset whose captures are on this machine legacy and calibrated (assumed +12 dBu device) and writes
// the comparison table (Markdown) to $SAWBLADE_I4A_TABLE_OUT (default: stdout via WARN). Run:
//   SAWBLADE_I4A_TABLE_OUT=/tmp/table.md build/tests/sawblade_tests "[i4a-table]"
// Tolerances are the existing ones: golden = the cross-platform level check of the preset golden test (rms and peak on di_riff.wav
// within 0.01 dB; bit identity on the reference platform), level = the trim acceptance (reference-DI loudness within 0.5 LU).
TEST_CASE("I4a committed presets: legacy vs calibrated table", "[.][i4a-table]") {
  const AudioFile di = readWav(fs::path(SAWBLADE_FIXTURES_DIR) / "di_riff.wav");
  std::string md =
      "| preset | NAM blocks | planned NAM gains A / B (dB) | uncal. | bit-identical | d rms (dB) | d peak (dB) | d LUFS ref DI | golden 0.01 dB | level 0.5 LU |\n"
      "|---|---|---|---|---|---|---|---|---|---|\n";
  std::vector<std::string> skipped;
  int within = 0, total = 0;
  const auto fmt = [](double v, const char* f = "%+.2f") {
    char b[32];
    std::snprintf(b, sizeof b, f, v);
    return std::string(b);
  };
  for (const fs::path& f : factoryPresets()) {
    Preset p = loadPresetFile(f);
    if (const auto missing = missingCaptures(p); !missing.empty()) {
      skipped.push_back(relToFactory(f) + ": " + std::to_string(namBlockCount(p)) + " NAM block(s); needs " + missing.front());
      continue;
    }
    CaptureCache cache;
    RenderOptions o;
    o.cache = &cache;
    const RenderResult leg = renderPreset(p, di, o);
    Preset cp = p;
    cp.calibrationMode = CalibrationMode::Calibrated;
    RenderOptions oc = o;
    oc.calibrationFromPreset = true;
    const RenderResult cal = renderPreset(cp, di, oc);
    const bool same = leg.samples == cal.samples;
    const auto l0 = measureReferenceLufs(p, &cache, false);
    ChainCalibration c12 = assumedDeviceCalibration();
    c12.device.dbu.reset();  // as tonerender without --device-dbu: assumed
    const auto l1 = measureReferenceLufs(p, &cache, false, nullptr, c12);
    std::string gains;
    for (std::size_t k = 0; k < 2; ++k) {
      std::string g;
      for (const auto& b : cal.calibration.blocks[k])
        if (b.planned && b.kind == calibration::LevelKind::Nam) g += (g.empty() ? "" : ", ") + fmt(b.gainInDb, "%+.1f") + (b.inputMissing ? "?" : "");
      gains += (k ? " / " : "") + (g.empty() ? std::string("-") : g);
    }
    const double dr = cal.output.rmsDbfs - leg.output.rmsDbfs, dp = cal.output.peakDbfs - leg.output.peakDbfs;
    const double dl = (l0 && l1) ? *l1 - *l0 : 0.0;
    const bool golden = same || (std::fabs(dr) <= 0.01 && std::fabs(dp) <= 0.01);
    const bool level = l0 && l1 && std::fabs(dl) <= 0.5;
    ++total;
    if (golden && level) ++within;
    md += "| " + relToFactory(f) + " | " + std::to_string(namBlockCount(p)) + " | " + gains + " | " + (cal.calibration.anyUncalibrated ? "yes" : "no") + " | " + (same ? "yes" : "no") +
          " | " + fmt(dr) + " | " + fmt(dp) + " | " + ((l0 && l1) ? fmt(dl) : std::string("n/a")) + " | " + (golden ? "within" : "OUTSIDE") +
          " | " + (level ? "within" : "OUTSIDE") + " |\n";
  }
  md += "\nWithin both tolerances: " + std::to_string(within) + " of " + std::to_string(total) + " rendered here.\n";
  if (!skipped.empty()) {
    md += "\nSkipped (captures not cached on this machine):\n";
    for (const auto& s : skipped) md += "- " + s + "\n";
  }
  if (const char* out = std::getenv("SAWBLADE_I4A_TABLE_OUT")) std::ofstream(out) << md;
  else WARN(md);
  CHECK(total >= 30);
}

TEST_CASE("I4a CLI: --level-match re-measures a stale calibrated stamp, and applies no trim when the reference renders silent", "[i4a][cli][autotrim]") {
  TempDir t;
  Preset p = parse(mkPreset("calibrated"));
  REQUIRE(stampAutoTrimCal(p));
  const double fresh = p.autoTrimCal.db;
  p.autoTrimCal.db = fresh + 5.0;  // a wrong value under a hash that no longer matches
  p.autoTrimCal.hash = "stale";
  writeText(t / "stale.json", toJson(p).dump(2));
  json quietJ = toJson(parse(mkPreset("calibrated")));
  quietJ["input"] = {{"gainDb", -60.0}};  // the reference DI falls below the -70 LUFS gate
  Preset quiet = parse(quietJ);
  quiet.autoTrim.db = 7.0;
  quiet.autoTrim.hash = "stale";
  quiet.autoTrimCal.db = 7.0;
  quiet.autoTrimCal.hash = "stale";
  writeText(t / "quiet.json", toJson(quiet).dump(2));
  Preset quietLegacy = quiet;
  quietLegacy.calibrationMode = CalibrationMode::Legacy;
  writeText(t / "quiet_legacy.json", toJson(quietLegacy).dump(2));
  writeWavFloat32(t / "di.wav", kFs, noise(24000, 5, 0.1f));
  const auto run = [&](const std::string& preset, const std::string& tag) {
    REQUIRE(runCli("--preset " + q(t / preset) + " --in " + q(t / "di.wav") + " --out " + q(t / (tag + ".wav")) + " --report " +
                       q(t / (tag + ".json")) + " --render-rate 48000 --level-match",
                   t / (tag + ".err")) == 0);
    return json::parse(slurp(t / (tag + ".json")));
  };
  const json s = run("stale.json", "stale");
  CHECK(s["levelMatchTrim"]["source"] == "measured at +12 dBu");
  CHECK(s["autoTrimDb"].get<double>() == Approx(fresh));
  for (const char* f : {"quiet.json", "quiet_legacy.json"}) {
    INFO(f);
    const json r = run(f, std::string("q_") + f);
    CHECK(r["autoTrimDb"].get<double>() == 0.0);
    CHECK(r["levelMatchTrim"]["source"] == "none (reference DI renders silent)");
  }
}
