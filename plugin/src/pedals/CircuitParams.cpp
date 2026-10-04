#include "CircuitParams.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace sawblade::plugin {
namespace {

constexpr CircuitInfo kInfo[kNumCircuits] = {
    {"pedal.hm", "Chainsaw", kHmFirst, kHmNumLive},
    {"pedal.muff", "Big Fuzz", kMuffFirst, kMuffNumLive},
};

template <class E>
E enumFrom(double v, int maxIndex) {
  const long i = std::lround(std::isfinite(v) ? v : 0.0);
  return static_cast<E>(std::clamp<long>(i, 0, maxIndex));
}

std::string unitFor(const std::string& key) {
  auto ends = [&](const char* s) {
    const std::string t(s);
    return key.size() >= t.size() && key.compare(key.size() - t.size(), t.size(), t) == 0;
  };
  if (ends("Freq") || ends("Hz")) return "Hz";
  if (ends("Db")) return "dB";
  if (key == "mix") return "%";
  return "";
}

double clampSnap(int index, double v) {
  const ParamSpec& s = paramSpec(index);
  if (!std::isfinite(v)) return s.def;
  return snapParam(std::clamp(v, s.min, s.max));
}

}  // namespace

double hmField(const HmParams& p, int i) {
  switch (i) {
    case kHmLevel: return p.level;
    case kHmLow: return p.low;
    case kHmHigh: return p.high;
    case kHmDistortion: return p.distortion;
    case kHmTightness: return p.tightness;
    case kHmMix: return p.mix;
    case kHmMode: return static_cast<int>(p.mode);
    case kHmClip: return static_cast<int>(p.clip);
    case kHmClip2: return static_cast<int>(p.clip2);
    case kHmLowFreq: return p.lowFreq;
    case kHmLowQ: return p.lowQ;
    case kHmHighFreq: return p.highFreq;
    case kHmHighSpread: return p.highSpread;
    case kHmPresenceFreq: return p.presenceFreq;
    case kHmPresenceDb: return p.presenceDb;
    case kHmRolloffHz: return p.rolloffHz;
    case kHmGain1Db: return p.gain1Db;
    case kHmGain2Db: return p.gain2Db;
    case kHmBias: return p.bias;
  }
  return 0.0;
}

void setHmField(HmParams& p, int i, double v) {
  switch (i) {
    case kHmLevel: p.level = v; break;
    case kHmLow: p.low = v; break;
    case kHmHigh: p.high = v; break;
    case kHmDistortion: p.distortion = v; break;
    case kHmTightness: p.tightness = v; break;
    case kHmMix: p.mix = v; break;
    case kHmMode: p.mode = enumFrom<HmMode>(v, 2); break;
    case kHmClip: p.clip = enumFrom<ClipType>(v, kNumClipTypes - 1); break;
    case kHmClip2: p.clip2 = enumFrom<Clip2Type>(v, kNumClipTypes); break;
    case kHmLowFreq: p.lowFreq = v; break;
    case kHmLowQ: p.lowQ = v; break;
    case kHmHighFreq: p.highFreq = v; break;
    case kHmHighSpread: p.highSpread = v; break;
    case kHmPresenceFreq: p.presenceFreq = v; break;
    case kHmPresenceDb: p.presenceDb = v; break;
    case kHmRolloffHz: p.rolloffHz = v; break;
    case kHmGain1Db: p.gain1Db = v; break;
    case kHmGain2Db: p.gain2Db = v; break;
    case kHmBias: p.bias = v; break;
    default: break;
  }
}

double muffField(const MuffParams& p, int i) {
  switch (i) {
    case kMuffVolume: return p.volume;
    case kMuffSustain: return p.sustain;
    case kMuffTone: return p.tone;
    case kMuffScoop: return p.scoop;
    case kMuffCrunch: return p.crunch;
    case kMuffVoice: return p.voice;
    case kMuffTightness: return p.tightness;
    case kMuffMix: return p.mix;
    case kMuffClip: return static_cast<int>(p.clip);
    case kMuffClip2: return static_cast<int>(p.clip2);
    case kMuffStackRatio: return p.stackRatio;
    case kMuffRolloffHz: return p.rolloffHz;
    case kMuffGain2Db: return p.gain2Db;
    case kMuffBias: return p.bias;
  }
  return 0.0;
}

void setMuffField(MuffParams& p, int i, double v) {
  switch (i) {
    case kMuffVolume: p.volume = v; break;
    case kMuffSustain: p.sustain = v; break;
    case kMuffTone: p.tone = v; break;
    case kMuffScoop: p.scoop = v; break;
    case kMuffCrunch: p.crunch = v; break;
    case kMuffVoice: p.voice = v; break;
    case kMuffTightness: p.tightness = v; break;
    case kMuffMix: p.mix = v; break;
    case kMuffClip: p.clip = enumFrom<ClipType>(v, kNumClipTypes - 1); break;
    case kMuffClip2: p.clip2 = enumFrom<Clip2Type>(v, kNumClipTypes); break;
    case kMuffStackRatio: p.stackRatio = v; break;
    case kMuffRolloffHz: p.rolloffHz = v; break;
    case kMuffGain2Db: p.gain2Db = v; break;
    case kMuffBias: p.bias = v; break;
    default: break;
  }
}

const CircuitInfo& circuitInfo(Circuit c) { return kInfo[static_cast<int>(c)]; }

std::optional<Circuit> circuitForBlockType(std::string_view t) {
  for (int i = 0; i < kNumCircuits; ++i)
    if (t == kInfo[i].blockType) return static_cast<Circuit>(i);
  return std::nullopt;
}

