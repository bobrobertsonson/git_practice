// Tests of the mic page's non-visual parts (docs/specs/phase9a_mic_page.md sections 3, 4 and 6): the IR name parser, the pack
// and its dot layout / snapping, choosing models through the processor, the sawblade-t3k runner and its settings. The page itself
// is tested in test_editor.cpp.
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <optional>
#include "BinaryData.h"
#include "browser/T3kRunner.h"
#include "PluginProcessor.h"
#include "mic/IrNameParser.h"
#include "mic/IrPack.h"
#include "mic/IrResponse.h"
#include "mic/MicSession.h"
#include "SettingsEnv.h"
#include "presets/PresetLibrary.h"
#include "presets/T3kTool.h"
#include "sawblade/sha256.h"
#include "sawblade/wav_io.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::plugin;
using namespace sawblade::plugin::mic;
using nlohmann::json;
namespace fs = std::filesystem;

#include "processor_harness.h"

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Expect {
  const char* name;
  const char* speaker;
  int slot;
  const char* mic;
  MicType micType;
  double distanceIn;  // kNaN = unknown
  MicPosition position;
  int positionIndex;
  const char* cabSize;
};

const Expect kTable[] = {
    // the formats the spec lists
    {"V30 UL 4FB 4x12 SM57 0.50in", "V30", 1, "SM57", MicType::Dynamic, 0.5, MicPosition::Unknown, 0, "4x12"},
    {"Mesa OS - V30 1 SM57 - V30 3", "V30", 1, "SM57", MicType::Dynamic, kNaN, MicPosition::Unknown, 3, ""},
    {"Marshall G12 1 SM57 3", "G12", 1, "SM57", MicType::Dynamic, kNaN, MicPosition::Unknown, 3, ""},
    {"SM57_CapEdge_1in", "unknown", 0, "SM57", MicType::Dynamic, 1.0, MicPosition::CapEdge, 0, ""},
    {"G12T75 LR MD421 cone 2\"", "G12T75", 4, "MD421", MicType::Dynamic, 2.0, MicPosition::Cone, 0, ""},
    {"R121 2in off axis", "unknown", 0, "R121", MicType::Ribbon, 2.0, MicPosition::OffAxis, 0, ""},
    {"Greenback 2x12 e906 25mm cap", "Greenback", 0, "e906", MicType::Dynamic, 25.0 / 25.4, MicPosition::Cap, 0, "2x12"},
    {"impulse 7", "unknown", 0, "unknown", MicType::Unknown, kNaN, MicPosition::Unknown, 0, ""},
    {"", "unknown", 0, "unknown", MicType::Unknown, kNaN, MicPosition::Unknown, 0, ""},
    // more real-world shapes
    {"Creamback TR u87 1.5in", "Creamback", 2, "U87", MicType::Condenser, 1.5, MicPosition::Unknown, 0, ""},
    {"v30_spk3_sm57_cap_0in", "V30", 3, "SM57", MicType::Dynamic, 0.0, MicPosition::Cap, 0, ""},
    {"G12M speaker 2 M160 2.5cm Edge", "G12M", 2, "M160", MicType::Ribbon, 2.5 / 2.54, MicPosition::Edge, 0, ""},
    {"EVM12L BL i5 CE 1\"", "EVM12L", 3, "i5", MicType::Dynamic, 1.0, MicPosition::CapEdge, 0, ""},
    {"Vintage 30 \xc2\xb7 SM7B \xc2\xb7 Cap Edge \xc2\xb7 0.5 in", "V30", 0, "SM7B", MicType::Dynamic, 0.5, MicPosition::CapEdge, 0, ""},
    {"sm-57 3 4x12 BR", "unknown", 4, "SM57", MicType::Dynamic, kNaN, MicPosition::Unknown, 3, "4x12"},
    {"MD-421 45\xc2\xb0 V-30 UR", "V30", 2, "MD421", MicType::Dynamic, kNaN, MicPosition::OffAxis, 0, ""},
    {"C414 4x10 cab", "unknown", 0, "C414", MicType::Condenser, kNaN, MicPosition::Unknown, 0, ""},
    {"SM58,2in,cone", "unknown", 0, "SM58", MicType::Dynamic, 2.0, MicPosition::Cone, 0, ""},
    {"G12H30 TL SM57 1.0in cap-edge", "G12H", 1, "SM57", MicType::Dynamic, 1.0, MicPosition::CapEdge, 0, ""},
    {"Royer121 TR", "unknown", 2, "R121", MicType::Ribbon, kNaN, MicPosition::Unknown, 0, ""},
    {"BeTa57 CONE", "unknown", 0, "Beta57", MicType::Dynamic, kNaN, MicPosition::Cone, 0, ""},
    {"V30 5 SM57", "V30", 0, "SM57", MicType::Dynamic, kNaN, MicPosition::Unknown, 5, ""},
    {"4x12 V30 4FB", "V30", 0, "unknown", MicType::Unknown, kNaN, MicPosition::Unknown, 0, "4x12"},
    {"1x12 combo SM57 12mm", "unknown", 0, "SM57", MicType::Dynamic, 12.0 / 25.4, MicPosition::Unknown, 0, "1x12"},
    {"SM57", "unknown", 0, "SM57", MicType::Dynamic, kNaN, MicPosition::Unknown, 0, ""},  // a mic is never a speaker
    {"V30", "V30", 0, "unknown", MicType::Unknown, kNaN, MicPosition::Unknown, 0, ""},    // and a speaker never a mic
    {"!!! ??? ...", "unknown", 0, "unknown", MicType::Unknown, kNaN, MicPosition::Unknown, 0, ""},
};

}  // namespace

TEST_CASE("IR name parser: table of real-world formats", "[mic][parser]") {
  for (const Expect& e : kTable) {
    INFO("name: \"" << e.name << "\"");
    const MicShot m = parseIrName(e.name);
    CHECK(m.raw == e.name);
    CHECK(m.speaker == e.speaker);
    CHECK(m.speakerSlot == e.slot);
    CHECK(m.mic == e.mic);
    CHECK(m.micType == e.micType);
    if (std::isnan(e.distanceIn)) CHECK(std::isnan(m.distanceIn));
    else CHECK(m.distanceIn == Catch::Approx(e.distanceIn).margin(1e-9));
    CHECK(m.position == e.position);
    CHECK(m.positionIndex == e.positionIndex);
    CHECK(m.cabSize == e.cabSize);
  }
  CHECK(std::size(kTable) >= 20);
}

TEST_CASE("IR name parser: never throws on odd input", "[mic][parser]") {
  const std::string junk[] = {std::string("\xff\xfe\xfd", 3), std::string(1, '\0'), std::string(100000, 'x'), "((((", "\"", "...", "1x", "x12",
                              "0.0.0in", "in", "mm mm mm", std::string("\xc2", 1), "V30 999999999999999 SM57 99999999999in"};
  for (const auto& j : junk) {
    CHECK_NOTHROW(parseIrName(j));
    CHECK(parseIrName(j).raw == j);
  }
}

