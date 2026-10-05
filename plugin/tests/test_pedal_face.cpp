// Processor-level tests of the pedal parameters and the CIRCUIT switch (docs/specs/phase7b_chainsaw_pedal.md
// section 5.1, acceptance test 12). Driven like a host, no window.
#include <map>
#include <set>
#include <thread>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "pedals/CircuitFaces.h"
#include "pedals/CircuitParams.h"
#include "processor_harness.h"

namespace {

constexpr int hmP(int live) { return kHmFirst + live; }
constexpr int muffP(int live) { return kMuffFirst + live; }

const fs::path kChainsawDir = fs::path(SAWBLADE_PRESETS_DIR) / "modeled" / "chainsaw";

// Output of the processor for the same stationary input; the first `settle` samples are discarded.
std::vector<float> render(Host& h, const std::vector<float>& x, std::size_t settle = 4096) {
  std::vector<float> y;
  h.run(x, y, {512});
  y.erase(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(settle));
  return y;
}

// Relative RMS of the difference between two renders of the same input.
double relDiff(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  std::vector<float> d(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i];
  return rms(d.data(), d.size()) / std::max(1e-12, rms(a.data(), a.size()));
}

const std::vector<float>& testInput() {
  static const std::vector<float> x = [] {
    auto n = noise(24000, 21, 0.05f);
    const auto s = sine(220.0, 48000.0, 24000, 0.06);
    for (std::size_t i = 0; i < n.size(); ++i) n[i] += s[i];
    return n;
  }();
  return x;
}

}  // namespace

TEST_CASE("Circuit params: the parameter table follows the core descriptors", "[pedalface]") {
  REQUIRE(kNumParams == kPostEqFirst + kPostEqSlots + 1 + kHmNumLive + kMuffNumLive);
  CHECK(paramSpec(kSawCircuit).id == "sawCircuit");
  CHECK(paramSpec(kSawCircuit).choices == std::vector<std::string>{"Chainsaw", "Big Fuzz"});
  const auto hm = hmLiveParamDescs();
  const auto mf = muffLiveParamDescs();
  for (int k = 0; k < kHmNumLive; ++k) {
    const ParamSpec& s = paramSpec(kHmFirst + k);
    INFO(s.id);
    CHECK(s.min == hm[static_cast<std::size_t>(k)].min);
    CHECK(s.max == hm[static_cast<std::size_t>(k)].max);
    CHECK(s.def == hm[static_cast<std::size_t>(k)].def);
    CHECK(s.choices == hm[static_cast<std::size_t>(k)].choices);
    CHECK(s.name.rfind("Chainsaw ", 0) == 0);
  }
  for (int k = 0; k < kMuffNumLive; ++k) {
    const ParamSpec& s = paramSpec(kMuffFirst + k);
    INFO(s.id);
    CHECK(s.min == mf[static_cast<std::size_t>(k)].min);
    CHECK(s.max == mf[static_cast<std::size_t>(k)].max);
    CHECK(s.choices == mf[static_cast<std::size_t>(k)].choices);
    CHECK(s.name.rfind("Big Fuzz ", 0) == 0);
  }
  // The ids listed in the spec.
  CHECK(paramSpec(hmP(kHmLowQ)).id == "hmLowQ");
  CHECK(paramSpec(hmP(kHmPresenceFreq)).id == "hmPresenceFreq");
  CHECK(paramSpec(hmP(kHmGain1Db)).id == "hmGain1Db");
  CHECK(paramSpec(muffP(kMuffStackRatio)).id == "muffStackRatio");
  CHECK(paramSpec(muffP(kMuffGain2Db)).id == "muffGain2Db");
  CHECK(paramSpec(hmP(kHmMix)).unit == "%");
  CHECK(paramSpec(hmP(kHmLowFreq)).unit == "Hz");
  CHECK(paramSpec(hmP(kHmPresenceDb)).unit == "dB");
  // Ids are unique.
  std::set<std::string> ids;
  for (int i = 0; i < kNumParams; ++i) ids.insert(paramSpec(i).id);
  CHECK(static_cast<int>(ids.size()) == kNumParams);
}

