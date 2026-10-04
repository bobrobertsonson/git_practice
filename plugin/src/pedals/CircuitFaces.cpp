#include "CircuitFaces.h"

namespace sawblade::plugin {
namespace {

constexpr int hm(int live) { return kHmFirst + live; }
constexpr int mf(int live) { return kMuffFirst + live; }

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