// ---- the pack and its dots ------------------------------------------------------------------------------------------
namespace {

CabLayout layout4x12() {
  CabLayout l;
  REQUIRE(parseCabLayout(std::string_view(BinaryData::cab_4x12_open_json, static_cast<size_t>(BinaryData::cab_4x12_open_jsonSize)), l));
  return l;
}

IrPack manifestOf(const std::vector<std::string>& names, const std::string& tone = "42") {
  json models = json::array();
  int i = 0;
  for (const auto& n : names) models.push_back({{"modelId", std::to_string(100 + i)}, {"name", n}, {"file", "/cache/" + std::to_string(i) + ".wav"}, {"sha256", std::string(64, 'a')}}), ++i;
  const json j = {{"toneId", tone}, {"title", "Test pack"}, {"creator", "someone"}, {"license", "cc-by"}, {"url", "https://example.org/t/42"}, {"models", models}};
  std::string err;
  IrPack p = IrPack::fromManifestJson(j.dump(), &err);
  REQUIRE(err.empty());
  return p;
}

}  // namespace

TEST_CASE("sidecars of the embedded open cab views parse and match their images", "[mic][layout]") {
  CabLayout l4, l2;
  REQUIRE(parseCabLayout(std::string_view(BinaryData::cab_4x12_open_json, static_cast<size_t>(BinaryData::cab_4x12_open_jsonSize)), l4));
  REQUIRE(parseCabLayout(std::string_view(BinaryData::cab_2x12_open_json, static_cast<size_t>(BinaryData::cab_2x12_open_jsonSize)), l2));
  CHECK(l4.drivers.size() == 4);
  CHECK(l2.drivers.size() == 2);
  // symmetric about the image centre, cones inside the image
  CHECK(l4.drivers[0].cx + l4.drivers[1].cx == Catch::Approx(l4.width).margin(0.5));
  CHECK(l4.drivers[0].cy + l4.drivers[2].cy == Catch::Approx(l4.height).margin(0.5));
  CHECK(l2.drivers[0].cx + l2.drivers[1].cx == Catch::Approx(l2.width).margin(0.5));
  for (const auto& d : l4.drivers) CHECK(d.cx - d.coneRadius > 0.0);
  CHECK_FALSE(parseCabLayout("not json", l4));
  CHECK_FALSE(parseCabLayout("{\"width\":10,\"height\":10,\"drivers\":[]}", l4));
  CHECK_FALSE(parseCabLayout("{\"width\":10,\"height\":10,\"drivers\":[{\"cx\":1}]}", l4));
}

TEST_CASE("dot layout: models at the same point merge into one dot", "[mic][layout]") {
  IrPack p = manifestOf({
      "V30 UL SM57 Cap 1in",        // 0  dot 0
      "V30 UL MD421 Cap 1in",       // 1  dot 0 (differs only in the mic)
      "V30 UL SM57 CapEdge 1in",    // 2  dot 1
      "V30 UR SM57 Cone 1in",       // 3  dot 2
      "V30 UR SM57 Edge 1in",       // 4  dot 3
      "V30 LL SM57 Cap 2in",        // 5  dot 4 (another driver)
      "V30 UL SM57 Cap 2in",        // 6  dot 0 (differs only in the distance)
      "V30 SM57 Cap",               // 7  dot 0 (unknown slot = driver 1)
      "V30 UR SM57 off axis 1in",   // 8  dot 2 (OffAxis is at the Cone radius): angle mark
      "V30 LR SM57",                // 9  dot 5 (unknown position = 0.3)
  });
  const CabLayout l = layout4x12();
  p.setLayout(l);
  REQUIRE(p.dots().size() == 6);
  CHECK(p.dots()[0].models == std::vector<int>{0, 1, 6, 7});
  CHECK(p.dots()[1].models == std::vector<int>{2});
  CHECK(p.dots()[2].models == std::vector<int>{3, 8});
  CHECK(p.dots()[3].models == std::vector<int>{4});
  CHECK(p.dots()[4].models == std::vector<int>{5});
  CHECK(p.dots()[5].models == std::vector<int>{9});
  CHECK(p.dotOfModel(7) == 0);
  CHECK(p.dotOfModel(8) == 2);
  CHECK(p.dots()[2].offAxis);
  CHECK_FALSE(p.dots()[0].offAxis);
  CHECK(p.dotOfModel(99) == -1);

  // positions: the offset from the dust-cap centre is radial * cone radius, towards the cab centre along the horizontal
  const auto& ul = l.drivers[0];
  const auto& ur = l.drivers[1];
  CHECK(p.dots()[0].pos.x == Catch::Approx(ul.cx / l.width));
  CHECK(p.dots()[0].pos.y == Catch::Approx(ul.cy / l.width));
  CHECK(p.dots()[1].pos.x == Catch::Approx((ul.cx + 0.3 * ul.coneRadius) / l.width));       // CapEdge, UL: to the right
  CHECK(p.dots()[2].pos.x == Catch::Approx((ur.cx - 0.6 * ur.coneRadius) / l.width));       // Cone, UR: to the left
  CHECK(p.dots()[3].pos.x == Catch::Approx((ur.cx - 0.9 * ur.coneRadius) / l.width));       // Edge
  CHECK(p.dots()[5].pos.x == Catch::Approx((l.drivers[3].cx - 0.3 * l.drivers[3].coneRadius) / l.width));
  CHECK(p.dots()[0].radial == 0.0);
  CHECK(p.dots()[1].radial == Catch::Approx(0.3));
  CHECK(p.dots()[3].radial == Catch::Approx(0.9));
}

TEST_CASE("dot layout: numeric positions spread over 0..0.9, a single unknown model sits at 0.3", "[mic][layout]") {
  IrPack p = manifestOf({"Marshall G12 1 SM57 1", "Marshall G12 1 SM57 2", "Marshall G12 1 SM57 3", "Marshall G12 1 SM57 5", "Marshall G12 1 MD421 5"});
  p.setLayout(layout4x12());
  REQUIRE(p.dots().size() == 4);  // N = 5: 0, 0.225, 0.45, 0.9; the MD421 shot merges with the SM57 one
  CHECK(p.dots()[0].radial == Catch::Approx(0.0));
  CHECK(p.dots()[1].radial == Catch::Approx(0.9 * 1.0 / 4.0));
  CHECK(p.dots()[2].radial == Catch::Approx(0.9 * 2.0 / 4.0));
  CHECK(p.dots()[3].radial == Catch::Approx(0.9));
  CHECK(p.dots()[3].models == std::vector<int>{3, 4});

  IrPack one = manifestOf({"impulse 7"});
  one.setLayout(layout4x12());
  REQUIRE(one.dots().size() == 1);
  CHECK(one.dots()[0].radial == Catch::Approx(0.3));
  IrPack single = manifestOf({"Marshall G12 1 SM57 1"});  // N = 1: max(N - 1, 1) avoids a division by zero
  single.setLayout(layout4x12());
  CHECK(single.dots()[0].radial == Catch::Approx(0.0));
}