TEST_CASE("Circuit params: field access agrees with the core's live converters", "[pedalface]") {
  HmParams h;
  h.level = 1.76; h.low = 9.5; h.tightness = 3.3; h.mix = 41.0; h.mode = HmMode::Modded; h.clip = ClipType::Led;
  h.clip2 = Clip2Type::Soft; h.lowFreq = 133.3; h.lowQ = 1.17; h.highSpread = 1.9; h.presenceDb = 11.5; h.bias = 7.0;
  float live[kHmNumLive];
  hmLiveFromParams(h, live);
  HmParams back;
  for (int i = 0; i < kHmNumLive; ++i) {
    CHECK(static_cast<float>(hmField(h, i)) == live[i]);
    setHmField(back, i, hmField(h, i));
  }
  CHECK(back == h);
  MuffParams m;
  m.volume = 3.5; m.scoop = 9.0; m.clip = ClipType::Asymmetric; m.clip2 = Clip2Type::Led; m.stackRatio = 6.25; m.gain2Db = -4.5;
  float ml[kMuffNumLive];
  muffLiveFromParams(m, ml);
  MuffParams mb;
  for (int i = 0; i < kMuffNumLive; ++i) {
    CHECK(static_cast<float>(muffField(m, i)) == ml[i]);
    setMuffField(mb, i, muffField(m, i));
  }
  CHECK(mb == m);
}

TEST_CASE("Circuit params: slot rule, switchCircuit and applyCircuitParams", "[pedalface]") {
  registerLatencyStub();
  const Preset saw = loadPresetFile(kChainsawDir / "classic_buzzsaw.json");
  const Preset two = loadPresetFile(kChainsawDir / "pickle_into_saw.json");  // muff, then hm
  const Preset init = makeInitPreset();

  CHECK_FALSE(findCircuitBlock(init).has_value());
  const auto s1 = findCircuitBlock(saw);
  REQUIRE(s1.has_value());
  CHECK(s1->path == 0);
  CHECK(s1->block == 0);
  CHECK(s1->circuit == Circuit::Chainsaw);
  const auto s2 = findCircuitBlock(two);  // the first of two circuit blocks
  REQUIRE(s2.has_value());
  CHECK(s2->circuit == Circuit::BigFuzz);

  // The two-circuit preset exposes the first; the second stays preset-static.
  ParamValues v = paramsFromPreset(two);
  CHECK(v[kSawCircuit] == 1.0);
  const auto* second = dynamic_cast<const HmBlockParams*>(two.a.blocks[1].params.get());
  REQUIRE(second != nullptr);
  v[muffP(kMuffSustain)] = 2.0;
  Preset moved = two;
  applyParams(moved, v);
  CHECK(moved.a.blocks[0].type == "pedal.muff");
  CHECK(dynamic_cast<const MuffBlockParams*>(moved.a.blocks[0].params.get())->p.sustain == 2.0);
  CHECK(moved.a.blocks[1] == two.a.blocks[1]);

  // applyCircuitParams writes the block's own set, never the block type; the other set is ignored.
  Preset q = saw;
  ParamValues w = paramsFromPreset(q);
  w[kSawCircuit] = 1.0;  // names the other circuit; the type is the processor's job
  w[hmP(kHmLow)] = 3.0;
  w[muffP(kMuffTone)] = 9.0;
  applyCircuitParams(q, w);
  CHECK(q.a.blocks[0].type == "pedal.hm");
  CHECK(dynamic_cast<const HmBlockParams*>(q.a.blocks[0].params.get())->p.low == 3.0);

  // switchCircuit: level <-> volume, mix, tightness, clip carried; the rest default.
  Preset src = saw;
  {
    auto hp = std::make_shared<HmBlockParams>();
    hp->p.level = 3.25; hp->p.mix = 62.0; hp->p.tightness = 4.5; hp->p.clip = ClipType::Led;
    hp->p.low = 10.0; hp->p.presenceDb = 14.0; hp->p.mode = HmMode::Custom;
    src.a.blocks[0].params = hp;
  }
  const Preset fuzz = switchCircuit(src, Circuit::BigFuzz);
  CHECK(fuzz.a.blocks[0].type == "pedal.muff");
  const auto* mp = dynamic_cast<const MuffBlockParams*>(fuzz.a.blocks[0].params.get());
  REQUIRE(mp != nullptr);
  MuffParams expect;
  expect.volume = 3.25; expect.mix = 62.0; expect.tightness = 4.5; expect.clip = ClipType::Led;
  CHECK(mp->p == expect);
  CHECK(fuzz.a.blocks[0].id == src.a.blocks[0].id);  // everything else about the block stays
  const Preset back = switchCircuit(fuzz, Circuit::Chainsaw);
  const auto* hb = dynamic_cast<const HmBlockParams*>(back.a.blocks[0].params.get());
  REQUIRE(hb != nullptr);
  HmParams hexp;
  hexp.level = 3.25; hexp.mix = 62.0; hexp.tightness = 4.5; hexp.clip = ClipType::Led;
  CHECK(hb->p == hexp);
  CHECK(switchCircuit(init, Circuit::BigFuzz) == init);
  CHECK(switchCircuit(saw, Circuit::Chainsaw) == saw);
}

