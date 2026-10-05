#include "rig/RigModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "sawblade/block_registry.h"

namespace sawblade::plugin::rig {
// --- topology --------------------------------------------------------------------------------------
int ampIndex(const PathPreset& p) {
  const int n = static_cast<int>(p.blocks.size());
  for (int i = n - 1; i >= 0; --i)
    if (p.blocks[static_cast<std::size_t>(i)].slot == "amp") return i;
  for (int i = n - 1; i >= 0; --i)
    if (p.blocks[static_cast<std::size_t>(i)].type == "nam") return i;
  return -1;
}

bool isPedalSlot(const PathPreset& p, int index) {
  return index >= 0 && index < static_cast<int>(p.blocks.size()) && index != ampIndex(p);
}

int activePedalSlots(const PathPreset& p) {
  int n = 0;
  for (int i = 0; i < static_cast<int>(p.blocks.size()); ++i)
    if (isPedalSlot(p, i) && !p.blocks[static_cast<std::size_t>(i)].bypass) ++n;
  return n;
}

Topology topologyOf(const Preset& p) {
  if (p.b.enabled) return Topology::Blend;
  return activePedalSlots(p.a) >= 2 ? Topology::SinglePlusTwoPedals : Topology::Single;
}

void setTopology(Preset& p, Topology to, double blendIfRestored) {
  if (to == Topology::Blend) {
    // Phase 10.1: a preset that carries neither level-match key (both at their defaults) gets the new-rig
    // defaults when it first becomes a blend; presets that carry non-default values keep them.
    if (!p.b.enabled && p.levelMatch.mode == LevelMatchMode::Off && p.blendLaw == BlendLaw::Linear) {
      p.levelMatch.mode = LevelMatchMode::Auto;
      p.blendLaw = BlendLaw::ConstantLoudness;
    }
    p.b.enabled = true;
    if (p.blend == 0.0) p.blend = blendIfRestored;
    return;
  }
  p.b.enabled = false;
  p.blend = 0.0;
  const int n = static_cast<int>(p.a.blocks.size());
  if (to == Topology::Single) {
    bool seenActive = false;
    for (int i = 0; i < n; ++i) {
      if (!isPedalSlot(p.a, i)) continue;
      Block& b = p.a.blocks[static_cast<std::size_t>(i)];
      if (b.bypass) continue;
      if (seenActive) b.bypass = true;
      seenActive = true;
    }
  } else if (activePedalSlots(p.a) < 2) {
    for (int i = 0; i < n; ++i) {
      if (!isPedalSlot(p.a, i)) continue;
      Block& b = p.a.blocks[static_cast<std::size_t>(i)];
      if (b.bypass) {
        b.bypass = false;
        break;
      }
    }
  }
}

// --- slots -----------------------------------------------------------------------------------------
bool addBlock(PathPreset& p, int index, Block b) {
  if (static_cast<int>(p.blocks.size()) >= kMaxBlocksPerPath) return false;
  index = std::clamp(index, 0, static_cast<int>(p.blocks.size()));
  p.blocks.insert(p.blocks.begin() + index, std::move(b));
  return true;
}

void removeBlock(PathPreset& p, int index) {
  if (index < 0 || index >= static_cast<int>(p.blocks.size())) return;
  p.blocks.erase(p.blocks.begin() + index);
}

void moveBlock(PathPreset& p, int from, int to) {
  const int n = static_cast<int>(p.blocks.size());
  if (from < 0 || from >= n) return;
  to = std::clamp(to, 0, n - 1);
  if (from == to) return;
  Block b = std::move(p.blocks[static_cast<std::size_t>(from)]);
  p.blocks.erase(p.blocks.begin() + from);
  p.blocks.insert(p.blocks.begin() + to, std::move(b));
}

void setBypass(PathPreset& p, int index, bool bypass) {
  if (index >= 0 && index < static_cast<int>(p.blocks.size())) p.blocks[static_cast<std::size_t>(index)].bypass = bypass;
}

bool setBlockInputGainDb(PathPreset& p, int index, double db) {
  if (index < 0 || index >= static_cast<int>(p.blocks.size()) || !std::isfinite(db)) return false;
  Block& b = p.blocks[static_cast<std::size_t>(index)];
  const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get());
  if (!nam) return false;
  auto np = std::make_shared<NamBlockParams>(*nam);
  np->inputGainDb = std::clamp(db, kBlockGainMinDb, kBlockGainMaxDb);
  b.params = std::move(np);
  return true;
}

std::string newBlockId(const Preset& p, char which) {
  std::set<std::string> used;
  for (const PathPreset* pp : {&p.a, &p.b})
    for (const Block& b : pp->blocks) used.insert(b.id);
  for (int i = 1;; ++i) {
    std::string id = std::string(1, which) + std::to_string(i);
    if (!used.count(id)) return id;
  }
}

std::string defaultSlotFor(const PathPreset& p, const std::string& type) {
  if (type == "eq") return "fx";
  if (type == "nam" && ampIndex(p) < 0) return "amp";
  return "pedal";
}