TEST_CASE("pack: the 2x12 view is used when most named cab sizes are 2x12", "[mic][layout]") {
  CHECK(manifestOf({"Greenback 2x12 SM57", "Greenback 2x12 e906", "V30 4x12 SM57", "x"}).preferredCab() == "2x12");
  CHECK(manifestOf({"V30 4x12 SM57", "V30 2x12 SM57", "x"}).preferredCab() == "4x12");
  CHECK(manifestOf({"no size here"}).preferredCab() == "4x12");
}

TEST_CASE("snap to nearest: nearest dot, ties to the lower model, keeps mic and nearest distance", "[mic][snap]") {
  IrPack p = manifestOf({
      "V30 UL SM57 Cap 1in",       // 0
      "V30 UL MD421 Cap 1in",      // 1
      "V30 UL SM57 Cap 2in",       // 2
      "V30 UL SM57 Cap 4in",       // 3
      "V30 UR MD421 Cap 1in",      // 4  (a dot without an SM57)
      "V30 LL SM57 Cap 1in",       // 5
  });
  p.setLayout(layout4x12());
  REQUIRE(p.dots().size() == 3);
  const Point ulDot = p.dots()[0].pos, urDot = p.dots()[1].pos, llDot = p.dots()[2].pos;

  // nearest dot, regardless of the distance from it
  CHECK(p.snapToNearest({ulDot.x + 0.01, ulDot.y - 0.02}, 5).dot == 0);
  CHECK(p.snapToNearest({urDot.x - 0.05, urDot.y + 0.10}, 0).dot == 1);
  CHECK(p.snapToNearest({0.0, 1.0}, 0).dot == 2);
  CHECK(p.snapToNearest({llDot.x, llDot.y}, 0).model == 5);

  // keeps the current mic and the nearest distance to the current one when the dot has them
  CHECK(p.snapToNearest(ulDot, 5).model == 0);   // current: SM57 1in (in another dot) -> SM57 1in
  CHECK(p.snapToNearest(ulDot, 2).model == 2);   // SM57 2in -> SM57 2in
  CHECK(p.snapToNearest(ulDot, 3).model == 3);   // SM57 4in
  CHECK(p.snapToNearest(ulDot, 1).model == 1);   // MD421 1in
  // a current distance between two available ones goes to the nearest; an exact tie in distance goes to the lower model
  IrPack q = manifestOf({"V30 UL SM57 Cap 1in", "V30 UL SM57 Cap 3in", "V30 UR SM57 Cap 2in"});
  q.setLayout(layout4x12());
  CHECK(q.snapToNearest(q.dots()[0].pos, 2).model == 0);  // |1 - 2| == |3 - 2|: the lower model
  // the dot has no such mic: its first model
  CHECK(p.snapToNearest(urDot, 0).model == 4);
  // no current model (nothing chosen yet): the dot's first model
  CHECK(p.snapToNearest(ulDot, -1).model == 0);

  // a tie between two dots goes to the lower model index: points exactly midway between two drivers
  CabLayout two;
  REQUIRE(parseCabLayout(R"({"cab":"2x12","width":128,"height":128,"drivers":[
      {"slot":1,"cx":32,"cy":64,"cone_radius":16,"cap_radius":4},{"slot":2,"cx":96,"cy":64,"cone_radius":16,"cap_radius":4}]})", two));
  IrPack t = manifestOf({"V30 UR SM57 Cap", "V30 UL SM57 Cap"});  // model 0 sits on the right driver, model 1 on the left
  t.setLayout(two);
  REQUIRE(t.dots().size() == 2);
  const Snap s = t.snapToNearest({0.5, 0.5}, -1);
  CHECK(s.model == 0);
  CHECK(s.dot == 0);
  CHECK(t.snapToNearest({0.5, 0.9}, -1).model == 0);

  IrPack empty;
  CHECK(empty.snapToNearest({0, 0}, 0).model == -1);
}

TEST_CASE("pack sources: manifest, folder and single IR", "[mic][pack]") {
  TempDir tmp;
  // a folder pack: file stems are the names, no attribution; non-wav files are ignored
  for (const char* n : {"b SM57.wav", "A MD421.WAV", "notes.txt"}) std::ofstream(tmp.dir / n) << "x";
  std::string err;
  IrPack f = IrPack::fromFolder(tmp.dir, &err);
  REQUIRE(f.size() == 2);
  CHECK(f.info().kind == PackInfo::Kind::Folder);
  CHECK(f.info().title == tmp.dir.filename().string());
  CHECK(f.info().creator.empty());
  CHECK(f.models()[0].name == "A MD421");  // sorted, case-insensitively
  CHECK(f.models()[1].name == "b SM57");
  const Capture c = f.captureFor(1);
  CHECK(c.file == (tmp.dir / "b SM57.wav").string());
  CHECK_FALSE(c.source);
  CHECK(c.sha256.empty());
  CHECK(f.findModel(c) == 1);

  // errors never throw
  CHECK(IrPack::fromFolder(tmp.dir / "nope", &err).empty());
  CHECK_FALSE(err.empty());
  fs::create_directories(tmp.dir / "emptydir");
  CHECK(IrPack::fromFolder(tmp.dir / "emptydir", &err).empty());
  CHECK(IrPack::fromManifestJson("{", &err).empty());
  CHECK(IrPack::fromManifestJson("{\"models\": []}", &err).empty());
  CHECK(IrPack::fromManifestJson("{\"models\": [{\"name\": \"x\"}]}", &err).empty());
  CHECK(IrPack::fromManifestFile(tmp.dir / "missing.json", &err).empty());

  // a manifest pack: the Capture carries the cache path, sha256 and a TONE3000 source
  const std::string a = (tmp.dir / "0.wav").string(), b = (tmp.dir / "1.wav").string();
  for (const auto& n : {a, b}) std::ofstream(n) << "x";
  const json j = {{"toneId", 77}, {"title", "Cab pack"}, {"creator", "jp"}, {"license", "t3k"}, {"url", "https://t/77"},
                  {"models", json::array({{{"modelId", 5}, {"name", "V30 UL SM57 Cap"}, {"file", a}, {"sha256", std::string(64, 'B')}},
                                          {{"modelId", "6"}, {"name", "V30 UL SM57 Cone"}, {"file", b}, {"sha256", std::string(64, 'c')}}})}};
  std::ofstream(tmp.dir / "m.json") << j.dump();
  IrPack m = IrPack::fromManifestFile(tmp.dir / "m.json", &err);
  REQUIRE(m.size() == 2);
  CHECK(m.info().toneId == "77");
  const Capture mc = m.captureFor(1);
  CHECK(mc.file == b);
  CHECK(mc.sha256 == std::string(64, 'c'));
  REQUIRE(mc.source);
  CHECK(mc.source->provider == "tone3000");
  CHECK(mc.source->id == "77");
  CHECK(mc.source->modelId == "6");
  CHECK(mc.source->title == "V30 UL SM57 Cone");
  CHECK(mc.source->creator == "jp");
  CHECK(mc.source->license == "t3k");
  CHECK(mc.source->url == "https://t/77");
  CHECK(m.captureFor(0).sha256 == std::string(64, 'b'));  // lower-cased
  // a model found by its TONE3000 id when the file is elsewhere
  Capture elsewhere = mc;
  elsewhere.file = "/other/cache/x.wav";
  elsewhere.resolvedPath = elsewhere.file;
  CHECK(m.findModel(elsewhere) == 1);
  // missing files make the cached manifest unusable (the page then offers LOAD PACK again)
  fs::remove(b);
  CHECK(IrPack::fromManifestFile(tmp.dir / "m.json", &err).empty());
  CHECK(err.find("missing") != std::string::npos);

  // the single IR of a cab
  Capture cab;
  cab.file = "/x/v30.wav";
  cab.resolvedPath = cab.file;
  IrPack s = IrPack::single(cab);
  REQUIRE(s.size() == 1);
  CHECK(s.info().kind == PackInfo::Kind::Single);
  CHECK(s.captureFor(0) == cab);
  s.setLayout(layout4x12());
  CHECK(s.dots().size() == 1);
}