TEST_CASE("Circuit faces: every table entry names a real parameter of its circuit", "[pedalface]") {
  for (int c = 0; c < kNumCircuits; ++c) {
    const CircuitFace& f = circuitFace(static_cast<Circuit>(c));
    const CircuitInfo& ci = circuitInfo(static_cast<Circuit>(c));
    INFO(ci.blockType);
    CHECK(std::string(f.blockType) == ci.blockType);
    auto inSet = [&](int p) { return p >= ci.firstParam && p < ci.firstParam + ci.numParams; };
    std::map<int, int> controls;  // param -> number of knobs (face + drawer)
    int knobCount = 0;
    for (const FaceKnob& k : f.knobs) {
      REQUIRE(inSet(k.param));
      ++controls[k.param];
      ++knobCount;
    }
    for (const FaceKnob& k : f.drawerKnobs) {
      REQUIRE(inSet(k.param));
      ++controls[k.param];
    }
    CHECK(f.drawerKnobs.size() <= 10);
    REQUIRE(inSet(f.clipParam));
    CHECK_FALSE(paramSpec(f.clipParam).choices.empty());
    REQUIRE(inSet(f.focus.param));
    for (const FaceKnob& s : f.drawerSwitches) {
      REQUIRE(inSet(s.param));
      CHECK_FALSE(paramSpec(s.param).choices.empty());
      ++controls[s.param];
    }
    ++controls[f.clipParam];
    // FOCUS parameter: a switch on the face plus one drawer knob. Everything else: exactly one control.
    for (int p = ci.firstParam; p < ci.firstParam + ci.numParams; ++p) {
      INFO(paramSpec(p).id);
      CHECK(controls[p] == 1);
    }
    CHECK(knobCount == 6);
    const double lo = paramSpec(f.focus.param).min, hi = paramSpec(f.focus.param).max;
    CHECK(f.focus.wideValue >= lo);
    CHECK(f.focus.wideValue <= hi);
    CHECK(f.focus.narrowValue >= lo);
    CHECK(f.focus.narrowValue <= hi);
  }
}

TEST_CASE("Pedal params: classic_buzzsaw maps onto the parameters and back", "[pedalface][params]") {
  const Preset p = loadPresetFile(kChainsawDir / "classic_buzzsaw.json");
  const auto* hm = dynamic_cast<const HmBlockParams*>(p.a.blocks[0].params.get());
  REQUIRE(hm != nullptr);
  const ParamValues v = paramsFromPreset(p);
  CHECK(v[kSawCircuit] == 0.0);
  for (int k = 0; k < kHmNumLive; ++k) {
    INFO(paramSpec(kHmFirst + k).id);
    CHECK(v[static_cast<std::size_t>(kHmFirst + k)] == snapParam(hmField(hm->p, k)));
  }
  // The muff set is at its defaults while a chainsaw block is loaded.
  for (int k = 0; k < kMuffNumLive; ++k) CHECK(v[static_cast<std::size_t>(kMuffFirst + k)] == paramSpec(kMuffFirst + k).def);
  CHECK(v[hmP(kHmLevel)] == Catch::Approx(1.76));
  CHECK(v[hmP(kHmLow)] == 10.0);

  Preset q = p;
  applyParams(q, v);
  CHECK(q == clampedToParams(p));

  Host h(48000.0, 512);
  h.load(kChainsawDir / "classic_buzzsaw.json");
  CHECK(h.param(kSawCircuit) == 0.0);
  CHECK(h.param(hmP(kHmLow)) == Catch::Approx(10.0));
  CHECK(h.param(hmP(kHmLevel)) == Catch::Approx(1.76).margin(1e-4));
  h.setParam(hmP(kHmDistortion), 7.5);
  h.setParam(hmP(kHmLowFreq), 120.0);
  h.setParam(hmP(kHmMode), 2.0);
  juce::MemoryBlock s;
  h.p.getStateInformation(s);
  const json j = json::parse(std::string(static_cast<const char*>(s.getData()), s.getSize()));
  const json& blk = j["paths"]["a"]["blocks"][0];
  CHECK(blk["type"] == "pedal.hm");
  CHECK(blk["modelVersion"] == 2);
  CHECK(blk["params"]["distortion"].get<double>() == Catch::Approx(7.5).margin(1e-4));
  CHECK(blk["params"]["lowFreq"].get<double>() == Catch::Approx(120.0).margin(1e-4));
  CHECK(blk["params"]["mode"] == "modded");
  CHECK(blk["params"]["low"].get<double>() == Catch::Approx(10.0).margin(1e-4));
}

