#include "sawblade/pedal_saw_params.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "sawblade/pedal_params.h"

namespace sawblade {
namespace {

void parseModelVersion(JsonObject& o) {
  const int v = o.integer("modelVersion", kPedalModelVersion, std::numeric_limits<int>::min(),
                          std::numeric_limits<int>::max());
  if (v != kPedalModelVersion)
    throw PresetError(o.child("modelVersion"), "unsupported modelVersion " + std::to_string(v) +
                                                   " (this build supports " + std::to_string(kPedalModelVersion) + ")");
}

double knob(JsonObject& po, const char* key, double def = kKnobDefault) { return po.number(key, def, kKnobMin, kKnobMax); }

double clampd(double v, double lo, double hi) noexcept { return std::min(std::max(v, lo), hi); }

double liveKnob(const float* v, int n, int i, double def, double lo, double hi) noexcept {
  if (i >= n) return def;
  const double d = static_cast<double>(v[i]);
  return std::isfinite(d) ? clampd(d, lo, hi) : def;
}

// One row per live parameter (the single source of truth for descriptors and the live converters).
struct Spec {
  const char* key;
  const char* name;
  double min, max, def;
};
constexpr Spec kHmxSpec[kHmxNumLive] = {
    {"level", "Level", 0, 10, 5},
    {"low", "Low", 0, 10, 5},
    {"lowMid", "Low-Mid", 0, 10, 5},
    {"highMid", "High-Mid", 0, 10, 5},
    {"high", "High", 0, 10, 5},
    {"distortion", "Distortion", 0, 10, 5},
    {"presence", "Presence", 0, 10, 5},
    {"tightness", "Tightness", 0, 10, 0},
    {"mix", "Mix", 0, 100, 100},
    {"clip", "Clip", 0, 3, 0},
    {"boost", "Boost", 0, 1, 0},
    {"lowMidFreq", "Low-Mid Freq", 0, 10, 5},
    {"highMidFreq", "High-Mid Freq", 0, 10, 5},
    {"midVoice", "Mid Voice", 0, 2, 0},
};
constexpr Spec kEyeSpec[kEyeNumLive] = {
    {"gain", "Gain", 0, 10, 5},
    {"level", "Level", 0, 10, 5},
    {"tightness", "Tightness", 0, 10, 0},
};

}  // namespace

std::vector<LiveParamDesc> hmxLiveParamDescs() {
  std::vector<LiveParamDesc> d;
  for (int i = 0; i < kHmxNumLive; ++i) {
    const Spec& s = kHmxSpec[i];
    std::vector<std::string> c;
    if (i == kHmxClip) c.assign(kClipNames, kClipNames + kNumClipTypes);
    if (i == kHmxBoost) c = {"off", "on"};
    if (i == kHmxMidVoice) c.assign(kMidVoiceNames, kMidVoiceNames + kNumMidVoices);
    d.push_back({s.key, s.name, s.min, s.max, s.def, std::move(c)});
  }
  return d;
}

std::vector<LiveParamDesc> eyeLiveParamDescs() {
  std::vector<LiveParamDesc> d;
  for (const Spec& s : kEyeSpec) d.push_back({s.key, s.name, s.min, s.max, s.def, {}});
  return d;
}

std::shared_ptr<const BlockParams> parseHmxBlock(JsonObject& o, const std::filesystem::path&) {
  auto b = std::make_shared<HmxBlockParams>();
  parseModelVersion(o);
  if (auto po = o.optionalObject("params")) {
    HmxParams& p = b->p;
    p.level = knob(*po, "level");
    p.low = knob(*po, "low");
    p.lowMid = knob(*po, "lowMid");
    p.highMid = knob(*po, "highMid");
    p.high = knob(*po, "high");
    p.distortion = knob(*po, "distortion");
    p.presence = knob(*po, "presence");
    p.tightness = knob(*po, "tightness", kSawTightnessDefault);
    p.mix = po->number("mix", kSawMixDefault, kSawMixMin, kSawMixMax);
    {
      const std::string c = po->oneOf("clip", "silicon", {"silicon", "led", "asymmetric", "soft"});
      for (int i = 0; i < kNumClipTypes; ++i)
        if (c == kClipNames[i]) p.clip = static_cast<ClipType>(i);
    }
    if (const nlohmann::json* v = po->take("boost")) {
      if (v->is_boolean()) {
        p.boost = v->get<bool>();
      } else if (v->is_string()) {
        const std::string s = v->get<std::string>();
        if (s == "on") p.boost = true;
        else if (s == "off") p.boost = false;
        else throw PresetError(po->child("boost"), "invalid value \"" + s + "\"; expected one of \"off\", \"on\"");
      } else {
        throw PresetError(po->child("boost"), "must be a string (\"off\" or \"on\") or a boolean");
      }
    }
    p.lowMidFreq = knob(*po, "lowMidFreq");
    p.highMidFreq = knob(*po, "highMidFreq");
    {
      const std::string v = po->oneOf("midVoice", "stock", {"stock", "low", "high"});
      for (int i = 0; i < kNumMidVoices; ++i)
        if (v == kMidVoiceNames[i]) p.midVoice = static_cast<MidVoice>(i);
    }
    po->finish();
  }
  return b;
}

std::shared_ptr<const BlockParams> parseEyeBlock(JsonObject& o, const std::filesystem::path&) {
  auto b = std::make_shared<EyeBlockParams>();
  parseModelVersion(o);
  if (auto po = o.optionalObject("params")) {
    b->p.gain = knob(*po, "gain");
    b->p.level = knob(*po, "level");
    b->p.tightness = knob(*po, "tightness", kSawTightnessDefault);
    po->finish();
  }
  return b;
}

bool HmxBlockParams::equals(const BlockParams& other) const {
  const auto* o = dynamic_cast<const HmxBlockParams*>(&other);
  return o && p == o->p;
}
nlohmann::json HmxBlockParams::toJson() const {
  return {{"modelVersion", kPedalModelVersion},
          {"params",
           {{"level", p.level},
            {"low", p.low},
            {"lowMid", p.lowMid},
            {"highMid", p.highMid},
            {"high", p.high},
            {"distortion", p.distortion},
            {"presence", p.presence},
            {"tightness", p.tightness},
            {"mix", p.mix},
            {"clip", clipTypeName(p.clip)},
            {"boost", p.boost ? "on" : "off"},
            {"lowMidFreq", p.lowMidFreq},
            {"highMidFreq", p.highMidFreq},
            {"midVoice", kMidVoiceNames[static_cast<int>(p.midVoice)]}}}};
}

bool EyeBlockParams::equals(const BlockParams& other) const {
  const auto* o = dynamic_cast<const EyeBlockParams*>(&other);
  return o && p == o->p;
}
nlohmann::json EyeBlockParams::toJson() const {
  return {{"modelVersion", kPedalModelVersion},
          {"params", {{"gain", p.gain}, {"level", p.level}, {"tightness", p.tightness}}}};
}

HmxParams hmxParamsFromLive(const float* v, int n) noexcept {
  HmxParams p;
  p.level = liveKnob(v, n, kHmxLevel, p.level, kKnobMin, kKnobMax);
  p.low = liveKnob(v, n, kHmxLow, p.low, kKnobMin, kKnobMax);
  p.lowMid = liveKnob(v, n, kHmxLowMid, p.lowMid, kKnobMin, kKnobMax);
  p.highMid = liveKnob(v, n, kHmxHighMid, p.highMid, kKnobMin, kKnobMax);
  p.high = liveKnob(v, n, kHmxHigh, p.high, kKnobMin, kKnobMax);
  p.distortion = liveKnob(v, n, kHmxDistortion, p.distortion, kKnobMin, kKnobMax);
  p.presence = liveKnob(v, n, kHmxPresence, p.presence, kKnobMin, kKnobMax);
  p.tightness = liveKnob(v, n, kHmxTightness, p.tightness, kKnobMin, kKnobMax);
  p.mix = liveKnob(v, n, kHmxMix, p.mix, kSawMixMin, kSawMixMax);
  const int clip = static_cast<int>(std::lround(liveKnob(v, n, kHmxClip, 0.0, 0.0, kNumClipTypes - 1.0)));
  p.clip = static_cast<ClipType>(clip);
  p.boost = liveKnob(v, n, kHmxBoost, 0.0, 0.0, 1.0) >= 0.5;
  p.lowMidFreq = liveKnob(v, n, kHmxLowMidFreq, p.lowMidFreq, kKnobMin, kKnobMax);
  p.highMidFreq = liveKnob(v, n, kHmxHighMidFreq, p.highMidFreq, kKnobMin, kKnobMax);
  p.midVoice = static_cast<MidVoice>(std::lround(liveKnob(v, n, kHmxMidVoice, 0.0, 0.0, kNumMidVoices - 1.0)));
  return p;
}

void hmxLiveFromParams(const HmxParams& p, float* v) noexcept {
  v[kHmxLevel] = static_cast<float>(p.level);
  v[kHmxLow] = static_cast<float>(p.low);
  v[kHmxLowMid] = static_cast<float>(p.lowMid);
  v[kHmxHighMid] = static_cast<float>(p.highMid);
  v[kHmxHigh] = static_cast<float>(p.high);
  v[kHmxDistortion] = static_cast<float>(p.distortion);
  v[kHmxPresence] = static_cast<float>(p.presence);
  v[kHmxTightness] = static_cast<float>(p.tightness);
  v[kHmxMix] = static_cast<float>(p.mix);
  v[kHmxClip] = static_cast<float>(static_cast<int>(p.clip));
  v[kHmxBoost] = p.boost ? 1.0f : 0.0f;
  v[kHmxLowMidFreq] = static_cast<float>(p.lowMidFreq);
  v[kHmxHighMidFreq] = static_cast<float>(p.highMidFreq);
  v[kHmxMidVoice] = static_cast<float>(static_cast<int>(p.midVoice));
}

EyeParams eyeParamsFromLive(const float* v, int n) noexcept {
  EyeParams p;
  p.gain = liveKnob(v, n, kEyeGain, p.gain, kKnobMin, kKnobMax);
  p.level = liveKnob(v, n, kEyeLevel, p.level, kKnobMin, kKnobMax);
  p.tightness = liveKnob(v, n, kEyeTightness, p.tightness, kKnobMin, kKnobMax);
  return p;
}

void eyeLiveFromParams(const EyeParams& p, float* v) noexcept {
  v[kEyeGain] = static_cast<float>(p.gain);
  v[kEyeLevel] = static_cast<float>(p.level);
  v[kEyeTightness] = static_cast<float>(p.tightness);
}

}  // namespace sawblade