nlohmann::json flatEqFields() {
  return {{"bands", nlohmann::json::array({{{"type", "peak"}, {"freq", 1000.0}, {"gainDb", 0.0}, {"q", 1.0}}})}};
}

Block makeBlock(const std::string& type, const std::string& id, const std::string& slot,
                const std::filesystem::path& baseDir, const nlohmann::json& typeFields) {
  const BlockType* t = BlockRegistry::instance().find(type);
  if (!t) throw PresetError("type", "unknown block type \"" + type + "\"");
  const nlohmann::json fields = typeFields.is_object() ? typeFields : nlohmann::json::object();
  JsonObject o(fields, "block");
  Block b;
  b.id = id;
  b.type = type;
  b.slot = slot;
  b.params = t->parse(o, baseDir);
  if (!b.params) throw PresetError("block", "block type \"" + type + "\" produced no parameters");
  o.finish();
  return b;
}

std::string captureTitle(const Capture& c) {
  if (c.source && !c.source->title.empty()) return c.source->title;
  return std::filesystem::path(c.file).stem().string();
}

std::string captureCredit(const Capture& c) {
  if (!c.source) return "LOCAL FILE";
  std::string s;
  const auto add = [&s](const std::string& part) {
    if (part.empty()) return;
    if (!s.empty()) s += " \xC2\xB7 ";
    s += part;
  };
  if (!c.source->creator.empty()) add("@" + c.source->creator);
  add(c.source->license);
  add("VIA TONE3000");
  return s;
}

std::string blockTitle(const Block& b) {
  if (const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get())) return captureTitle(nam->model);
  if (const auto* eq = dynamic_cast<const EqBlockParams*>(b.params.get())) {
    const auto n = eq->bands.size();
    return "EQ " + std::to_string(n) + (n == 1 ? " band" : " bands");
  }
  return b.type;
}

std::string blockCredit(const Block& b) {
  if (const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get())) return captureCredit(nam->model);
  return {};
}

// --- EQ --------------------------------------------------------------------------------------------
std::vector<EqBand>& eqBands(Preset& p, EqTarget t) {
  switch (t) {
    case EqTarget::PreA: return p.a.preEq;
    case EqTarget::EqA: return p.a.eq;
    case EqTarget::PreB: return p.b.preEq;
    case EqTarget::EqB: return p.b.eq;
    case EqTarget::Post: break;
  }
  return p.postEq;
}

const std::vector<EqBand>& eqBands(const Preset& p, EqTarget t) { return eqBands(const_cast<Preset&>(p), t); }

bool hasGain(EqType t) { return t != EqType::HighPass && t != EqType::LowPass; }

bool addBand(Preset& p, EqTarget t, EqBand b) {
  auto& v = eqBands(p, t);
  if (static_cast<int>(v.size()) >= ParametricEq::kMaxBands) return false;
  v.push_back(b);
  return true;
}

void removeBand(Preset& p, EqTarget t, int index) {
  auto& v = eqBands(p, t);
  if (index >= 0 && index < static_cast<int>(v.size())) v.erase(v.begin() + index);
}

void setBandLive(Preset& p, EqTarget t, int index, double freq, double gainDb, double q) {
  auto& v = eqBands(p, t);
  if (index < 0 || index >= static_cast<int>(v.size())) return;
  if (!std::isfinite(freq) || !std::isfinite(gainDb) || !std::isfinite(q)) return;
  EqBand& b = v[static_cast<std::size_t>(index)];
  b.freq = std::clamp(freq, kEqFreqMin, kEqFreqMax);
  b.gainDb = hasGain(b.type) ? std::clamp(gainDb, -kEqGainMax, kEqGainMax) : 0.0;
  b.q = std::clamp(q, kEqQMin, kEqQMax);
}

void setBandType(Preset& p, EqTarget t, int index, EqType type) {
  auto& v = eqBands(p, t);
  if (index < 0 || index >= static_cast<int>(v.size())) return;
  EqBand& b = v[static_cast<std::size_t>(index)];
  b.type = type;
  if (!hasGain(type)) b.gainDb = 0.0;
}

void setBandEnabled(Preset& p, EqTarget t, int index, bool enabled) {
  auto& v = eqBands(p, t);
  if (index >= 0 && index < static_cast<int>(v.size())) v[static_cast<std::size_t>(index)].enabled = enabled;
}

// --- cab -------------------------------------------------------------------------------------------
void setCabMode(Preset& p, CabMode m) {
  if (p.cab.mode == m) return;
  if (m == CabMode::PerPath) {
    p.cab.irA = p.cab.ir;
    p.cab.irB = p.cab.ir;
  } else {
    p.cab.ir = p.cab.irA;
  }
  p.cab.mode = m;
}

void setCabEnabled(Preset& p, bool on) { p.cab.enabled = on; }

void setCabIr(Preset& p, CabSlot which, Capture c) {
  switch (which) {
    case CabSlot::Shared: p.cab.ir = std::move(c); break;
    case CabSlot::A: p.cab.irA = std::move(c); break;
    case CabSlot::B: p.cab.irB = std::move(c); break;
  }
}