TEST_CASE("Pedal params: moving chainsaw parameters changes the audio live, with no rebuild", "[pedalface][params][rt]") {
  Host h(48000.0, 512);
  h.load(kChainsawDir / "classic_buzzsaw.json");
  const std::uint64_t builds = h.p.engineBuilds();
  const auto x = testInput();
  render(h, x);  // warm up
  const auto base = render(h, x);
  REQUIRE(rms(base.data(), base.size()) > 1e-3);
  const auto again = render(h, x);
  CHECK(relDiff(base, again) < 1e-6);  // static parameters: the render is repeatable

  struct Move { int param; double to; const char* what; };
  for (const Move m : {Move{kHmDistortion, 3.0, "distortion"}, Move{kHmMode, 2.0, "mode"}, Move{kHmClip, 1.0, "clip"}, Move{kHmMix, 40.0, "mix"}}) {
    INFO(m.what);
    const double was = h.param(kHmFirst + m.param);
    h.setParam(kHmFirst + m.param, m.to);
    render(h, x);  // let the ramps settle
    const auto moved = render(h, x);
    CHECK(relDiff(base, moved) > 0.01);
    h.setParam(kHmFirst + m.param, was);
    render(h, x);
    const auto restored = render(h, x);
    CHECK(relDiff(base, restored) < 1e-4);  // coming back restores the sound
  }
  CHECK(h.p.engineBuilds() == builds);
  CHECK(h.allocs == 0);
  if (LockGuard::enabled()) CHECK(h.locks == 0);
}

TEST_CASE("Pedal params: sweeping every pedal parameter while audio runs allocates and locks nothing", "[pedalface][rt]") {
  for (const char* file : {"classic_buzzsaw.json", "pickle_chainsaw.json"}) {
    INFO(file);
    Host h(44100.0, 512);
    h.load(kChainsawDir / file);
    const std::uint64_t builds = h.p.engineBuilds();
    const auto x = noise(512 * 120, 5, 0.1f);
    std::vector<float> out(512);
    for (int step = 0; step < 120; ++step) {
      for (int i = kHmFirst; i < kNumParams; ++i) {
        const ParamSpec& s = paramSpec(i);
        const double ph = 0.5 + 0.5 * std::sin(0.05 * step * (i - kHmFirst + 1));
        h.setParam(i, s.min + ph * (s.max - s.min));
      }
      h.process(x.data() + step * 512, out.data(), 512);
    }
    CHECK(h.p.engineBuilds() == builds);
    CHECK(h.allocs == 0);
    if (LockGuard::enabled()) CHECK(h.locks == 0);
    CHECK_FALSE(h.nonFinite);
  }
}

TEST_CASE("Pedal params: Init has no circuit block, so the parameters and the switch are inert", "[pedalface][params]") {
  Host h(48000.0, 512);
  CHECK_FALSE(findCircuitBlock(h.p.currentPreset()).has_value());
  CHECK_FALSE(h.p.circuitSlot().has_value());
  const std::uint64_t builds = h.p.engineBuilds();
  const auto x = noise(8000, 3, 0.3f);
  std::vector<float> y;
  h.run(x, y, {256});
  CHECK(y == x);
  h.setParam(hmP(kHmDistortion), 9.0);
  h.setParam(muffP(kMuffScoop), 9.0);
  h.setParam(kSawCircuit, 1.0);
  REQUIRE(h.p.waitForLoader());
  h.run(x, y, {256});
  CHECK(y == x);
  CHECK(h.p.engineBuilds() == builds);
  CHECK(h.p.currentPreset().a.blocks.empty());
  CHECK(h.p.currentPreset().name == "Init");
}

