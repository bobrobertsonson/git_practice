// v0.8 I3: input-level drift check (docs/specs/v0_8-I3-drift_check.md). Everything is synthetic.
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numbers>
#include <random>
#include <vector>

#include "alloc_guard.h"
#include "sawblade/chain.h"
#include "sawblade/drift.h"
#include "sawblade/render.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;
const fs::path kNam = fs::path(SAWBLADE_FIXTURES_DIR) / "nam";
const fs::path kPresets = fs::path(SAWBLADE_FIXTURES_DIR) / "presets";

// Linear identity on path A, absolute gate at -55 dBFS, optional INPUT gain (the gate is keyed after it).
json mkPreset(double inputDb = 0.0, bool gate = true) {
  json pa = {{"blocks", json::array({{{"id", "a1"}, {"type", "nam"}, {"model", {{"file", (kNam / "linear_identity.nam").string()}}}}})}};
  json pb = {{"enabled", false}, {"blocks", json::array({{{"id", "b1"}, {"type", "nam"}, {"model", {{"file", (kNam / "linear_identity.nam").string()}}}}})}};
  json j = {{"schema", "sawblade.preset"}, {"version", 2}, {"name", "drift"}, {"paths", {{"a", pa}, {"b", pb}}},
            {"align", {{"mode", "off"}}}, {"blend", 0.0},
            {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}};
  if (gate) j["gate"] = {{"enabled", true}, {"thresholdDb", -55.0}, {"hysteresisDb", 6.0}};
  if (inputDb != 0.0) j["input"] = {{"gainDb", inputDb}};
  return j;
}

std::unique_ptr<Chain> build(const json& j, bool tap, int maxBlock = 512) {
  const Preset p = parsePreset(j, kPresets);
  auto ch = std::make_unique<Chain>(p, loadResources(p, kFs));
  ch->prepare({kFs, maxBlock});
  ch->setDriftTapEnabled(tap);
  return ch;
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

// A played DI: 0.4 s notes (300 Hz, peak between -30 and -18 dBFS, 5 ms ramps) every 0.7 s, -75 dBFS noise in between (below the
// gate). `gainDb` scales everything, like an interface gain knob.
std::vector<float> playing(double seconds, double gainDb, unsigned seed = 7) {
  const auto n = static_cast<std::size_t>(seconds * kFs);
  std::vector<float> x = noise(n, seed + 100, static_cast<float>(std::pow(10.0, -75.0 / 20.0)));
  std::mt19937 g(seed);
  std::uniform_real_distribution<double> lvl(-30.0, -18.0);
  const auto noteLen = static_cast<std::size_t>(0.4 * kFs), period = static_cast<std::size_t>(0.7 * kFs), ramp = static_cast<std::size_t>(0.005 * kFs);
  for (std::size_t s = 0; s + noteLen <= n; s += period) {
    const double amp = std::pow(10.0, lvl(g) / 20.0);
    for (std::size_t i = 0; i < noteLen; ++i) {
      const double env = std::min({1.0, static_cast<double>(i) / static_cast<double>(ramp), static_cast<double>(noteLen - i) / static_cast<double>(ramp)});
      x[s + i] = static_cast<float>(amp * env * std::sin(2.0 * std::numbers::pi * 300.0 * static_cast<double>(i) / kFs));
    }
  }
  const auto k = static_cast<float>(std::pow(10.0, gainDb / 20.0));
  for (auto& v : x) v *= k;
  return x;
}

std::vector<std::uint16_t> windowsOf(const drift::PeakTap& tap) {
  std::vector<std::uint16_t> all;
  std::uint32_t cur = 0;
  std::uint16_t buf[64];
  for (int n; (n = tap.read(cur, buf, 64)) > 0;) all.insert(all.end(), buf, buf + n);
  return all;
}

double rollingP95Of(const json& preset, const std::vector<float>& x, int block = 256) {
  auto ch = build(preset, true);
  run(*ch, x, block);
  drift::DriftTracker t;
  t.consume(ch->driftTap());
  const auto p = t.rollingP95Db();
  REQUIRE(p.has_value());
  return *p;
}

// ---- tracker helpers: windows are 50 ms of played audio ----
// A "playing style": a fixed cycle of 600 windows (30 s) whose levels spread over 6 dB below the peak level. Because the cycle is as long as
// the rolling window, every full rolling window holds the same set of levels, so the p95 is exactly reproducible (a +6 dB gain knob is
// exactly 24 bins).
constexpr int kPerS = 20;
struct Player {
  std::array<int, 600> off{};
  int idx = 0;
  Player() {
    std::mt19937 g(11);
    std::uniform_int_distribution<int> d(-24, 0);
    for (auto& o : off) o = d(g);
  }
  void play(drift::DriftTracker& t, double seconds, double peakDb) {
    const int base = drift::binForDb(peakDb);
    for (int i = 0; i < static_cast<int>(seconds * kPerS); ++i) t.addWindow(base + off[static_cast<std::size_t>(idx++ % 600)]);
  }
};
drift::DriftTracker learned(Player& p, double levelDb = -20.0) {
  drift::DriftTracker t;
  p.play(t, 60.0, levelDb);
  REQUIRE(t.takeNewBaseline().has_value());
  return t;
}

}  // namespace