TEST_CASE("response: flat for an impulse, shaped for a low-pass, linear in the IR", "[mic][response]") {
  std::vector<float> imp(256, 0.0f);
  imp[0] = 1.0f;
  const Response flat = magnitudeResponse(imp, 48000.0);
  REQUIRE(flat.db.size() == 240);
  for (float v : flat.db) CHECK(std::fabs(v) < 0.01f);
  // a 2-tap average has |H| = cos(w/2): -3 dB at fs/4 (12 kHz), about 0 dB at 20 Hz
  std::vector<float> lp(256, 0.0f);
  lp[0] = lp[1] = 0.5f;
  const Response r = magnitudeResponse(lp, 48000.0);
  CHECK(std::fabs(r.db.front()) < 0.1f);
  CHECK(r.db.back() < -6.0f);
  // the mix of two spectra is the spectrum of the mixed IR
  std::vector<float> d1(256, 0.0f);
  d1[1] = 1.0f;
  const Spectrum sa = irSpectrum(imp, 48000.0), sb = irSpectrum(d1, 48000.0);
  std::vector<float> mixed(256, 0.0f);
  mixed[0] = 0.7f;
  mixed[1] = 0.3f;
  const Response viaSpectra = responseFromSpectrum(sa, &sb, 0.3), direct = magnitudeResponse(mixed, 48000.0);
  REQUIRE(viaSpectra.db.size() == direct.db.size());
  for (std::size_t i = 0; i < direct.db.size(); ++i) CHECK(viaSpectra.db[i] == Catch::Approx(direct.db[i]).margin(1e-3));
  CHECK(magnitudeResponse({}, 48000.0).empty());
}

// ---- choosing models through the processor ------------------------------------------------------------------------
namespace {

// A folder of synthetic IRs (never committed; TONE3000 captures are not used in tests).
fs::path writeIrFolder(const fs::path& root, const std::vector<std::string>& names) {
  const fs::path dir = root / "ir pack";
  fs::create_directories(dir);
  int k = 0;
  for (const auto& n : names) {
    std::vector<float> ir(200, 0.0f);
    ir[static_cast<std::size_t>(5 + 7 * k)] = 1.0f;
    ir[static_cast<std::size_t>(9 + 3 * k)] += 0.25f;
    writeWavFloat32(dir / (n + ".wav"), 48000.0, ir);
    ++k;
  }
  return dir;
}

int indexOf(const IrPack& p, const std::string& name) {
  for (int i = 0; i < p.size(); ++i)
    if (p.models()[static_cast<std::size_t>(i)].name == name) return i;
  FAIL("no model named " << name);
  return -1;
}

struct ProcessorWithPreset {
  SawbladeProcessor proc;
  TempDir tmp;
  ProcessorWithPreset() {
    proc.prepareToPlay(48000.0, 512);
    std::string err;
    REQUIRE(proc.loadPresetFile(writeIdentityPreset(tmp.dir, "init", 0), &err));
    REQUIRE(proc.waitForLoader());
    REQUIRE(proc.status().error.empty());
  }
  void apply(const std::optional<Preset>& p) {
    REQUIRE(p.has_value());
    proc.loadPreset(*p);
    REQUIRE(proc.waitForLoader());
    REQUIRE(proc.status().error.empty());
  }
};

}  // namespace

TEST_CASE("choosing a model from a folder pack loads a preset whose cab file is that WAV", "[mic][session]") {
  ProcessorWithPreset h;
  const fs::path dir = writeIrFolder(h.tmp.dir, {"V30 UL SM57 Cap 1in", "V30 UL MD421 Cap 1in", "V30 UR SM57 Cone 1in"});
  MicSession s;
  s.setLayout(layout4x12());
  s.adopt(h.proc.currentPreset());
  std::string err;
  s.setPack(IrPack::fromFolder(dir, &err));
  REQUIRE(s.pack().size() == 3);
  CHECK(s.model(0) == -1);  // the cab still uses the init IR
  CHECK(s.pack().dots().size() == 2);

  const Preset orig = h.proc.currentPreset();
  h.apply(s.choose(h.proc.currentPreset(), 0, indexOf(s.pack(), "V30 UR SM57 Cone 1in")));
  const Preset after = h.proc.currentPreset();
  REQUIRE(after.cab.mode == CabMode::Shared);
  CHECK(after.cab.ir.file == (dir / "V30 UR SM57 Cone 1in.wav").string());
  CHECK(after.cab.ir.resolvedPath == dir / "V30 UR SM57 Cone 1in.wav");
  CHECK_FALSE(after.cab.ir.source);  // a folder pack has no attribution
  CHECK(h.proc.status().latencySamples == 0);
  CHECK(s.model(0) == indexOf(s.pack(), "V30 UR SM57 Cone 1in"));
  // everything but the cab is untouched
  Preset a = after, b = orig;
  a.cab = CabPreset{};
  b.cab = CabPreset{};
  CHECK(a == b);

  // the page notices the processor caught up: nothing to adopt
  CHECK(sameCab(h.proc.currentPreset().cab, s.build(h.proc.currentPreset()).cab));

  // A/B toggles between the last two chosen models, NEXT POSITION goes to the next dot
  h.apply(s.choose(h.proc.currentPreset(), 0, indexOf(s.pack(), "V30 UL SM57 Cap 1in")));
  CHECK(h.proc.currentPreset().cab.ir.file == (dir / "V30 UL SM57 Cap 1in.wav").string());
  REQUIRE(s.canToggleAB());
  h.apply(s.toggleAB(h.proc.currentPreset()));
  CHECK(h.proc.currentPreset().cab.ir.file == (dir / "V30 UR SM57 Cone 1in.wav").string());
  h.apply(s.toggleAB(h.proc.currentPreset()));
  CHECK(h.proc.currentPreset().cab.ir.file == (dir / "V30 UL SM57 Cap 1in.wav").string());
  h.apply(s.nextPosition(h.proc.currentPreset()));
  CHECK(h.proc.currentPreset().cab.ir.file == (dir / "V30 UR SM57 Cone 1in.wav").string());
  h.apply(s.nextPosition(h.proc.currentPreset()));  // wraps to the first dot
  CHECK(h.proc.currentPreset().cab.ir.file == (dir / "V30 UL SM57 Cap 1in.wav").string());
}

