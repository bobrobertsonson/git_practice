#include "CircuitFaces.h"

namespace sawblade::plugin {
namespace {

constexpr int hm(int live) { return kHmFirst + live; }
constexpr int mf(int live) { return kMuffFirst + live; }
constexpr int hx(int live) { return kHmxFirst + live; }
constexpr int ey(int live) { return kEyeFirst + live; }

const std::array<CircuitFace, kNumCircuits>& faces() {
  static const std::array<CircuitFace, kNumCircuits> t{{
      {"pedal.hm",
       "CHAINSAW",
       {{{hm(kHmLow), "LOW"}, {hm(kHmHigh), "HIGH"}, {hm(kHmDistortion), "DIST"}, {hm(kHmTightness), "TIGHT"}, {hm(kHmLevel), "OUT"}, {hm(kHmMix), "MIX"}}},
       hm(kHmClip),
       {hm(kHmLowQ), "FOCUS", 0.8, 1.6, 1.2},
       {{hm(kHmLowFreq), "LOW HZ"},
        {hm(kHmLowQ), "LOW Q"},
        {hm(kHmHighFreq), "HIGH HZ"},
        {hm(kHmHighSpread), "SPREAD"},
        {hm(kHmPresenceFreq), "PRES HZ"},
        {hm(kHmPresenceDb), "PRES dB"},
        {hm(kHmRolloffHz), "ROLL-OFF"},
        {hm(kHmGain1Db), "STAGE 1"},
        {hm(kHmGain2Db), "STAGE 2"},
        {hm(kHmBias), "BIAS"}},
       {{hm(kHmMode), "MODE"}, {hm(kHmClip2), "CLIP 2"}}},
      {"pedal.muff",
       "BIG FUZZ",
       {{{mf(kMuffSustain), "SUSTAIN"}, {mf(kMuffTone), "TONE"}, {mf(kMuffScoop), "SCOOP"}, {mf(kMuffTightness), "TIGHT"}, {mf(kMuffVolume), "OUT"}, {mf(kMuffMix), "MIX"}}},
       mf(kMuffClip),
       {mf(kMuffStackRatio), "FOCUS", 4.4, 2.5, 3.4},
       {{mf(kMuffCrunch), "CRUNCH"},
        {mf(kMuffVoice), "VOICE"},
        {mf(kMuffStackRatio), "WIDTH"},
        {mf(kMuffRolloffHz), "ROLL-OFF"},
        {mf(kMuffGain2Db), "STAGE 2"},
        {mf(kMuffBias), "BIAS"}},
       {{mf(kMuffClip2), "CLIP 2"}}},
      {"pedal.hmx",
       "MODDED SAW",
       {{{hx(kHmxLow), "LOW"}, {hx(kHmxHigh), "HIGH"}, {hx(kHmxDistortion), "DIST"}, {hx(kHmxTightness), "TIGHT"}, {hx(kHmxLevel), "OUT"}, {hx(kHmxMix), "MIX"}}},
       hx(kHmxClip),
       {hx(kHmxBoost), "BOOST", 0.0, 1.0, 0.5, "OFF", "ON"},
       {{hx(kHmxLowMid), "LOW-MID"},
        {hx(kHmxLowMidFreq), "LM HZ"},
        {hx(kHmxHighMid), "HIGH-MID"},
        {hx(kHmxHighMidFreq), "HM HZ"},
        {hx(kHmxPresence), "PRESENCE"}},
       {{hx(kHmxBoost), "BOOST"}}},
      {"pedal.eye",
       "ONE-KNOB SAW",
       {{{ey(kEyeGain), "GAIN"}, {-1, ""}, {-1, ""}, {ey(kEyeTightness), "TIGHT"}, {ey(kEyeLevel), "OUT"}, {-1, ""}}},
       -1,
       {ey(kEyeTightness), "TIGHT", 0.0, 5.0, 2.5, "OFF", "ON"},
       {},
       {}},
  }};
  return t;
}

}  // namespace

const CircuitFace& circuitFace(Circuit c) { return faces()[static_cast<std::size_t>(c)]; }

const char* clipShortName(int i) {
  static constexpr const char* n[kNumClipTypes] = {"SI", "LED", "ASYM", "SOFT"};
  return n[i < 0 ? 0 : i >= kNumClipTypes ? kNumClipTypes - 1 : i];
}

}  // namespace sawblade::plugin