// ---- statistic (core, audio thread side) ----------------------------------------------------------------------------
TEST_CASE("Drift I3: a DI at +0, +6 and -6 dB shifts the played p95 by the true offset within 0.5 dB", "[drift][chain]") {
  const json p = mkPreset();
  const double base = rollingP95Of(p, playing(40.0, 0.0));
  const double up = rollingP95Of(p, playing(40.0, 6.0));
  const double down = rollingP95Of(p, playing(40.0, -6.0));
  CHECK(std::fabs((up - base) - 6.0) <= 0.5);
  CHECK(std::fabs((down - base) + 6.0) <= 0.5);
  CHECK(base > -31.0);
  CHECK(base < -17.0);
}

TEST_CASE("Drift I3: the statistic is the DI, so the user's INPUT gain does not move it", "[drift][chain]") {
  const auto x = playing(40.0, 0.0);
  const double a = rollingP95Of(mkPreset(0.0), x);
  const double b = rollingP95Of(mkPreset(6.0), x);  // INPUT +6 dB: the gate still sees the same notes, the DI is unchanged
  CHECK(std::fabs(a - b) <= 0.25);
}

TEST_CASE("Drift I3: frames with the gate closed are excluded", "[drift][chain]") {
  // PeakTap level: a loud burst in a closed region adds no window and does not move the p95.
  const auto x = playing(20.0, 0.0);
  std::vector<std::uint8_t> open(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) open[i] = std::fabs(x[i]) > 0.003f ? 1 : 0;  // a stand-in gate: notes open, floor closed
  auto loud = x;
  for (std::size_t i = 0; i < loud.size(); ++i)  // 1 s of -3 dBFS noise at 5.0 s .. 6.0 s, where the stand-in gate is closed
    if (i >= static_cast<std::size_t>(5.0 * kFs) && i < static_cast<std::size_t>(6.0 * kFs)) {
      loud[i] = 0.7f * ((i % 2) ? 1.0f : -1.0f);
      open[i] = 0;
    }
  drift::PeakTap a, b;
  a.prepare(kFs);
  b.prepare(kFs);
  a.setEnabled(true);
  b.setEnabled(true);
  a.process(x.data(), open.data(), static_cast<int>(x.size()));
  // the same notes but with the burst present in the DI; the burst region is closed, but zero the quiet-floor bins the same way
  std::vector<std::uint8_t> open2 = open;
  b.process(loud.data(), open2.data(), static_cast<int>(loud.size()));
  const auto wa = windowsOf(a), wb = windowsOf(b);
  REQUIRE(!wa.empty());
  // the burst removed whole closed windows only; the windows around it are untouched, so the maximum bin is the same
  CHECK(*std::max_element(wa.begin(), wa.end()) == *std::max_element(wb.begin(), wb.end()));
  CHECK(*std::max_element(wb.begin(), wb.end()) < drift::binForDb(-10.0));

  // Chain level: with the gate held closed by a very low INPUT the key never opens it, so a loud DI makes no window at all.
  auto ch = build(mkPreset(-60.0), true);
  run(*ch, playing(10.0, 0.0), 256);
  CHECK(ch->driftTap().written() == 0u);
}

TEST_CASE("Drift I3: no statistic when the gate is off, and none when the tap is off", "[drift][chain]") {
  auto noGate = build(mkPreset(0.0, false), true);
  run(*noGate, playing(5.0, 0.0), 256);
  CHECK(noGate->driftTap().written() == 0u);
  auto off = build(mkPreset(), false);
  run(*off, playing(5.0, 0.0), 256);
  CHECK(off->driftTap().written() == 0u);
}