TEST_CASE("a TONE3000 manifest model becomes a Capture with source, sha256 and the cache path", "[mic][session]") {
  ProcessorWithPreset h;
  const fs::path dir = writeIrFolder(h.tmp.dir, {"V30 UL SM57 Cap 1in", "V30 UL SM57 Cone 1in"});
  json models = json::array();
  for (const char* n : {"V30 UL SM57 Cap 1in", "V30 UL SM57 Cone 1in"})
    models.push_back({{"modelId", n[10] == 'C' && n[11] == 'a' ? "11" : "12"}, {"name", n}, {"file", (dir / (std::string(n) + ".wav")).string()},
                      {"sha256", sha256File(dir / (std::string(n) + ".wav"))}});
  std::string err;
  IrPack p = IrPack::fromManifestJson(json{{"toneId", "9001"}, {"title", "Pack"}, {"creator", "me"}, {"license", "cc-by-nc"}, {"url", "https://t/9001"}, {"models", models}}.dump(), &err);
  MicSession s;
  s.setLayout(layout4x12());
  s.adopt(h.proc.currentPreset());
  s.setPack(std::move(p));
  h.apply(s.choose(h.proc.currentPreset(), 0, 1));
  const Capture& c = h.proc.currentPreset().cab.ir;
  CHECK(c.file == (dir / "V30 UL SM57 Cone 1in.wav").string());
  CHECK(c.sha256 == sha256File(dir / "V30 UL SM57 Cone 1in.wav"));
  REQUIRE(c.source);
  CHECK(c.source->id == "9001");
  CHECK(c.source->modelId == "12");
  CHECK(c.source->license == "cc-by-nc");
  CHECK(s.toneId() == "9001");
  // the processor's state (the preset JSON) carries it
  const json st = json::parse(presetToStateJson(h.proc.currentPreset()));
  CHECK(st["cab"]["ir"]["source"]["id"] == "9001");
}

TEST_CASE("BLEND on and off produce irMix and shared presets", "[mic][session]") {
  ProcessorWithPreset h;
  const fs::path dir = writeIrFolder(h.tmp.dir, {"V30 UL SM57 Cap 1in", "V30 UL MD421 Cap 1in", "V30 UR SM57 Cone 1in"});
  MicSession s;
  s.setLayout(layout4x12());
  s.adopt(h.proc.currentPreset());
  s.setPack(IrPack::fromFolder(dir));
  h.apply(s.choose(h.proc.currentPreset(), 0, indexOf(s.pack(), "V30 UL SM57 Cap 1in")));
  const std::string first = (dir / "V30 UL SM57 Cap 1in.wav").string();

  // ON: irA = the current IR, irB = the same IR (the sound does not change), mix 0.5
  h.apply(s.setBlend(h.proc.currentPreset(), true));
  Preset p = h.proc.currentPreset();
  REQUIRE(p.cab.mode == CabMode::IrMix);
  CHECK(p.cab.irA.file == first);
  CHECK(p.cab.irB.file == first);
  CHECK(p.cab.mix == 0.5);
  CHECK(h.proc.status().liveCompatible);
  CHECK(h.proc.status().info.cabMode == "irMix");
  CHECK(s.blend());

  // the second mic and the mix
  h.apply(s.choose(h.proc.currentPreset(), 1, indexOf(s.pack(), "V30 UR SM57 Cone 1in")));
  p = h.proc.currentPreset();
  CHECK(p.cab.irA.file == first);
  CHECK(p.cab.irB.file == (dir / "V30 UR SM57 Cone 1in.wav").string());
  CHECK(s.active() == 1);
  s.setMixValue(0.3);
  h.apply(s.setMix(h.proc.currentPreset(), 0.3));
  CHECK(h.proc.currentPreset().cab.mix == 0.3);
  CHECK(h.proc.currentPreset().cab.mode == CabMode::IrMix);
  // an irMix preset round-trips through the plugin state
  Preset restored = parsePreset(json::parse(presetToStateJson(h.proc.currentPreset())), "/");
  CHECK(restored.cab.mode == CabMode::IrMix);
  CHECK(restored.cab.mix == 0.3);

  // OFF: shared with irA
  h.apply(s.setBlend(h.proc.currentPreset(), false));
  p = h.proc.currentPreset();
  REQUIRE(p.cab.mode == CabMode::Shared);
  CHECK(p.cab.ir.file == first);
  CHECK(h.proc.status().info.cabMode == "shared");
  CHECK_FALSE(s.blend());

  // an irMix preset loaded from disk opens with BLEND on
  MicSession fresh;
  fresh.setLayout(layout4x12());
  h.apply(s.setBlend(h.proc.currentPreset(), true));
  fresh.adopt(h.proc.currentPreset());
  CHECK(fresh.blend());
  CHECK(fresh.mix() == 0.5);
  CHECK(fresh.capture(0).file == first);
}

TEST_CASE("perPath presets are read-only and keep their cab", "[mic][session]") {
  ProcessorWithPreset h;
  const fs::path dir = writeIrFolder(h.tmp.dir, {"V30 UL SM57 Cap 1in", "V30 UR SM57 Cone 1in"});
  Preset p = h.proc.currentPreset();
  p.cab.mode = CabMode::PerPath;
  p.cab.irA = p.cab.ir;
  p.cab.irB = p.cab.ir;
  p.cab.ir = Capture{};
  h.proc.loadPreset(p);
  REQUIRE(h.proc.waitForLoader());
  MicSession s;
  s.setLayout(layout4x12());
  s.adopt(h.proc.currentPreset());
  s.setPack(IrPack::fromFolder(dir));
  CHECK(s.readOnly());
  CHECK_FALSE(s.choose(h.proc.currentPreset(), 0, 1).has_value());
  CHECK_FALSE(s.setBlend(h.proc.currentPreset(), true).has_value());
  CHECK_FALSE(s.nextPosition(h.proc.currentPreset()).has_value());
  CHECK(sameCab(s.build(h.proc.currentPreset()).cab, h.proc.currentPreset().cab));
}

