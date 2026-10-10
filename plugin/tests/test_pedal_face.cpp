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
constexpr int hmxP(int live) { return kHmxFirst + live; }
constexpr int eyeP(int live) { return kEyeFirst + live; }

const fs::path kChainsawDir = fs::path(SAWBLADE_PRESETS_DIR) / "modeled" / "chainsaw";
const fs::path kHmxDir = fs::path(SAWBLADE_PRESETS_DIR) / "modeled" / "hmx";
const fs::path kEyeDir = fs::path(SAWBLADE_PRESETS_DIR) / "modeled" / "eye";

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
  REQUIRE(kNumParams == kPostEqFirst + kPostEqSlots + 1 + kHmNumLive + kMuffNumLive + kHmxNumLive + kEyeNumLive);
  CHECK(static_cast<int>(kHmxFirst) == static_cast<int>(kMuffFirst) + static_cast<int>(kMuffNumLive));
  CHECK(static_cast<int>(kEyeFirst) == static_cast<int>(kHmxFirst) + static_cast<int>(kHmxNumLive));
  CHECK(paramSpec(kSawCircuit).id == "sawCircuit");
  CHECK(paramSpec(kSawCircuit).choices == std::vector<std::string>{"Chainsaw", "Big Fuzz", "Modded Saw", "One-Knob Saw"});
  CHECK(kNumCircuits == 4);
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
  const auto hx = hmxLiveParamDescs();
  const auto ey = eyeLiveParamDescs();
  for (int k = 0; k < kHmxNumLive; ++k) {
    const ParamSpec& s = paramSpec(kHmxFirst + k);
    INFO(s.id);
    CHECK(s.min == hx[static_cast<std::size_t>(k)].min);
    CHECK(s.max == hx[static_cast<std::size_t>(k)].max);
    CHECK(s.def == hx[static_cast<std::size_t>(k)].def);
    if (k != kHmxMidVoice) CHECK(s.choices == hx[static_cast<std::size_t>(k)].choices);  // midVoice reads Stock / Low / High
    CHECK(s.name.rfind("Modded Saw ", 0) == 0);
  }
  for (int k = 0; k < kEyeNumLive; ++k) {
    const ParamSpec& s = paramSpec(kEyeFirst + k);
    INFO(s.id);
    CHECK(s.min == ey[static_cast<std::size_t>(k)].min);
    CHECK(s.max == ey[static_cast<std::size_t>(k)].max);
    CHECK(s.def == ey[static_cast<std::size_t>(k)].def);
    CHECK(s.name.rfind("One-Knob Saw ", 0) == 0);
  }
  // The 7c ids listed in the spec 2.3, in order.
  const char* hmxIds[] = {"hmxLevel", "hmxLow", "hmxLowMid", "hmxHighMid", "hmxHigh", "hmxDistortion", "hmxPresence",
                          "hmxTightness", "hmxMix", "hmxClip", "hmxBoost", "hmxLowMidFreq", "hmxHighMidFreq", "hmxMidVoice"};
  for (int k = 0; k < kHmxNumLive; ++k) CHECK(paramSpec(hmxP(k)).id == hmxIds[k]);
  const char* eyeIds[] = {"eyeGain", "eyeLevel", "eyeTightness"};
  for (int k = 0; k < kEyeNumLive; ++k) CHECK(paramSpec(eyeP(k)).id == eyeIds[k]);
  CHECK(paramSpec(hmxP(kHmxClip)).choices == std::vector<std::string>{"silicon", "led", "asymmetric", "soft"});
  CHECK(paramSpec(hmxP(kHmxBoost)).choices == std::vector<std::string>{"off", "on"});
  CHECK(paramSpec(hmxP(kHmxMidVoice)).choices == std::vector<std::string>{"Stock", "Low", "High"});
  CHECK(paramSpec(hmxP(kHmxMidVoice)).name == "Modded Saw Mid Voice");
  CHECK(paramSpec(hmxP(kHmxMix)).unit == "%");
  CHECK(paramSpec(hmxP(kHmxLowMidFreq)).unit.empty());  // a 0..10 knob, not Hz
  CHECK(paramSpec(hmxP(kHmxHighMidFreq)).unit.empty());
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
      if (k.param < 0) continue;  // an empty position
      REQUIRE(inSet(k.param));
      ++controls[k.param];
      ++knobCount;
    }
    for (const FaceKnob& k : f.drawerKnobs) {
      REQUIRE(inSet(k.param));
      ++controls[k.param];
    }
    CHECK(f.drawerKnobs.size() <= 10);
    if (f.clipParam >= 0) {  // -1: no CLIP switch (the one-knob circuit)
      REQUIRE(inSet(f.clipParam));
      CHECK_FALSE(paramSpec(f.clipParam).choices.empty());
      ++controls[f.clipParam];
    }
    REQUIRE(inSet(f.focus.param));
    for (const FaceKnob& s : f.drawerSwitches) {
      REQUIRE(inSet(s.param));
      CHECK_FALSE(paramSpec(s.param).choices.empty());
      ++controls[s.param];
    }
    // FOCUS parameter: a switch on the face plus one drawer knob. Everything else: exactly one control.
    for (int p = ci.firstParam; p < ci.firstParam + ci.numParams; ++p) {
      INFO(paramSpec(p).id);
      CHECK(controls[p] == 1);
    }
    CHECK(knobCount == (c == static_cast<int>(Circuit::OneKnobSaw) ? 3 : 6));
    const double lo = paramSpec(f.focus.param).min, hi = paramSpec(f.focus.param).max;
    CHECK(f.focus.wideValue >= lo);
    CHECK(f.focus.wideValue <= hi);
    CHECK(f.focus.narrowValue >= lo);
    CHECK(f.focus.narrowValue <= hi);
    CHECK(std::string(f.focus.wideText).size() > 0);
    CHECK(std::string(f.focus.narrowText).size() > 0);
  }
  // The 7c rows: BOOST reads OFF / ON over 0 / 1; the one-knob TIGHT reads OFF / ON over 0 / 5.
  const CircuitFace& hx = circuitFace(Circuit::ModdedSaw);
  CHECK(hx.focus.param == hmxP(kHmxBoost));
  CHECK(std::string(hx.focus.label) == "BOOST");
  CHECK(std::string(hx.focus.wideText) == "OFF");
  CHECK(std::string(hx.focus.narrowText) == "ON");
  CHECK(hx.focus.wideValue == 0.0);
  CHECK(hx.focus.narrowValue == 1.0);
  CHECK(hx.focus.threshold == 0.5);
  CHECK(hx.drawerKnobs.size() == 5);
  CHECK(hx.drawerSwitches.size() == 2);  // BOOST, VOICE
  const CircuitFace& ey = circuitFace(Circuit::OneKnobSaw);
  CHECK(ey.clipParam == -1);
  CHECK(ey.focus.param == eyeP(kEyeTightness));
  CHECK(ey.focus.narrowValue == 5.0);
  CHECK(ey.focus.threshold == 2.5);
  CHECK(ey.drawerKnobs.empty());
  CHECK(ey.drawerSwitches.empty());
  CHECK(ey.knobs[1].param == -1);
  CHECK(ey.knobs[2].param == -1);
  CHECK(ey.knobs[5].param == -1);
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
  const fs::path files[] = {kChainsawDir / "classic_buzzsaw.json", kChainsawDir / "pickle_chainsaw.json",
                            kHmxDir / "arizona_mids.json", kEyeDir / "one_knob_max.json"};
  for (const fs::path& file : files) {
    INFO(file.string());
    Host h(44100.0, 512);
    h.load(file);
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
  const HmParams def = HmParams::v2();  // a v1 preset reads as the v2 voicing (HmParams{} is v3 since 7c part 3)
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
  // The 10 Hz timer takes the flag on the message thread. How soon it fires in a dispatch-loop slice is the platform's business (macOS
  // CI took longer than a fixed 250 ms once): pump in small slices until the flag is taken, bounded by 5 s.
  const auto t0 = std::chrono::steady_clock::now();
  while (h.p.circuitEditPending() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
  INFO("the timer took " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() << " ms");
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

// ---- phase 7c Part 2: MODDED SAW (pedal.hmx) and ONE-KNOB SAW (pedal.eye) -------------------------------

TEST_CASE("Pedal params: field access of the 7c sets agrees with the core's live converters", "[pedalface][7c]") {
  HmxParams h;
  h.level = 1.5; h.low = 9.5; h.lowMid = 7.25; h.highMid = 3.5; h.high = 8.0; h.distortion = 6.5; h.presence = 2.5;
  h.tightness = 3.3; h.mix = 41.0; h.clip = ClipType::Soft; h.boost = true; h.lowMidFreq = 2.5; h.highMidFreq = 8.5;
  float live[kHmxNumLive];
  hmxLiveFromParams(h, live);
  HmxParams back;
  for (int i = 0; i < kHmxNumLive; ++i) {
    CHECK(static_cast<float>(hmxField(h, i)) == live[i]);
    setHmxField(back, i, hmxField(h, i));
  }
  CHECK(back == h);
  EyeParams e;
  e.gain = 9.25; e.level = 2.5; e.tightness = 6.0;
  float el[kEyeNumLive];
  eyeLiveFromParams(e, el);
  EyeParams eb;
  for (int i = 0; i < kEyeNumLive; ++i) {
    CHECK(static_cast<float>(eyeField(e, i)) == el[i]);
    setEyeField(eb, i, eyeField(e, i));
  }
  CHECK(eb == e);
  CHECK(circuitForBlockType("pedal.hmx") == Circuit::ModdedSaw);
  CHECK(circuitForBlockType("pedal.eye") == Circuit::OneKnobSaw);
  CHECK(std::string(circuitInfo(Circuit::ModdedSaw).choiceName) == "Modded Saw");
  CHECK(std::string(circuitInfo(Circuit::OneKnobSaw).choiceName) == "One-Knob Saw");
}

TEST_CASE("Pedal params: arizona_mids maps onto the modded-saw set and back; moving it is live", "[pedalface][params][7c][rt]") {
  const Preset p = loadPresetFile(kHmxDir / "arizona_mids.json");
  const auto* hx = dynamic_cast<const HmxBlockParams*>(p.a.blocks[0].params.get());
  REQUIRE(hx != nullptr);
  const ParamValues v = paramsFromPreset(p);
  CHECK(v[kSawCircuit] == 2.0);  // ModdedSaw
  for (int k = 0; k < kHmxNumLive; ++k) {
    INFO(paramSpec(hmxP(k)).id);
    CHECK(v[static_cast<std::size_t>(hmxP(k))] == snapParam(hmxField(hx->p, k)));
  }
  // the other sets are at their defaults
  for (int k = 0; k < kHmNumLive; ++k) CHECK(v[static_cast<std::size_t>(hmP(k))] == paramSpec(hmP(k)).def);
  for (int k = 0; k < kEyeNumLive; ++k) CHECK(v[static_cast<std::size_t>(eyeP(k))] == paramSpec(eyeP(k)).def);
  CHECK(v[hmxP(kHmxClip)] == 1.0);   // led
  CHECK(v[hmxP(kHmxMix)] == 80.0);
  CHECK(v[hmxP(kHmxBoost)] == 0.0);
  Preset q = p;
  applyParams(q, v);  // writes the values back into the block
  CHECK(q == clampedToParams(p));

  Host h(48000.0, 512);
  h.load(kHmxDir / "arizona_mids.json");
  CHECK(h.param(kSawCircuit) == 2.0);
  CHECK(h.param(hmxP(kHmxHighMid)) == Catch::Approx(hx->p.highMid).margin(1e-4));
  CHECK(h.param(hmxP(kHmxHighMidFreq)) == Catch::Approx(6.0).margin(1e-4));
  CHECK(h.param(hmxP(kHmxPresence)) == Catch::Approx(8.0).margin(1e-4));

  const std::uint64_t builds = h.p.engineBuilds();
  const auto x = testInput();
  render(h, x);
  const auto base = render(h, x);
  REQUIRE(rms(base.data(), base.size()) > 1e-3);
  struct Move { int param; double to; const char* what; };
  for (const Move m : {Move{kHmxHighMid, 2.0, "highMid"}, Move{kHmxClip, 0.0, "clip"}, Move{kHmxBoost, 1.0, "boost"}, Move{kHmxDistortion, 3.0, "distortion"},
                       Move{kHmxMix, 30.0, "mix"}, Move{kHmxLowMid, 9.0, "lowMid"}, Move{kHmxPresence, 1.0, "presence"}}) {
    INFO(m.what);
    const double was = h.param(hmxP(m.param));
    h.setParam(hmxP(m.param), m.to);
    render(h, x);
    const auto moved = render(h, x);
    CHECK(relDiff(base, moved) > 0.005);
    h.setParam(hmxP(m.param), was);
    render(h, x);
    CHECK(relDiff(base, render(h, x)) < 1e-4);
  }
  CHECK(h.p.engineBuilds() == builds);  // never a rebuild
  CHECK(h.allocs == 0);
  if (LockGuard::enabled()) CHECK(h.locks == 0);

  // The state carries the moved values and round-trips.
  h.setParam(hmxP(kHmxHighMid), 4.5);
  h.setParam(hmxP(kHmxBoost), 1.0);
  h.setParam(hmxP(kHmxClip), 3.0);
  juce::MemoryBlock s;
  h.p.getStateInformation(s);
  const json j = json::parse(std::string(static_cast<const char*>(s.getData()), s.getSize()));
  const json& blk = j["paths"]["a"]["blocks"][0];
  CHECK(blk["type"] == "pedal.hmx");
  CHECK(blk["params"]["highMid"].get<double>() == Catch::Approx(4.5).margin(1e-4));
  CHECK(blk["params"]["boost"] == "on");
  CHECK(blk["params"]["clip"] == "soft");
  SawbladeProcessor b;
  b.setStateInformation(s.getData(), static_cast<int>(s.getSize()));
  CHECK(b.parameters().getRawParameterValue("sawCircuit")->load() == 2.0f);
  CHECK(b.parameters().getRawParameterValue("hmxBoost")->load() == 1.0f);
  juce::MemoryBlock s2;
  b.getStateInformation(s2);
  CHECK(std::string(static_cast<const char*>(s2.getData()), s2.getSize()) == std::string(static_cast<const char*>(s.getData()), s.getSize()));
}

TEST_CASE("Pedal params: one_knob_max maps onto the one-knob set; eyeGain moves the audio live", "[pedalface][params][7c][rt]") {
  const Preset p = loadPresetFile(kEyeDir / "one_knob_max.json");
  const auto* ey = dynamic_cast<const EyeBlockParams*>(p.a.blocks[0].params.get());
  REQUIRE(ey != nullptr);
  const ParamValues v = paramsFromPreset(p);
  CHECK(v[kSawCircuit] == 3.0);  // OneKnobSaw
  for (int k = 0; k < kEyeNumLive; ++k) CHECK(v[static_cast<std::size_t>(eyeP(k))] == snapParam(eyeField(ey->p, k)));
  CHECK(v[eyeP(kEyeGain)] == 10.0);
  Preset q = p;
  applyParams(q, v);
  CHECK(q == clampedToParams(p));

  Host h(48000.0, 512);
  h.load(kEyeDir / "one_knob_max.json");
  CHECK(h.param(kSawCircuit) == 3.0);
  CHECK(h.param(eyeP(kEyeGain)) == Catch::Approx(10.0));
  const std::uint64_t builds = h.p.engineBuilds();
  const auto x = testInput();
  render(h, x);
  const auto base = render(h, x);
  REQUIRE(rms(base.data(), base.size()) > 1e-3);
  for (const auto& m : {std::pair<int, double>{kEyeGain, 2.0}, {kEyeLevel, 8.0}, {kEyeTightness, 8.0}}) {
    const double was = h.param(eyeP(m.first));
    h.setParam(eyeP(m.first), m.second);
    render(h, x);
    CHECK(relDiff(base, render(h, x)) > 0.005);
    h.setParam(eyeP(m.first), was);
    render(h, x);
    CHECK(relDiff(base, render(h, x)) < 1e-4);
  }
  CHECK(h.p.engineBuilds() == builds);
  CHECK(h.allocs == 0);
  if (LockGuard::enabled()) CHECK(h.locks == 0);
}

TEST_CASE("Pedal params: the CIRCUIT switch cycles all four circuits, one rebuild per step, with the carry-over", "[pedalface][params][7c][swap]") {
  Host h(48000.0, 512);
  h.load(kChainsawDir / "classic_buzzsaw.json");
  h.setParam(hmP(kHmLevel), 3.25);
  h.setParam(hmP(kHmMix), 62.0);
  h.setParam(hmP(kHmTightness), 4.5);
  h.setParam(hmP(kHmClip), 1.0);  // led
  h.setParam(hmP(kHmPresenceDb), 14.0);
  std::uint64_t builds = h.p.engineBuilds();
  const auto x = testInput();

  auto step = [&](Circuit to) {
    h.setParam(kSawCircuit, static_cast<double>(static_cast<int>(to)));
    REQUIRE(h.p.waitForLoader());
    REQUIRE(h.p.status().error.empty());
    CHECK(h.p.engineBuilds() == ++builds);  // exactly one rebuild per step
    REQUIRE(h.p.circuitSlot().has_value());
    CHECK(h.p.circuitSlot()->circuit == to);
    CHECK(h.param(kSawCircuit) == static_cast<double>(static_cast<int>(to)));
    render(h, x);
    const auto y = render(h, x);
    CHECK(rms(y.data(), y.size()) > 1e-3);
    return h.p.currentPreset();
  };

  const Preset fuzz = step(Circuit::BigFuzz);
  CHECK(fuzz.a.blocks[0].type == "pedal.muff");

  const Preset modded = step(Circuit::ModdedSaw);
  CHECK(modded.a.blocks[0].type == "pedal.hmx");
  const auto* hp = dynamic_cast<const HmxBlockParams*>(modded.a.blocks[0].params.get());
  REQUIRE(hp != nullptr);
  HmxParams hexp;
  hexp.level = 3.25; hexp.mix = 62.0; hexp.tightness = 4.5; hexp.clip = ClipType::Led;  // level, mix, tightness, clip carried
  CHECK(hp->p == hexp);
  CHECK(h.param(hmxP(kHmxLevel)) == Catch::Approx(3.25));
  CHECK(h.param(hmxP(kHmxPresence)) == paramSpec(hmxP(kHmxPresence)).def);
  h.setParam(hmxP(kHmxBoost), 1.0);  // not carried to the next circuit

  const Preset eye = step(Circuit::OneKnobSaw);
  CHECK(eye.a.blocks[0].type == "pedal.eye");
  const auto* ep = dynamic_cast<const EyeBlockParams*>(eye.a.blocks[0].params.get());
  REQUIRE(ep != nullptr);
  EyeParams eexp;
  eexp.level = 3.25; eexp.tightness = 4.5;  // level and tightness only: no mix, no clip
  CHECK(ep->p == eexp);
  CHECK(h.param(eyeP(kEyeLevel)) == Catch::Approx(3.25));
  CHECK(h.param(eyeP(kEyeTightness)) == Catch::Approx(4.5));
  CHECK(h.param(hmxP(kHmxBoost)) == 0.0);  // the old set is back at its defaults

  // the state round-trips with the eye block
  juce::MemoryBlock s;
  h.p.getStateInformation(s);
  const json j = json::parse(std::string(static_cast<const char*>(s.getData()), s.getSize()));
  CHECK(j["paths"]["a"]["blocks"][0]["type"] == "pedal.eye");
  SawbladeProcessor b;
  b.setStateInformation(s.getData(), static_cast<int>(s.getSize()));
  CHECK(b.parameters().getRawParameterValue("sawCircuit")->load() == 3.0f);
  CHECK(b.circuitSlot()->circuit == Circuit::OneKnobSaw);
  juce::MemoryBlock s2;
  b.getStateInformation(s2);
  CHECK(std::string(static_cast<const char*>(s2.getData()), s2.getSize()) == std::string(static_cast<const char*>(s.getData()), s.getSize()));

  const Preset saw = step(Circuit::Chainsaw);  // back around; mix and clip are at the chainsaw's defaults
  CHECK(saw.a.blocks[0].type == "pedal.hm");
  const auto* sp = dynamic_cast<const HmBlockParams*>(saw.a.blocks[0].params.get());
  REQUIRE(sp != nullptr);
  HmParams sexp;
  sexp.level = 3.25; sexp.tightness = 4.5;
  CHECK(sp->p == sexp);
  CHECK(sp->p.modelVersion == 3);  // a switch to Chainsaw builds the calibrated (v3) voicing
  CHECK(h.allocs == 0);
}