TEST_CASE("Pedal params: a modelVersion 1 preset reads the v2 defaults", "[pedalface][params]") {
  const Preset p = loadPresetFile(fs::path(SAWBLADE_PRESETS_DIR) / "modeled" / "hm_chainsaw.json");
  const auto* hm = dynamic_cast<const HmBlockParams*>(findCircuitBlock(p) ? p.a.blocks[0].params.get() : nullptr);
  REQUIRE(hm != nullptr);
  Host h(48000.0, 512);
  h.load(fs::path(SAWBLADE_PRESETS_DIR) / "modeled" / "hm_chainsaw.json");
  CHECK(h.param(kSawCircuit) == 0.0);
  const HmParams def;
  CHECK(h.param(hmP(kHmLevel)) == Catch::Approx(hm->p.level).margin(1e-4));
  CHECK(h.param(hmP(kHmLow)) == Catch::Approx(hm->p.low).margin(1e-4));
  CHECK(h.param(hmP(kHmDistortion)) == Catch::Approx(hm->p.distortion).margin(1e-4));
  CHECK(h.param(hmP(kHmTightness)) == def.tightness);
  CHECK(h.param(hmP(kHmMix)) == def.mix);
  CHECK(h.param(hmP(kHmMode)) == 0.0);
  CHECK(h.param(hmP(kHmClip)) == 0.0);
  CHECK(h.param(hmP(kHmClip2)) == 0.0);
  CHECK(h.param(hmP(kHmLowFreq)) == def.lowFreq);
  CHECK(h.param(hmP(kHmLowQ)) == Catch::Approx(def.lowQ).margin(1e-6));
  CHECK(h.param(hmP(kHmHighSpread)) == Catch::Approx(def.highSpread).margin(1e-6));
  CHECK(h.param(hmP(kHmPresenceFreq)) == def.presenceFreq);
  CHECK(h.param(hmP(kHmRolloffHz)) == def.rolloffHz);
  CHECK(h.param(hmP(kHmBias)) == def.bias);
  juce::MemoryBlock s;
  h.p.getStateInformation(s);
  const json j = json::parse(std::string(static_cast<const char*>(s.getData()), s.getSize()));
  CHECK(j["paths"]["a"]["blocks"][0]["modelVersion"] == 2);  // saved as v2
}

TEST_CASE("Pedal params: pickle_chainsaw maps onto the big-fuzz set and scoop moves the audio", "[pedalface][params]") {
  Host h(48000.0, 512);
  h.load(kChainsawDir / "pickle_chainsaw.json");
  CHECK(h.param(kSawCircuit) == 1.0);
  CHECK(h.param(muffP(kMuffVolume)) == Catch::Approx(6.0));
  CHECK(h.param(muffP(kMuffSustain)) == Catch::Approx(10.0));
  CHECK(h.param(muffP(kMuffTone)) == Catch::Approx(4.0));
  CHECK(h.param(muffP(kMuffScoop)) == Catch::Approx(8.0));
  CHECK(h.param(muffP(kMuffRolloffHz)) == Catch::Approx(6000.0));
  CHECK(h.param(hmP(kHmLow)) == paramSpec(hmP(kHmLow)).def);  // the other set stays at its defaults
  const std::uint64_t builds = h.p.engineBuilds();
  const auto x = testInput();
  render(h, x);
  const auto base = render(h, x);
  REQUIRE(rms(base.data(), base.size()) > 1e-3);
  h.setParam(muffP(kMuffScoop), 0.0);
  render(h, x);
  const auto moved = render(h, x);
  CHECK(relDiff(base, moved) > 0.02);
  CHECK(h.p.engineBuilds() == builds);
  CHECK(h.allocs == 0);
}