std::optional<CircuitSlot> findCircuitBlock(const Preset& p) {
  const PathPreset* paths[2] = {&p.a, &p.b};
  for (int k = 0; k < 2; ++k)
    for (std::size_t i = 0; i < paths[k]->blocks.size(); ++i)
      if (const auto c = circuitForBlockType(paths[k]->blocks[i].type)) return CircuitSlot{k, static_cast<int>(i), *c};
  return std::nullopt;
}

ParamSpec circuitParamSpec(int index) {
  if (index == kSawCircuit) {
    ParamSpec s{"sawCircuit", "Circuit", "", 0.0, static_cast<double>(kNumCircuits - 1), 0.0, {}};
    for (const CircuitInfo& c : kInfo) s.choices.push_back(c.choiceName);
    return s;
  }
  for (int c = 0; c < kNumCircuits; ++c) {
    const CircuitInfo& ci = kInfo[c];
    if (index < ci.firstParam || index >= ci.firstParam + ci.numParams) continue;
    const int k = index - ci.firstParam;
    const LiveParamDesc d = (c == 0 ? hmLiveParamDescs() : muffLiveParamDescs())[static_cast<std::size_t>(k)];
    std::string id = d.key;
    id[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(id[0])));
    id = (c == 0 ? "hm" : "muff") + id;
    return {id, std::string(ci.choiceName) + " " + d.name, d.choices.empty() ? unitFor(d.key) : "", d.min, d.max, d.def, d.choices};
  }
  return {};
}

void circuitParamsFromPreset(const Preset& p, ParamValues& v) {
  v[kSawCircuit] = 0.0;
  for (int i = kHmFirst; i < kNumParams; ++i) v[static_cast<std::size_t>(i)] = paramSpec(i).def;
  const auto slot = findCircuitBlock(p);
  if (!slot) return;
  v[kSawCircuit] = static_cast<double>(slot->circuit);
  const PathPreset& path = slot->path == 0 ? p.a : p.b;
  const Block& b = path.blocks[static_cast<std::size_t>(slot->block)];
  const CircuitInfo& ci = circuitInfo(slot->circuit);
  for (int k = 0; k < ci.numParams; ++k) {
    double x = paramSpec(ci.firstParam + k).def;
    if (const auto* h = dynamic_cast<const HmBlockParams*>(b.params.get())) x = hmField(h->p, k);
    else if (const auto* m = dynamic_cast<const MuffBlockParams*>(b.params.get())) x = muffField(m->p, k);
    v[static_cast<std::size_t>(ci.firstParam + k)] = clampSnap(ci.firstParam + k, x);
  }
}

void applyCircuitParams(Preset& p, const ParamValues& v) {
  const auto slot = findCircuitBlock(p);
  if (!slot) return;
  PathPreset& path = slot->path == 0 ? p.a : p.b;
  Block& b = path.blocks[static_cast<std::size_t>(slot->block)];
  const CircuitInfo& ci = circuitInfo(slot->circuit);
  if (const auto* h = dynamic_cast<const HmBlockParams*>(b.params.get())) {
    auto nb = std::make_shared<HmBlockParams>();
    nb->p = h->p;
    for (int k = 0; k < ci.numParams; ++k) setHmField(nb->p, k, v[static_cast<std::size_t>(ci.firstParam + k)]);
    b.params = std::move(nb);
  } else if (const auto* m = dynamic_cast<const MuffBlockParams*>(b.params.get())) {
    auto nb = std::make_shared<MuffBlockParams>();
    nb->p = m->p;
    for (int k = 0; k < ci.numParams; ++k) setMuffField(nb->p, k, v[static_cast<std::size_t>(ci.firstParam + k)]);
    b.params = std::move(nb);
  }
}

Preset switchCircuit(const Preset& p, Circuit target) {
  Preset out = p;
  const auto slot = findCircuitBlock(out);
  if (!slot || slot->circuit == target) return out;
  PathPreset& path = slot->path == 0 ? out.a : out.b;
  Block& b = path.blocks[static_cast<std::size_t>(slot->block)];
  double level = 5.0, mix = 100.0, tight = 0.0;
  ClipType clip = ClipType::Silicon;
  if (const auto* h = dynamic_cast<const HmBlockParams*>(b.params.get())) {
    level = h->p.level; mix = h->p.mix; tight = h->p.tightness; clip = h->p.clip;
  } else if (const auto* m = dynamic_cast<const MuffBlockParams*>(b.params.get())) {
    level = m->p.volume; mix = m->p.mix; tight = m->p.tightness; clip = m->p.clip;
  }
  if (target == Circuit::Chainsaw) {
    auto nb = std::make_shared<HmBlockParams>();
    nb->p.level = level; nb->p.mix = mix; nb->p.tightness = tight; nb->p.clip = clip;
    b.params = std::move(nb);
  } else {
    auto nb = std::make_shared<MuffBlockParams>();
    nb->p.volume = level; nb->p.mix = mix; nb->p.tightness = tight; nb->p.clip = clip;
    b.params = std::move(nb);
  }
  b.type = circuitInfo(target).blockType;
  return out;
}

int circuitLiveValues(Circuit c, const ParamValues& v, float* out) noexcept {
  const CircuitInfo& ci = circuitInfo(c);
  for (int k = 0; k < ci.numParams; ++k) out[k] = static_cast<float>(v[static_cast<std::size_t>(ci.firstParam + k)]);
  return ci.numParams;
}

int blockLiveValues(const Block& b, float* out) noexcept {
  if (const auto* h = dynamic_cast<const HmBlockParams*>(b.params.get())) {
    hmLiveFromParams(h->p, out);
    return kHmNumLive;
  }
  if (const auto* m = dynamic_cast<const MuffBlockParams*>(b.params.get())) {
    muffLiveFromParams(m->p, out);
    return kMuffNumLive;
  }
  return 0;
}

}  // namespace sawblade::plugin