TEST_CASE("a cached manifest is found by the cab's tone id", "[mic][session]") {
  TempDir tmp;
  ::setenv("SAWBLADE_APPDATA", tmp.dir.string().c_str(), 1);
  REQUIRE(packManifestPath("555") == tmp.dir / "packs" / "555.json");
  const fs::path ir = writeIrFolder(tmp.dir, {"V30 UL SM57 Cap 1in", "V30 UR SM57 Cone 1in"});
  json models = json::array();
  for (const char* n : {"V30 UL SM57 Cap 1in", "V30 UR SM57 Cone 1in"})
    models.push_back({{"modelId", "1"}, {"name", n}, {"file", (ir / (std::string(n) + ".wav")).string()}});
  fs::create_directories(tmp.dir / "packs");
  std::ofstream(packManifestPath("555")) << json{{"toneId", "555"}, {"title", "Cached"}, {"models", models}}.dump();

  Preset p;
  p.cab.ir.file = (ir / "V30 UL SM57 Cap 1in.wav").string();
  p.cab.ir.resolvedPath = p.cab.ir.file;
  p.cab.ir.source = CaptureSource{"tone3000", "555", "1", "", "", "", ""};
  MicSession s;
  s.setLayout(layout4x12());
  s.adopt(p);  // the pack of this tone is cached: loaded on adoption
  CHECK(s.pack().info().kind == PackInfo::Kind::Manifest);
  CHECK(s.pack().info().title == "Cached");
  CHECK(s.model(0) == 0);
  // a cab from another tone drops the pack
  Preset q = p;
  q.cab.ir.source->id = "556";
  q.cab.ir.file = "/elsewhere/x.wav";
  q.cab.ir.resolvedPath = q.cab.ir.file;
  s.adopt(q);
  CHECK(s.pack().info().kind == PackInfo::Kind::Single);
  ::unsetenv("SAWBLADE_APPDATA");
  sawblade::plugin::settings::Settings::resetSharedForTests();
}

// ---- sawblade-t3k runner ------------------------------------------------------------------------------------------
namespace {

fs::path writeScript(const fs::path& dir, const std::string& name, const std::string& body) {
  const fs::path p = dir / name;
  std::ofstream(p) << "#!/bin/sh\n" << body << "\n";
  fs::permissions(p, fs::perms::owner_all);
  return p;
}

struct Run {
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  T3kTool::Result result;
  std::vector<T3kTool::Progress> progress;
  bool wait(std::chrono::milliseconds t = std::chrono::seconds(20)) {
    std::unique_lock<std::mutex> lk(m);
    return cv.wait_for(lk, t, [&] { return done; });
  }
  bool waitProgress(std::size_t n, std::chrono::milliseconds t = std::chrono::seconds(20)) {
    std::unique_lock<std::mutex> lk(m);
    return cv.wait_for(lk, t, [&] { return progress.size() >= n; });
  }
};

bool start(T3kTool& tool, Run& r, const std::vector<std::string>& args, const fs::path& exe) {
  return tool.start(
      args,
      [&r](const T3kTool::Progress& p) {
        std::lock_guard<std::mutex> lk(r.m);
        r.progress.push_back(p);
        r.cv.notify_all();
      },
      [&r](const T3kTool::Result& res) {
        std::lock_guard<std::mutex> lk(r.m);
        r.result = res;
        r.done = true;
        r.cv.notify_all();
      },
      exe);
}

}  // namespace

TEST_CASE("T3kTool: progress lines, arguments and a clean exit", "[t3k]") {
  TempDir tmp;
  const fs::path exe = writeScript(tmp.dir, "fake-t3k",
                                   "echo \"$@\" > \"" + (tmp.dir / "args.txt").string() + "\"\n"
                                   "echo '{\"done\": 1, \"total\": 3, \"name\": \"V30 UL SM57\"}'\n"
                                   "echo 'not progress'\n"
                                   "echo '{\"done\": 2, \"total\": 3, \"name\": \"b\"}'\n"
                                   "echo '{\"done\": 3, \"total\": 3, \"name\": \"c\"}'\n"
                                   "exit 0");
  Run r;  // before the tool: the tool's thread calls back into r until the tool is destroyed
  T3kTool tool;
  REQUIRE(start(tool, r, T3kTool::packArgs("1234", tmp.dir / "m.json"), exe));
  REQUIRE(r.wait());
  CHECK(r.result.status == T3kTool::Status::Ok);
  CHECK(r.result.exitCode == 0);
  REQUIRE(r.progress.size() == 3);
  CHECK(r.progress[0].done == 1);
  CHECK(r.progress[0].total == 3);
  CHECK(r.progress[0].name == "V30 UL SM57");
  CHECK(r.progress[2].done == 3);
  CHECK(r.result.output.find("not progress") != std::string::npos);
  std::ifstream a(tmp.dir / "args.txt");
  std::string line;
  std::getline(a, line);
  CHECK(line == "pack 1234 -o " + (tmp.dir / "m.json").string() + " --progress-json");
}

TEST_CASE("T3kTool: exit 4 is 'not logged in', exit 1 shows the last stderr line, other codes fail", "[t3k]") {
  TempDir tmp;
  {
    const fs::path exe = writeScript(tmp.dir, "login", "echo 'token expired' >&2\nexit 4");
    Run r;  // before the tool: the tool's thread calls back into r until the tool is destroyed
    T3kTool tool;
    REQUIRE(start(tool, r, {"pack", "1"}, exe));
    REQUIRE(r.wait());
    CHECK(r.result.status == T3kTool::Status::NotLoggedIn);
    CHECK(r.result.exitCode == 4);
    CHECK(r.result.message == "Not logged in to TONE3000. Run `sawblade-t3k login` in a terminal, then try again.");
  }
  {
    const fs::path exe = writeScript(tmp.dir, "boom", "echo 'first line' >&2\necho 'error: the pack has no models' >&2\nexit 1");
    Run r;  // before the tool: the tool's thread calls back into r until the tool is destroyed
    T3kTool tool;
    REQUIRE(start(tool, r, {"pack", "1"}, exe));
    REQUIRE(r.wait());
    CHECK(r.result.status == T3kTool::Status::Failed);
    CHECK(r.result.exitCode == 1);
    CHECK(r.result.message == "error: the pack has no models");
  }
  {
    const fs::path exe = writeScript(tmp.dir, "silent", "exit 3");
    Run r;  // before the tool: the tool's thread calls back into r until the tool is destroyed
    T3kTool tool;
    REQUIRE(start(tool, r, {"pack", "1"}, exe));
    REQUIRE(r.wait());
    CHECK(r.result.status == T3kTool::Status::Failed);
    CHECK(r.result.message.find("exit code 3") != std::string::npos);
  }
}