TEST_CASE("Pedal params: the CIRCUIT switch rebuilds exactly once and carries the shared knobs", "[pedalface][params]") {
  Host h(48000.0, 512);
  h.load(kChainsawDir / "classic_buzzsaw.json");
  // Give the chainsaw block distinctive values for the carried knobs, and for a few that must not be carried.
  h.setParam(hmP(kHmLevel), 3.25);
  h.setParam(hmP(kHmMix), 62.0);
  h.setParam(hmP(kHmTightness), 4.5);
  h.setParam(hmP(kHmClip), 1.0);  // led
  h.setParam(hmP(kHmPresenceDb), 14.0);
  h.setParam(hmP(kHmMode), 1.0);
  const std::uint64_t builds = h.p.engineBuilds();
  const auto x = testInput();
  const auto before = render(h, x);

  h.setParam(kSawCircuit, 1.0);
  REQUIRE(h.p.waitForLoader());
  REQUIRE(h.p.status().error.empty());
  CHECK(h.p.engineBuilds() == builds + 1);

  const Preset fuzz = h.p.currentPreset();
  REQUIRE(fuzz.a.blocks.size() == 1);
  CHECK(fuzz.a.blocks[0].type == "pedal.muff");
  const auto* mp = dynamic_cast<const MuffBlockParams*>(fuzz.a.blocks[0].params.get());
  REQUIRE(mp != nullptr);
  MuffParams expect;
  expect.volume = 3.25; expect.mix = 62.0; expect.tightness = 4.5; expect.clip = ClipType::Led;
  CHECK(mp->p == expect);
  CHECK(h.param(kSawCircuit) == 1.0);
  CHECK(h.param(muffP(kMuffVolume)) == Catch::Approx(3.25));
  CHECK(h.param(muffP(kMuffSustain)) == paramSpec(muffP(kMuffSustain)).def);
  CHECK(h.param(hmP(kHmPresenceDb)) == paramSpec(hmP(kHmPresenceDb)).def);  // the old set is back at defaults
  CHECK(h.p.circuitSlot()->circuit == Circuit::BigFuzz);

  // The new circuit actually plays (a different sound), after the swap is adopted.
  render(h, x);
  const auto after = render(h, x);
  CHECK(rms(after.data(), after.size()) > 1e-3);
  CHECK(relDiff(before, after) > 0.1);

  // The state round-trips: a fresh processor restores the big-fuzz block and its values.
  juce::MemoryBlock s;
  h.p.getStateInformation(s);
  const json j = json::parse(std::string(static_cast<const char*>(s.getData()), s.getSize()));
  CHECK(j["paths"]["a"]["blocks"][0]["type"] == "pedal.muff");
  CHECK(j["paths"]["a"]["blocks"][0]["params"]["volume"].get<double>() == Catch::Approx(3.25));
  SawbladeProcessor b;
  b.setStateInformation(s.getData(), static_cast<int>(s.getSize()));
  CHECK(b.parameters().getRawParameterValue("sawCircuit")->load() == 1.0f);
  CHECK(b.circuitSlot()->circuit == Circuit::BigFuzz);
  juce::MemoryBlock s2;
  b.getStateInformation(s2);
  CHECK(std::string(static_cast<const char*>(s2.getData()), s2.getSize()) == std::string(static_cast<const char*>(s.getData()), s.getSize()));

  // Setting it back restores a pedal.hm block: defaults except the carried ones.
  h.setParam(kSawCircuit, 0.0);
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.engineBuilds() == builds + 2);
  const Preset saw = h.p.currentPreset();
  CHECK(saw.a.blocks[0].type == "pedal.hm");
  const auto* hp = dynamic_cast<const HmBlockParams*>(saw.a.blocks[0].params.get());
  REQUIRE(hp != nullptr);
  HmParams hexp;
  hexp.level = 3.25; hexp.mix = 62.0; hexp.tightness = 4.5; hexp.clip = ClipType::Led;
  CHECK(hp->p == hexp);
  CHECK(h.param(kSawCircuit) == 0.0);

  // Setting the circuit it already is changes nothing.
  h.setParam(kSawCircuit, 0.0);
  CHECK(h.p.engineBuilds() == builds + 2);
}

TEST_CASE("Pedal params: the CIRCUIT switch works while audio runs and the engine swap stays clean", "[pedalface][swap][rt]") {
  Host h(48000.0, 512);
  h.load(kChainsawDir / "classic_buzzsaw.json");
  const auto x = noise(512 * 200, 9, 0.1f);
  std::vector<float> out(512);
  for (int step = 0; step < 200; ++step) {
    if (step == 20) h.setParam(kSawCircuit, 1.0);
    if (step == 100) h.setParam(kSawCircuit, 0.0);
    h.process(x.data() + step * 512, out.data(), 512);
    if (step == 60 || step == 150) REQUIRE(h.p.waitForLoader());
  }
  REQUIRE(h.p.waitForLoader());
  CHECK(h.allocs == 0);
  if (LockGuard::enabled()) CHECK(h.locks == 0);
  CHECK_FALSE(h.nonFinite);
  CHECK(h.p.circuitSlot()->circuit == Circuit::Chainsaw);
}