TEST_CASE("Drift I3: the windows do not depend on the block size", "[drift][chain]") {
  const auto x = playing(12.0, 0.0);
  auto ref = build(mkPreset(), true, 4096);
  run(*ref, x, 4096);
  const auto want = windowsOf(ref->driftTap());
  REQUIRE(want.size() > 50);
  for (int block : {1, 37, 480, 1000, 4096}) {
    auto ch = build(mkPreset(), true, 512);  // chunked internally at 512
    run(*ch, x, block);
    CHECK(windowsOf(ch->driftTap()) == want);
  }
}

TEST_CASE("Drift I3: tap on or off renders bit-identically, and process() allocates nothing", "[drift][chain][alloc]") {
  const auto x = playing(6.0, 0.0);
  auto a = build(mkPreset(), false);
  auto b = build(mkPreset(), true);
  const auto ya = run(*a, x, 256);
  std::vector<float> yb(x.size());
  {
    AllocGuard g;
    for (std::size_t pos = 0; pos < x.size(); pos += 256) {
      const auto n = static_cast<int>(std::min<std::size_t>(256, x.size() - pos));
      b->process(x.data() + pos, yb.data() + pos, n);
    }
    CHECK(g.count() == 0);
  }
  SAWBLADE_REQUIRE_SAME_SAMPLES(ya, yb);
  CHECK(b->driftTap().written() > 0u);
}

TEST_CASE("Drift I3: windows overwritten before they are read are skipped, not replayed", "[drift]") {
  drift::PeakTap tap;
  tap.prepare(1000.0);  // 50-sample windows
  tap.setEnabled(true);
  std::vector<float> di(50, 0.1f);
  std::vector<std::uint8_t> open(50, 1);
  for (int i = 0; i < drift::kRing + 100; ++i) tap.process(di.data(), open.data(), 50);
  std::uint32_t cur = 0;
  std::vector<std::uint16_t> out(2000);
  CHECK(tap.read(cur, out.data(), 2000) == drift::kRing);
  CHECK(tap.read(cur, out.data(), 2000) == 0);
}

// ---- tracker (message thread) ----------------------------------------------------------------------------------------
TEST_CASE("Drift I3: a baseline is learned from at least 60 s of played windows and not before", "[drift][tracker]") {
  Player p;
  drift::DriftTracker t;
  CHECK(t.learning());
  p.play(t, 59.0, -20.0);
  CHECK(t.learning());
  CHECK_FALSE(t.takeNewBaseline().has_value());
  p.play(t, 1.0, -20.0);
  CHECK_FALSE(t.learning());
  const auto b = t.takeNewBaseline();
  REQUIRE(b.has_value());
  CHECK(std::fabs(*b - (-20.0)) < 0.6);
  CHECK_FALSE(t.takeNewBaseline().has_value());  // once
  t.setBaseline(*b);                             // the settings value coming back is a no-op
  CHECK(t.baselineDb() == b);
  CHECK(t.rollingS() >= 29.9);
  CHECK_FALSE(t.notice().active);
}

TEST_CASE("Drift I3: +6 dB sustained raises the notice, +6 dB for 20 s does not", "[drift][tracker]") {
  Player p;
  auto t = learned(p);
  p.play(t, 20.0, -20.0 + 6.0);
  CHECK_FALSE(t.notice().active);
  p.play(t, 70.0, -20.0);  // back to normal: the rolling p95 falls, the sustain resets
  CHECK_FALSE(t.notice().active);
  CHECK(t.sustainedS() == 0.0);

  // The 30 s rolling p95 reaches +6 only after much of the window is new (here ~10 s), and the shift must then hold for 30 s.
  Player q;
  auto u = learned(q);
  q.play(u, 29.0, -20.0 + 6.0);
  CHECK_FALSE(u.notice().active);
  q.play(u, 36.0, -20.0 + 6.0);
  const auto n = u.notice();
  CHECK(n.active);
  CHECK(n.hotter);
  CHECK(n.db == 6);
  CHECK(drift::driftNoticeText(n).find("hotter") != std::string::npos);
}

TEST_CASE("Drift I3: a bigger step is noticed sooner, and -8 dB says quieter in whole dB", "[drift][tracker]") {
  Player p;
  auto t = learned(p);
  p.play(t, 40.0, -20.0 + 12.0);  // +12: the rolling p95 crosses within a few seconds, then 30 s sustained
  CHECK(t.notice().active);
  CHECK(t.notice().db == 12);

  Player q;
  auto d = learned(q);
  q.play(d, 70.0, -20.0 - 8.0);
  const auto n = d.notice();
  REQUIRE(n.active);
  CHECK_FALSE(n.hotter);
  CHECK(n.db == 8);
  const std::string s = drift::driftNoticeText(n);
  CHECK(s.find("quieter") != std::string::npos);
  CHECK(s.find("~8 dB") != std::string::npos);
}