TEST_CASE("T3kTool: a missing executable is reported clearly, not run", "[t3k]") {
  TempDir tmp;
  Run r;  // before the tool: the tool's thread calls back into r until the tool is destroyed
  T3kTool tool;
  const fs::path exe = tmp.dir / "no" / "sawblade-t3k";
  REQUIRE(start(tool, r, {"pack", "1"}, exe));
  REQUIRE(r.wait());
  CHECK(r.result.status == T3kTool::Status::MissingExecutable);
  CHECK(r.result.message.find(exe.string()) != std::string::npos);
  CHECK(r.result.message.find("Locate") != std::string::npos);
  // a file that is not executable counts as missing too
  std::ofstream(tmp.dir / "plain.txt") << "x";
  Run r2;
  REQUIRE(start(tool, r2, {}, tmp.dir / "plain.txt"));
  REQUIRE(r2.wait());
  CHECK(r2.result.status == T3kTool::Status::MissingExecutable);
}

TEST_CASE("T3kTool: cancel kills the child and reports Cancelled", "[t3k]") {
  TempDir tmp;
  const fs::path exe = writeScript(tmp.dir, "slow", "echo '{\"done\": 1, \"total\": 9, \"name\": \"a\"}'\nexec sleep 60");
  Run r;  // before the tool: the tool's thread calls back into r until the tool is destroyed
  T3kTool tool;
  REQUIRE(start(tool, r, {"pack", "1"}, exe));
  REQUIRE(tool.running());
  CHECK_FALSE(start(tool, r, {"pack", "2"}, exe));  // one run at a time
  REQUIRE(r.waitProgress(1));
  const auto t0 = std::chrono::steady_clock::now();
  tool.cancel();
  REQUIRE(r.wait(std::chrono::seconds(10)));
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8));
  CHECK(r.result.status == T3kTool::Status::Cancelled);
  // the tool can run again afterwards
  Run r3;
  const fs::path ok = writeScript(tmp.dir, "ok", "exit 0");
  REQUIRE(start(tool, r3, {}, ok));
  REQUIRE(r3.wait());
  CHECK(r3.result.status == T3kTool::Status::Ok);
}

TEST_CASE("T3kTool: destroying the tool while a child runs cancels it", "[t3k]") {
  TempDir tmp;
  const fs::path exe = writeScript(tmp.dir, "slow", "exec sleep 60");
  const auto t0 = std::chrono::steady_clock::now();
  {
    Run r;  // before the tool: the tool's thread calls back into r until the tool is destroyed
    T3kTool tool;
    REQUIRE(start(tool, r, {}, exe));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8));
}

TEST_CASE("settings.json: the t3kExecutable key is read-modify-write and keeps unknown keys", "[t3k][settings]") {
  TempDir tmp;
  ::setenv("SAWBLADE_APPDATA", (tmp.dir / "data").string().c_str(), 1);
  HookIsolation iso;
  CHECK(settingsFile() == tmp.dir / "data" / "settings.json");
  // default: <repo>/match/.venv/bin/sawblade-t3k
  CHECK(settings::t3kExecutable() == settings::defaultT3kExecutable());
  CHECK(settings::defaultT3kExecutable().string().find("match/.venv/bin/sawblade-t3k") != std::string::npos);

  fs::create_directories(tmp.dir / "data");
  std::ofstream(settingsFile()) << R"({"presetBrowser": {"lastFolder": "/x", "n": [1, 2]}, "t3kExecutable": "/old"})";
  CHECK(settings::t3kExecutable() == fs::path("/old"));
  std::string err;
  REQUIRE(settings::setT3kExecutable("/new/sawblade-t3k", &err));
  CHECK(settings::t3kExecutable() == fs::path("/new/sawblade-t3k"));
  const json j = json::parse(std::ifstream(settingsFile()));
  CHECK(j["t3kExecutable"] == "/new/sawblade-t3k");
  CHECK(j["presetBrowser"]["lastFolder"] == "/x");  // unknown keys survive
  CHECK(j["presetBrowser"]["n"] == json::array({1, 2}));

  // a missing file is created; a file that is not a JSON object is never overwritten
  fs::remove(settingsFile());
  REQUIRE(settings::setT3kExecutable("/a", &err));
  CHECK(json::parse(std::ifstream(settingsFile()))["t3kExecutable"] == "/a");
  std::ofstream(settingsFile()) << "[1, 2, 3]";
  CHECK_FALSE(settings::setT3kExecutable("/b", &err));
  CHECK_FALSE(err.empty());
  std::ifstream in(settingsFile());
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content == "[1, 2, 3]");
  ::unsetenv("SAWBLADE_APPDATA");
  sawblade::plugin::settings::Settings::resetSharedForTests();
}

TEST_CASE("a cached-but-unresolved cab IR previews and lays out through the capture cache", "[mic][session]") {
  TempDir tmp;
  ::setenv("SAWBLADE_CACHE_DIR", (tmp.dir / "cache").string().c_str(), 1);
  fs::create_directories(tmp.dir / "cache" / "77");
  writeWavFloat32(tmp.dir / "cache" / "77" / "9.wav", 48000.0, std::vector<float>(64, 0.1f));
  Capture c;
  c.file = "captures/missing.wav";
  c.resolvedPath = tmp.dir / "nowhere" / "missing.wav";
  c.source = CaptureSource{"tone3000", "77", "9", "", "", "", ""};
  CHECK_FALSE(spectrumOfCapture(c, true).empty());
  const IrPack p = IrPack::single(c);
  REQUIRE(p.size() == 1);
  CHECK(p.models()[0].file == tmp.dir / "cache" / "77" / "9.wav");
  CHECK(p.findModel(c) == 0);
  ::unsetenv("SAWBLADE_CACHE_DIR");
}

TEST_CASE("no capture: the \"(none)\" placeholder is never a file", "[mic][init]") {
  Capture c;
  CHECK(isNoCapture(c));  // empty file
  c.file = kNoCaptureFile;
  CHECK(isNoCapture(c));
  c.file = "/tmp/(none)";  // exact match only: an absolute path is a file
  CHECK_FALSE(isNoCapture(c));
  c.file = "cab.wav";
  CHECK_FALSE(isNoCapture(c));

  const Preset init = makeInitPreset();
  CHECK(isNoCapture(init.cab.ir));
  CHECK(json::parse(presetToStateJson(init))["cab"]["ir"]["file"] == "(none)");  // not absolutised
  CHECK(summariseCaptures(init).empty());
  CHECK(IrPack::single(init.cab.ir).empty());
}

// ---- v0.3.0.1: the tool gets the client id from Settings, not from the DAW's environment -----------------------------
namespace {
// Runs the tests/fake_t3k.py CLI behind a one-line wrapper (a DAW launched from the Dock has no TONE3000_CLIENT_ID).
struct EnvTool {
  TempDir tmp;
  fs::path exe, envLog;
  std::vector<std::pair<std::string, std::optional<std::string>>> saved;
  EnvTool() {
    const fs::path py = fs::path(SAWBLADE_TEST_TOOLS_DIR).parent_path() / "fake_t3k.py";
    exe = writeScript(tmp.dir, "sawblade-t3k", "exec python3 \"" + py.string() + "\" \"$@\"");
    envLog = tmp.dir / "env.jsonl";
    set("FAKE_T3K_REQUIRE_ID", "1");
    set("FAKE_T3K_ENV_LOG", envLog.string());
  }
  void set(const char* k, const std::string& v) {
    const char* cur = std::getenv(k);
    saved.emplace_back(k, cur ? std::optional<std::string>(cur) : std::nullopt);
    ::setenv(k, v.c_str(), 1);
  }
  ~EnvTool() {
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
      if (it->second) ::setenv(it->first.c_str(), it->second->c_str(), 1);
      else ::unsetenv(it->first.c_str());
    }
  }
  std::vector<json> calls() const {
    std::vector<json> v;
    std::ifstream in(envLog);
    std::string line;
    while (std::getline(in, line))
      if (!line.empty()) v.push_back(json::parse(line));
    return v;
  }
};