TEST_CASE("Pedal params: the CIRCUIT switch set from another thread is retried by the message-thread timer", "[pedalface][swap]") {
  Host h(48000.0, 512);
  h.load(kChainsawDir / "classic_buzzsaw.json");
  const std::uint64_t builds = h.p.engineBuilds();
  std::thread t([&] { h.setParam(kSawCircuit, 1.0); });  // not the message thread: flagged, not handled inline
  t.join();
  CHECK(h.p.engineBuilds() == builds);
  for (int i = 0; i < 40 && h.p.engineBuilds() == builds; ++i) {
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    h.p.waitForLoader();
  }
  REQUIRE(h.p.waitForLoader());
  CHECK(h.p.engineBuilds() == builds + 1);
  CHECK(h.p.circuitSlot()->circuit == Circuit::BigFuzz);
  CHECK(h.p.currentPreset().a.blocks[0].type == "pedal.muff");
}

TEST_CASE("Pedal params: a failed circuit-switch build writes the circuit parameters back", "[pedalface][swap]") {
  TempDir t;
  const fs::path nam = t.dir / "model.nam";
  fs::copy_file(kFixtures / "nam" / "linear_identity.nam", nam);
  json j = json::parse(std::ifstream(kChainsawDir / "classic_buzzsaw.json"));
  j["paths"]["b"] = {{"enabled", true}, {"blocks", json::array({{{"id", "b1"}, {"type", "nam"}, {"model", {{"file", nam.string()}}}}})}};
  j["cab"]["ir"]["file"] = (kFixtures / "ir" / "impulse.wav").string();
  const fs::path file = t.dir / "with_nam.json";
  std::ofstream(file) << j.dump(2);
  Host h(48000.0, 512);
  h.load(file);
  REQUIRE(h.param(kSawCircuit) == 0.0);
  fs::remove(nam);  // the switch rebuilds everything: the model is gone, so the build fails
  h.setParam(kSawCircuit, 1.0);
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(h.p.status().error.empty());
  CHECK(h.p.circuitSlot()->circuit == Circuit::Chainsaw);
  CHECK(h.p.currentPreset().a.blocks[0].type == "pedal.hm");
  CHECK(h.param(kSawCircuit) == 0.0);  // the lever follows the sounding circuit again
}

TEST_CASE("Pedal params: overlapping loads of different circuits do not trigger a spurious switch", "[pedalface][swap]") {
  Host h(48000.0, 512);
  // The mechanism, deterministically: a load commits its own sawCircuit value from the loader thread, and
  // that write is not an edit, so nothing is left for the timer to "retry" (it would act on whichever
  // preset is wanted by then and replace it by the other circuit's version of itself).
  for (const char* file : {"pickle_chainsaw.json", "classic_buzzsaw.json", "pickle_into_saw.json", "classic_buzzsaw.json"}) {
    h.load(kChainsawDir / file);
    CHECK_FALSE(h.p.circuitEditPending());
  }
  // A real edit from another thread is still flagged.
  std::thread([&] { h.setParam(kSawCircuit, 1.0); }).join();
  CHECK(h.p.circuitEditPending());
  juce::MessageManager::getInstance()->runDispatchLoopUntil(250);
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(h.p.circuitEditPending());

  for (int round = 0; round < 5; ++round) {
    INFO("round " << round);
    h.load(kChainsawDir / "classic_buzzsaw.json");
    const std::uint64_t builds = h.p.engineBuilds();
    // A (big fuzz) is committing while B (chainsaw) is already the wanted preset.
    h.p.loadPreset(loadPresetFile(kChainsawDir / "pickle_chainsaw.json"));
    h.p.loadPreset(loadPresetFile(kChainsawDir / "classic_buzzsaw.json"));
    REQUIRE(h.p.waitForLoader());
    for (int i = 0; i < 6; ++i) {  // past the 10 Hz circuit timer
      juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
      REQUIRE(h.p.waitForLoader());
    }
    CHECK(h.p.currentPreset().name == "Classic Buzzsaw");
    CHECK(h.p.currentPreset().a.blocks[0].type == "pedal.hm");
    CHECK(h.param(kSawCircuit) == 0.0);
    CHECK(h.p.engineBuilds() <= builds + 2);  // the two loads (A may be superseded), never an extra switch build
  }
}