// --- align -----------------------------------------------------------------------------------------
void setAlignMode(Preset& p, AlignMode m) { p.align.mode = m; }

namespace {
void toManual(Preset& p, const AlignResult& measured) {
  if (p.align.mode == AlignMode::Manual) return;
  const bool seed = p.align.mode == AlignMode::Auto;
  p.align.mode = AlignMode::Manual;
  p.align.delaySamplesB = seed ? std::clamp(measured.delaySamplesB, -kAlignMaxSamples, kAlignMaxSamples) : 0;
  p.align.invertB = seed ? measured.invertB : false;
}
}  // namespace

void nudgeAlign(Preset& p, int samples, const AlignResult& measured) {
  toManual(p, measured);
  p.align.delaySamplesB = std::clamp(p.align.delaySamplesB + samples, -kAlignMaxSamples, kAlignMaxSamples);
}

void setInvertB(Preset& p, bool invert, const AlignResult& measured) {
  toManual(p, measured);
  p.align.invertB = invert;
}

// --- gate / comp -----------------------------------------------------------------------------------
Range gateRange(GateField f) {
  switch (f) {
    case GateField::Threshold: return {-120.0, 0.0};
    case GateField::Hysteresis: return {0.0, 60.0};
    case GateField::Attack: return {0.01, 1000.0};
    case GateField::Hold: return {0.0, 10000.0};
    case GateField::Release: return {0.01, 10000.0};
    case GateField::Range: return {-120.0, 0.0};
    case GateField::Ratio: return {1.5, 10.0};
    case GateField::KeyHpf: return {0.0, 400.0};
  }
  return {0.0, 1.0};
}

double gateField(const GateParams& g, GateField f) {
  switch (f) {
    case GateField::Threshold: return g.thresholdDb;
    case GateField::Hysteresis: return g.hysteresisDb;
    case GateField::Attack: return g.attackMs;
    case GateField::Hold: return g.holdMs;
    case GateField::Release: return g.releaseMs;
    case GateField::Range: return g.rangeDb;
    case GateField::Ratio: return g.ratio;
    case GateField::KeyHpf: return g.keyHighPassHz;
  }
  return 0.0;
}

void setGateField(Preset& p, GateField f, double v) {
  if (!std::isfinite(v)) return;
  const Range r = gateRange(f);
  v = std::clamp(v, r.lo, r.hi);
  if (f == GateField::KeyHpf) v = v < 20.0 ? 0.0 : std::max(v, 40.0);
  GateParams& g = p.gate;
  switch (f) {
    case GateField::Threshold: g.thresholdDb = v; break;
    case GateField::Hysteresis: g.hysteresisDb = v; break;
    case GateField::Attack: g.attackMs = v; break;
    case GateField::Hold: g.holdMs = v; break;
    case GateField::Release: g.releaseMs = v; break;
    case GateField::Range: g.rangeDb = v; break;
    case GateField::Ratio: g.ratio = v; break;
    case GateField::KeyHpf: g.keyHighPassHz = v; break;
  }
}

void setGateEnabled(Preset& p, bool on) { p.gate.enabled = on; }
void setGateMode(Preset& p, GateMode m) { p.gate.mode = m; }
void setGateReleaseCurve(Preset& p, GateReleaseCurve c) { p.gate.releaseCurve = c; }

Range compRange(CompField f) {
  switch (f) {
    case CompField::Threshold: return {-80.0, 0.0};
    case CompField::Ratio: return {1.0, 100.0};
    case CompField::Knee: return {0.0, 48.0};
    case CompField::Attack: return {0.01, 1000.0};
    case CompField::Release: return {1.0, 10000.0};
    case CompField::Makeup: return {-24.0, 48.0};
  }
  return {0.0, 1.0};
}

double compField(const BusCompParams& c, CompField f) {
  switch (f) {
    case CompField::Threshold: return c.thresholdDb;
    case CompField::Ratio: return c.ratio;
    case CompField::Knee: return c.kneeDb;
    case CompField::Attack: return c.attackMs;
    case CompField::Release: return c.releaseMs;
    case CompField::Makeup: return c.makeupDb;
  }
  return 0.0;
}

void setCompField(Preset& p, CompField f, double v) {
  if (!std::isfinite(v)) return;
  const Range r = compRange(f);
  v = std::clamp(v, r.lo, r.hi);
  BusCompParams& c = p.busComp;
  switch (f) {
    case CompField::Threshold: c.thresholdDb = v; break;
    case CompField::Ratio: c.ratio = v; break;
    case CompField::Knee: c.kneeDb = v; break;
    case CompField::Attack: c.attackMs = v; break;
    case CompField::Release: c.releaseMs = v; break;
    case CompField::Makeup: c.makeupDb = v; break;
  }
}

void setCompEnabled(Preset& p, bool on) { p.busComp.enabled = on; }

}  // namespace sawblade::plugin::rig