T3kTool::Result runOnce(const std::vector<std::string>& args, const fs::path& exe) {
  Run r;  // before the tool: the tool's thread calls back into r until the tool is destroyed
  T3kTool tool;
  REQUIRE(start(tool, r, args, exe));
  REQUIRE(r.wait());
  return r.result;
}
}  // namespace

TEST_CASE("T3kTool: every command runs with TONE3000_CLIENT_ID from Settings when the host environment has none", "[t3k][env]") {
  SettingsEnv env{"{\"version\": 1, \"firstRunCompleted\": true, \"tone3000ClientId\": \"t3k_pub_fromsettings\"}"};  // scrubs TONE3000_CLIENT_ID
  REQUIRE(std::getenv("TONE3000_CLIENT_ID") == nullptr);
  EnvTool t;
  // the entry points the capture browser, body fill, preset fetch and ladders use all end in T3kTool::start
  const std::vector<std::vector<std::string>> commands = {
      {"whoami", "--json"}, {"fetch", "101", "--json"}, {"ladder", "101"}, {"search", "--json", "--", "hm-2"}, {"models", "101", "--json"}};
  for (const auto& args : commands) {
    const auto res = runOnce(args, t.exe);
    INFO(args.front() << ": " << res.message << " / " << res.output);
    CHECK(res.status == T3kTool::Status::Ok);
  }
  const auto calls = t.calls();
  REQUIRE(calls.size() == commands.size());
  for (const auto& c : calls) {
    CHECK(c["TONE3000_CLIENT_ID"] == "t3k_pub_fromsettings");
    CHECK(c["PYTHONUNBUFFERED"] == "1");
    CHECK(c["SAWBLADE_CACHE_DIR"] == sawblade::plugin::settings::Settings::shared().effectiveCaptureCacheDir().string());
  }
}

TEST_CASE("T3kTool: with no client id anywhere the failure message carries the tool's stderr line", "[t3k][env]") {
  SettingsEnv env{"{\"version\": 1, \"firstRunCompleted\": true}"};
  EnvTool t;
  const auto res = runOnce({"fetch", "101", "--json"}, t.exe);
  CHECK(res.status == T3kTool::Status::Failed);
  CHECK(res.exitCode == 1);
  CHECK(res.message == "TONE3000_CLIENT_ID is not set");
  CHECK(t.calls().size() == 1);
  CHECK(t.calls()[0]["TONE3000_CLIENT_ID"].is_null());
}

TEST_CASE("T3kTool: a secret key in settings or the environment is never passed to the tool", "[t3k][env]") {
  SettingsEnv env{"{\"version\": 1, \"firstRunCompleted\": true, \"tone3000ClientId\": \"t3k_cs_notapublicid\"}"};
  EnvTool t;
  t.set("TONE3000_CLIENT_ID", "t3k_cs_alsonot");  // the host environment
  const auto res = runOnce({"whoami", "--json"}, t.exe);
  CHECK(res.status == T3kTool::Status::Failed);
  CHECK(res.message.find("t3k_cs_") == std::string::npos);
  for (const auto& c : t.calls()) {
    if (c["TONE3000_CLIENT_ID"].is_string()) CHECK(c["TONE3000_CLIENT_ID"].get<std::string>().find("t3k_cs_") == std::string::npos);
  }
}

TEST_CASE("T3kTool: a path with '=' is reported, not run through env", "[t3k][env]") {
  SettingsEnv env{"{\"version\": 1, \"firstRunCompleted\": true}"};
  TempDir tmp;
  fs::create_directories(tmp.dir / "a=b");
  const fs::path exe = writeScript(tmp.dir / "a=b", "sawblade-t3k", "exit 0");
  const auto res = runOnce({"whoami"}, exe);
  CHECK(res.status == T3kTool::Status::MissingExecutable);
  CHECK(res.message.find("'='") != std::string::npos);
}

TEST_CASE("T3kRunner lastErrorLine: the last plain line, never JSON or anything credential-like", "[t3k][env]") {
  CHECK(lastErrorLine("") == "");
  CHECK(lastErrorLine("TONE3000_CLIENT_ID is not set\n") == "TONE3000_CLIENT_ID is not set");
  CHECK(lastErrorLine("{\"event\": \"device_code\"}\nTONE3000_CLIENT_ID is not set\r\n\n") == "TONE3000_CLIENT_ID is not set");
  CHECK(lastErrorLine("real error\nrefresh_token=SECRETTOKEN123456\n{\"event\": \"x\"}\n") == "real error");
  CHECK(lastErrorLine("{\"a\": 1}\n") == "");
  // long lines are cut to 300 bytes on a code-point boundary (words stay short, so the credential filter keeps them)
  std::string longLine;
  while (longLine.size() < 500) longLine += "\xc3\xa9t\xc3\xa9 ";  // "été "
  const std::string cut = lastErrorLine(longLine);
  CHECK(cut.size() <= 300);
  CHECK(cut.size() >= 298);
  CHECK((static_cast<unsigned char>(cut.back()) & 0xC0) != 0xC0);  // not the lead byte of a split sequence
  // JWT-like / opaque codes and key=<long> lines are dropped
  CHECK(lastErrorLine("ok line\neyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.abc\n") == "ok line");
  CHECK(lastErrorLine("ok line\nsession=0123456789abcdefghij\n") == "ok line");
  CHECK(lastErrorLine("ok line\nrefresh_token=abc123456789\n") == "ok line");
  CHECK(lastErrorLine("TONE3000_CLIENT_ID is not set (your publishable key, t3k_pub_...). See match/README.md\n") ==
        "TONE3000_CLIENT_ID is not set (your publishable key, t3k_pub_...). See match/README.md");
}

TEST_CASE("T3kTool: a credential-looking last line never becomes the failure message", "[t3k][env]") {
  SettingsEnv env{"{\"version\": 1, \"firstRunCompleted\": true}"};
  TempDir tmp;
  const fs::path exe = writeScript(tmp.dir, "leaky", "echo 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0' >&2\nexit 1");
  const auto res = runOnce({"whoami"}, exe);
  CHECK(res.status == T3kTool::Status::Failed);
  CHECK(res.message.find("eyJ") == std::string::npos);
  CHECK(res.message.find("exit code 1") != std::string::npos);
}
