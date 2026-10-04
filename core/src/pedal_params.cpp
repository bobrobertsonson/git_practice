#include "sawblade/pedal_params.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sawblade {
namespace {

// One row per live parameter: the single source of truth for keys, ranges and defaults. Enum rows
// have min 0, max = (number of choices - 1).
struct Spec {
  const char* key;
  const char* name;
  double min, max, def;
};

constexpr Spec kHmSpec[kHmNumLive] = {
    {"level", "Level", 0, 10, 5},
    {"low", "Low", 0, 10, 5},
    {"high", "High", 0, 10, 5},
    {"distortion", "Distortion", 0, 10, 5},
    {"tightness", "Tightness", 0, 10, 0},
    {"mix", "Mix", 0, 100, 100},
    {"mode", "Mode", 0, 2, 0},
    {"clip", "Clip", 0, 3, 0},
    {"clip2", "Clip 2", 0, 4, 0},
    {"lowFreq", "Low Freq", 60, 160, 100},
    {"lowQ", "Low Q", 0.5, 2.0, 0.8},
    {"highFreq", "High Freq", 800, 2000, 1000},
    {"highSpread", "High Spread", 1.0, 2.0, 1.5},
    {"presenceFreq", "Presence Freq", 3000, 7000, 4800},
    {"presenceDb", "Presence", 0, 16, 8},
    {"rolloffHz", "Roll-off", 4000, 12000, 9000},
    {"gain1Db", "Stage 1", -12, 12, 0},
    {"gain2Db", "Stage 2", -12, 12, 0},
    {"bias", "Bias", 0, 10, 0},
};

constexpr Spec kMuffSpec[kMuffNumLive] = {
    {"volume", "Volume", 0, 10, 5},
    {"sustain", "Sustain", 0, 10, 5},
    {"tone", "Tone", 0, 10, 5},
    {"scoop", "Scoop", 0, 10, 3},
    {"crunch", "Crunch", 0, 10, 5},
    {"voice", "Voice", 0, 10, 5},
    {"tightness", "Tightness", 0, 10, 0},
    {"mix", "Mix", 0, 100, 100},
    {"clip", "Clip", 0, 3, 0},
    {"clip2", "Clip 2", 0, 4, 0},
    {"stackRatio", "Width", 2.0, 8.0, 4.4},
    {"rolloffHz", "Roll-off", 4000, 12000, 10000},
    {"gain2Db", "Stage 2", -12, 12, 0},
    {"bias", "Bias", 0, 10, 0},
};

std::vector<std::string> clipChoices() { return {kClipNames, kClipNames + kNumClipTypes}; }
std::vector<std::string> clip2Choices() { return {kClip2Names, kClip2Names + kNumClipTypes + 1}; }
std::vector<std::string> modeChoices() { return {kHmModeNames, kHmModeNames + 3}; }

LiveParamDesc desc(const Spec& s, std::vector<std::string> choices = {}) {
  return {s.key, s.name, s.min, s.max, s.def, std::move(choices)};
}

double live(const float* v, int n, int i, const Spec& s) noexcept {
  if (i >= n || v == nullptr || !std::isfinite(v[i])) return s.def;
  return std::clamp(static_cast<double>(v[i]), s.min, s.max);
}
int liveEnum(const float* v, int n, int i, const Spec& s) noexcept {
  return static_cast<int>(std::lround(live(v, n, i, s)));
}

int parseModelVersion(JsonObject& o, int current, int oldest) {
  const int v = o.integer("modelVersion", oldest, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
  if (v < oldest || v > current)
    throw PresetError(o.child("modelVersion"), "unsupported modelVersion " + std::to_string(v) +
                                                   " (this build supports " + std::to_string(oldest) +
                                                   (current == oldest ? "" : ".." + std::to_string(current)) + ")");
  return v;
}

double knob(JsonObject& po, const char* key) { return po.number(key, kKnobDefault, kKnobMin, kKnobMax); }
double num(JsonObject& po, const Spec& s) { return po.number(s.key, s.def, s.min, s.max); }

ClipType parseClip(JsonObject& po, const char* key) {
  const std::string s = po.oneOf(key, "silicon", {"silicon", "led", "asymmetric", "soft"});
  for (int i = 0; i < kNumClipTypes; ++i)
    if (s == kClipNames[i]) return static_cast<ClipType>(i);
  return ClipType::Silicon;
}
Clip2Type parseClip2(JsonObject& po, const char* key) {
  const std::string s = po.oneOf(key, "follow", {"follow", "silicon", "led", "asymmetric", "soft"});
  for (int i = 0; i <= kNumClipTypes; ++i)
    if (s == kClip2Names[i]) return static_cast<Clip2Type>(i);
  return Clip2Type::Follow;
}

}  // namespace

std::vector<LiveParamDesc> hmLiveParamDescs() {
  std::vector<LiveParamDesc> d;
  for (int i = 0; i < kHmNumLive; ++i) {
    std::vector<std::string> c;
    if (i == kHmMode) c = modeChoices();
    if (i == kHmClip) c = clipChoices();
    if (i == kHmClip2) c = clip2Choices();
    d.push_back(desc(kHmSpec[i], std::move(c)));
  }
  return d;
}

std::vector<LiveParamDesc> muffLiveParamDescs() {
  std::vector<LiveParamDesc> d;
  for (int i = 0; i < kMuffNumLive; ++i) {
    std::vector<std::string> c;
    if (i == kMuffClip) c = clipChoices();
    if (i == kMuffClip2) c = clip2Choices();
    d.push_back(desc(kMuffSpec[i], std::move(c)));
  }
  return d;
}

HmParams hmParamsFromLive(const float* v, int n) noexcept {
  const auto L = [&](int i) { return live(v, n, i, kHmSpec[i]); };
  HmParams p;
  p.level = L(kHmLevel);
  p.low = L(kHmLow);
  p.high = L(kHmHigh);
  p.distortion = L(kHmDistortion);
  p.tightness = L(kHmTightness);
  p.mix = L(kHmMix);
  p.mode = static_cast<HmMode>(liveEnum(v, n, kHmMode, kHmSpec[kHmMode]));
  p.clip = static_cast<ClipType>(liveEnum(v, n, kHmClip, kHmSpec[kHmClip]));
  p.clip2 = static_cast<Clip2Type>(liveEnum(v, n, kHmClip2, kHmSpec[kHmClip2]));
  p.lowFreq = L(kHmLowFreq);
  p.lowQ = L(kHmLowQ);
  p.highFreq = L(kHmHighFreq);
  p.highSpread = L(kHmHighSpread);
  p.presenceFreq = L(kHmPresenceFreq);
  p.presenceDb = L(kHmPresenceDb);
  p.rolloffHz = L(kHmRolloffHz);
  p.gain1Db = L(kHmGain1Db);
  p.gain2Db = L(kHmGain2Db);
  p.bias = L(kHmBias);
  return p;
}

void hmLiveFromParams(const HmParams& p, float* v) noexcept {
  const auto f = [](double x) { return static_cast<float>(x); };
  v[kHmLevel] = f(p.level);
  v[kHmLow] = f(p.low);
  v[kHmHigh] = f(p.high);
  v[kHmDistortion] = f(p.distortion);
  v[kHmTightness] = f(p.tightness);
  v[kHmMix] = f(p.mix);
  v[kHmMode] = static_cast<float>(static_cast<int>(p.mode));
  v[kHmClip] = static_cast<float>(static_cast<int>(p.clip));
  v[kHmClip2] = static_cast<float>(static_cast<int>(p.clip2));
  v[kHmLowFreq] = f(p.lowFreq);
  v[kHmLowQ] = f(p.lowQ);
  v[kHmHighFreq] = f(p.highFreq);
  v[kHmHighSpread] = f(p.highSpread);
  v[kHmPresenceFreq] = f(p.presenceFreq);
  v[kHmPresenceDb] = f(p.presenceDb);
  v[kHmRolloffHz] = f(p.rolloffHz);
  v[kHmGain1Db] = f(p.gain1Db);
  v[kHmGain2Db] = f(p.gain2Db);
  v[kHmBias] = f(p.bias);
}

MuffParams muffParamsFromLive(const float* v, int n) noexcept {
  const auto L = [&](int i) { return live(v, n, i, kMuffSpec[i]); };
  MuffParams p;
  p.volume = L(kMuffVolume);
  p.sustain = L(kMuffSustain);
  p.tone = L(kMuffTone);
  p.scoop = L(kMuffScoop);
  p.crunch = L(kMuffCrunch);
  p.voice = L(kMuffVoice);
  p.tightness = L(kMuffTightness);
  p.mix = L(kMuffMix);
  p.clip = static_cast<ClipType>(liveEnum(v, n, kMuffClip, kMuffSpec[kMuffClip]));
  p.clip2 = static_cast<Clip2Type>(liveEnum(v, n, kMuffClip2, kMuffSpec[kMuffClip2]));
  p.stackRatio = L(kMuffStackRatio);
  p.rolloffHz = L(kMuffRolloffHz);
  p.gain2Db = L(kMuffGain2Db);
  p.bias = L(kMuffBias);
  return p;
}

void muffLiveFromParams(const MuffParams& p, float* v) noexcept {
  const auto f = [](double x) { return static_cast<float>(x); };
  v[kMuffVolume] = f(p.volume);
  v[kMuffSustain] = f(p.sustain);
  v[kMuffTone] = f(p.tone);
  v[kMuffScoop] = f(p.scoop);
  v[kMuffCrunch] = f(p.crunch);
  v[kMuffVoice] = f(p.voice);
  v[kMuffTightness] = f(p.tightness);
  v[kMuffMix] = f(p.mix);
  v[kMuffClip] = static_cast<float>(static_cast<int>(p.clip));
  v[kMuffClip2] = static_cast<float>(static_cast<int>(p.clip2));
  v[kMuffStackRatio] = f(p.stackRatio);
  v[kMuffRolloffHz] = f(p.rolloffHz);
  v[kMuffGain2Db] = f(p.gain2Db);
  v[kMuffBias] = f(p.bias);
}

std::shared_ptr<const BlockParams> parseHmBlock(JsonObject& o, const std::filesystem::path&) {
  auto b = std::make_shared<HmBlockParams>();
  const int version = parseModelVersion(o, kHmModelVersion, 1);
  if (auto po = o.optionalObject("params")) {
    // The four stock knobs exist in both versions.
    b->p.level = num(*po, kHmSpec[kHmLevel]);
    b->p.low = num(*po, kHmSpec[kHmLow]);
    b->p.high = num(*po, kHmSpec[kHmHigh]);
    b->p.distortion = num(*po, kHmSpec[kHmDistortion]);
    if (version >= 2) {
      HmParams& p = b->p;
      p.tightness = num(*po, kHmSpec[kHmTightness]);
      p.mix = num(*po, kHmSpec[kHmMix]);
      const std::string mode = po->oneOf("mode", "stock", {"stock", "custom", "modded"});
      p.mode = mode == "custom" ? HmMode::Custom : mode == "modded" ? HmMode::Modded : HmMode::Stock;
      p.clip = parseClip(*po, "clip");
      p.clip2 = parseClip2(*po, "clip2");
      p.lowFreq = num(*po, kHmSpec[kHmLowFreq]);
      p.lowQ = num(*po, kHmSpec[kHmLowQ]);
      p.highFreq = num(*po, kHmSpec[kHmHighFreq]);
      p.highSpread = num(*po, kHmSpec[kHmHighSpread]);
      p.presenceFreq = num(*po, kHmSpec[kHmPresenceFreq]);
      p.presenceDb = num(*po, kHmSpec[kHmPresenceDb]);
      p.rolloffHz = num(*po, kHmSpec[kHmRolloffHz]);
      p.gain1Db = num(*po, kHmSpec[kHmGain1Db]);
      p.gain2Db = num(*po, kHmSpec[kHmGain2Db]);
      p.bias = num(*po, kHmSpec[kHmBias]);
    }
    po->finish();
  }
  return b;
}

std::shared_ptr<const BlockParams> parseMuffBlock(JsonObject& o, const std::filesystem::path&) {
  auto b = std::make_shared<MuffBlockParams>();
  parseModelVersion(o, kMuffModelVersion, 1);
  if (auto po = o.optionalObject("params")) {
    MuffParams& p = b->p;
    p.volume = num(*po, kMuffSpec[kMuffVolume]);
    p.sustain = num(*po, kMuffSpec[kMuffSustain]);
    p.tone = num(*po, kMuffSpec[kMuffTone]);
    p.scoop = num(*po, kMuffSpec[kMuffScoop]);
    p.crunch = num(*po, kMuffSpec[kMuffCrunch]);
    p.voice = num(*po, kMuffSpec[kMuffVoice]);
    p.tightness = num(*po, kMuffSpec[kMuffTightness]);
    p.mix = num(*po, kMuffSpec[kMuffMix]);
    p.clip = parseClip(*po, "clip");
    p.clip2 = parseClip2(*po, "clip2");
    p.stackRatio = num(*po, kMuffSpec[kMuffStackRatio]);
    p.rolloffHz = num(*po, kMuffSpec[kMuffRolloffHz]);
    p.gain2Db = num(*po, kMuffSpec[kMuffGain2Db]);
    p.bias = num(*po, kMuffSpec[kMuffBias]);
    po->finish();
  }
  return b;
}

std::shared_ptr<const BlockParams> parseTsBlock(JsonObject& o, const std::filesystem::path&) {
  auto b = std::make_shared<TsBlockParams>();
  parseModelVersion(o, kPedalModelVersion, 1);
  if (auto po = o.optionalObject("params")) {
    b->p.drive = knob(*po, "drive");
    b->p.tone = knob(*po, "tone");
    b->p.level = knob(*po, "level");
    po->finish();
  }
  return b;
}

bool HmBlockParams::equals(const BlockParams& other) const {
  const auto* o = dynamic_cast<const HmBlockParams*>(&other);
  return o && p == o->p;
}
nlohmann::json HmBlockParams::toJson() const {
  nlohmann::json j = {{"level", p.level},           {"low", p.low},
                      {"high", p.high},             {"distortion", p.distortion},
                      {"tightness", p.tightness},   {"mix", p.mix},
                      {"mode", kHmModeNames[static_cast<int>(p.mode)]},
                      {"clip", clipTypeName(p.clip)},
                      {"clip2", clip2TypeName(p.clip2)},
                      {"lowFreq", p.lowFreq},       {"lowQ", p.lowQ},
                      {"highFreq", p.highFreq},     {"highSpread", p.highSpread},
                      {"presenceFreq", p.presenceFreq}, {"presenceDb", p.presenceDb},
                      {"rolloffHz", p.rolloffHz},   {"gain1Db", p.gain1Db},
                      {"gain2Db", p.gain2Db},       {"bias", p.bias}};
  return {{"modelVersion", kHmModelVersion}, {"params", j}};
}

bool MuffBlockParams::equals(const BlockParams& other) const {
  const auto* o = dynamic_cast<const MuffBlockParams*>(&other);
  return o && p == o->p;
}
nlohmann::json MuffBlockParams::toJson() const {
  nlohmann::json j = {{"volume", p.volume},   {"sustain", p.sustain}, {"tone", p.tone},
                      {"scoop", p.scoop},     {"crunch", p.crunch},   {"voice", p.voice},
                      {"tightness", p.tightness}, {"mix", p.mix},
                      {"clip", clipTypeName(p.clip)}, {"clip2", clip2TypeName(p.clip2)},
                      {"stackRatio", p.stackRatio}, {"rolloffHz", p.rolloffHz},
                      {"gain2Db", p.gain2Db}, {"bias", p.bias}};
  return {{"modelVersion", kMuffModelVersion}, {"params", j}};
}

bool TsBlockParams::equals(const BlockParams& other) const {
  const auto* o = dynamic_cast<const TsBlockParams*>(&other);
  return o && p == o->p;
}
nlohmann::json TsBlockParams::toJson() const {
  return {{"modelVersion", kPedalModelVersion}, {"params", {{"drive", p.drive}, {"tone", p.tone}, {"level", p.level}}}};
}

}  // namespace sawblade