TEST_CASE("Drift I3: playing dynamics within 4 dB over minutes never raise it", "[drift][tracker]") {
  Player p;
  auto t = learned(p);
  bool ever = false;
  for (int rep = 0; rep < 16; ++rep) {  // alternating loud (+4) and soft (-4) passages of 15 s
    for (int i = 0; i < 15 * kPerS; ++i) {
      drift::DriftTracker& tr = t;
      p.play(tr, 1.0 / kPerS, -20.0 + (rep % 2 ? 4.0 : -4.0));
      ever = ever || tr.notice().active;
    }
  }
  CHECK_FALSE(ever);
}

TEST_CASE("Drift I3: silence neither counts toward the 30 s nor resets it", "[drift][tracker]") {
  // Silence makes no windows at all (the tap only counts played frames), so the tracker's clock is played time.
  drift::PeakTap tap;
  tap.prepare(kFs);
  tap.setEnabled(true);
  std::vector<float> z(48000, 0.0f);
  std::vector<std::uint8_t> closed(48000, 0);
  for (int i = 0; i < 30; ++i) tap.process(z.data(), closed.data(), 48000);
  CHECK(tap.written() == 0u);

  Player p;
  auto t = learned(p);
  p.play(t, 20.0, -20.0 + 8.0);
  const double before = t.sustainedS();
  CHECK(before > 8.0);
  CHECK(before < 20.0);
  CHECK_FALSE(t.notice().active);
  for (int i = 0; i < 5; ++i) CHECK(t.consume(tap) == 0);  // a long silence in between: nothing changes
  CHECK(t.sustainedS() == before);
  p.play(t, 25.0, -20.0 + 8.0);
  CHECK(t.notice().active);
}

TEST_CASE("Drift I3: Ignore silences it until the drift moves another 6 dB", "[drift][tracker]") {
  Player p;
  auto t = learned(p);
  p.play(t, 70.0, -12.0);  // +8
  REQUIRE(t.notice().active);
  t.ignore();
  CHECK_FALSE(t.notice().active);
  CHECK(t.ignored());
  p.play(t, 90.0, -12.0);  // stays: silent
  CHECK_FALSE(t.notice().active);
  p.play(t, 90.0, -12.0 + 4.0);  // 4 dB further: still silent
  CHECK_FALSE(t.notice().active);
  p.play(t, 90.0, -12.0 + 7.0);  // 7 dB further: raised again, about the whole drift
  CHECK(t.notice().active);
  CHECK(t.notice().hotter);
  CHECK(t.notice().db == 15);
}

TEST_CASE("Drift I3: Ignore is forgotten when the level returns, and a changed baseline restarts everything", "[drift][tracker]") {
  Player p;
  auto t = learned(p);
  p.play(t, 70.0, -12.0);
  REQUIRE(t.notice().active);
  t.ignore();
  p.play(t, 70.0, -20.0);  // the user fixed the gain
  CHECK_FALSE(t.ignored());
  p.play(t, 70.0, -12.0);  // and moved it again: raised afresh
  CHECK(t.notice().active);

  t.setBaseline(-30.0);  // the record changed
  CHECK_FALSE(t.notice().active);
  CHECK_FALSE(t.ignored());
  CHECK(t.rollingS() == 0.0);
  CHECK(t.baselineDb().value() == -30.0);
  t.setBaseline(std::nullopt);  // re-picked: cleared, learns again
  CHECK(t.learning());
}

TEST_CASE("Drift I3: a tracker follows a new tap from its start", "[drift][tracker]") {
  drift::PeakTap a, b;
  for (auto* t : {&a, &b}) {
    t->prepare(1000.0);
    t->setEnabled(true);
  }
  std::vector<float> di(50, 0.1f);
  std::vector<std::uint8_t> open(50, 1);
  for (int i = 0; i < 20; ++i) a.process(di.data(), open.data(), 50);
  for (int i = 0; i < 7; ++i) b.process(di.data(), open.data(), 50);
  drift::DriftTracker t;
  CHECK(t.consume(a) == 20);
  CHECK(t.consume(a) == 0);
  CHECK(t.consume(b) == 7);  // the engine was rebuilt: a new tap
}
