#include "PresetMapping.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "pedals/CircuitParams.h"

namespace sawblade::plugin {
namespace {

const std::array<ParamSpec, kNumParams>& specs() {
  static const std::array<ParamSpec, kNumParams> s = [] {
    std::array<ParamSpec, kNumParams> a{{
        {"inputGain", "Input Gain", "dB", -24.0, 24.0, 0.0},
        {"outputGain", "Output Gain", "dB", -24.0, 24.0, 0.0},
        {"gateThreshold", "Gate Threshold", "dB", -80.0, -20.0, -55.0},
        {"blend", "Blend (Saw - Body)", "", 0.0, 1.0, 0.5},
        {"levelA", "Saw Level", "dB", -24.0, 12.0, 0.0},
        {"levelB", "Body Level", "dB", -24.0, 12.0, 0.0},
    }};
    static const char* const kKnobId[kAmpKnobCount] = {"gain", "bass", "mid", "treble", "presence", "level"};
    static const char* const kKnobName[kAmpKnobCount] = {"Gain", "Bass", "Mid", "Treble", "Presence", "Level"};
    for (int path = 0; path < 2; ++path)
      for (int k = 0; k < kAmpKnobCount; ++k)
        a[static_cast<std::size_t>(ampParam(path, k))] = {std::string("amp") + (path == 0 ? "A_" : "B_") + kKnobId[k],
                                                          std::string(path == 0 ? "Saw Amp " : "Body Amp ") + kKnobName[k], "",
                                                          kAmpKnobMin, kAmpKnobMax, kAmpKnobDefault};
    for (int k = 0; k < kPostEqSlots; ++k)
      a[static_cast<std::size_t>(kPostEqFirst + k)] = {"postEq" + std::to_string(k + 1),
                                                       "Post EQ " + std::to_string(k + 1) + " Gain", "dB", -18.0, 18.0, 0.0};
    for (int i = kSawCircuit; i < kNumParams; ++i) a[static_cast<std::size_t>(i)] = circuitParamSpec(i);
    return a;
  }();
  return s;
}

double clampTo(int index, double v) {
  const ParamSpec& s = paramSpec(index);
  if (!std::isfinite(v)) return s.def;
  return snapParam(std::clamp(v, s.min, s.max));
}

bool hasGain(EqType t) { return t != EqType::HighPass && t != EqType::LowPass; }

}  // namespace

double snapParam(double v) noexcept { return std::round(v * 1e4) / 1e4; }

const ParamSpec& paramSpec(int index) { return specs()[static_cast<std::size_t>(index)]; }

SlotBands postEqSlotBands(const Preset& p) {
  SlotBands s;
  s.fill(-1);
  int slot = 0;
  for (std::size_t i = 0; i < p.postEq.size() && slot < kPostEqSlots; ++i)
    if (hasGain(p.postEq[i].type)) s[static_cast<std::size_t>(slot++)] = static_cast<int>(i);
  return s;
}

ParamValues paramsFromPreset(const Preset& p) {
  ParamValues v{};
  v[kInputGain] = clampTo(kInputGain, p.inputGainDb);
  v[kOutputGain] = clampTo(kOutputGain, p.outputGainDb);
  v[kGateThreshold] = clampTo(kGateThreshold, activeDynamics(p).gate.thresholdDb);  // the parameter belongs to the active set
  v[kBlend] = clampTo(kBlend, p.blend);
  v[kLevelA] = clampTo(kLevelA, p.a.levelDb);
  v[kLevelB] = clampTo(kLevelB, p.b.levelDb);
  for (int path = 0; path < 2; ++path) {
    const AmpKnobs& a = (path == 0 ? p.a : p.b).ampControls;
    const double vals[kAmpKnobCount] = {a.gain, a.bass, a.mid, a.treble, a.presence, a.level};
    for (int k = 0; k < kAmpKnobCount; ++k) v[static_cast<std::size_t>(ampParam(path, k))] = clampTo(ampParam(path, k), vals[k]);
  }
  const SlotBands bands = postEqSlotBands(p);
  for (int k = 0; k < kPostEqSlots; ++k) {
    const int b = bands[static_cast<std::size_t>(k)];
    v[static_cast<std::size_t>(kPostEqFirst + k)] =
        b < 0 ? 0.0 : clampTo(kPostEqFirst + k, p.postEq[static_cast<std::size_t>(b)].gainDb);
  }
  circuitParamsFromPreset(p, v);
  return v;
}

void applyParams(Preset& p, const ParamValues& v) {
  p.inputGainDb = v[kInputGain];
  p.outputGainDb = v[kOutputGain];
  if (p.liveDynamics && effectiveDynamicsMode(p) == DynamicsMode::Live) p.liveDynamics->gate.thresholdDb = v[kGateThreshold];
  else p.gate.thresholdDb = v[kGateThreshold];  // record mode, or a live set derived from / equal to the record set
  p.blend = v[kBlend];
  p.a.levelDb = v[kLevelA];
  p.b.levelDb = v[kLevelB];
  for (int path = 0; path < 2; ++path) {  // the knobs only: gainStep is preset state, not a parameter
    AmpControls& a = (path == 0 ? p.a : p.b).ampControls;
    const auto val = [&](int k) { return v[static_cast<std::size_t>(ampParam(path, k))]; };
    a.gain = val(kAmpGain);
    a.bass = val(kAmpBass);
    a.mid = val(kAmpMid);
    a.treble = val(kAmpTreble);
    a.presence = val(kAmpPresence);
    a.level = val(kAmpLevel);
  }
  const SlotBands bands = postEqSlotBands(p);
  for (int k = 0; k < kPostEqSlots; ++k) {
    const int b = bands[static_cast<std::size_t>(k)];
    if (b >= 0) p.postEq[static_cast<std::size_t>(b)].gainDb = v[static_cast<std::size_t>(kPostEqFirst + k)];
  }
  applyCircuitParams(p, v);
}

Preset makeInitPreset() {
  Preset p;
  p.name = "Init";
  p.cab.enabled = false;
  p.align.mode = AlignMode::Off;
  p.cab.ir.file = kNoCaptureFile;  // the schema requires a file even when the cab is disabled; it is never loaded
  p.cab.ir.resolvedPath = p.cab.ir.file;
  return p;
}

Preset clampedToParams(Preset p) {
  // Live dynamics policy: a preset that has no dynamicsMode plays its LIVE set in the plugin (the plugin state is the preset, so
  // the mode is then written). Files and goldens outside the plugin keep the file-format default, "record".
  if (!p.dynamicsMode) p.dynamicsMode = DynamicsMode::Live;
  applyParams(p, paramsFromPreset(p));
  return p;
}

std::string presetToStateJson(const Preset& p) {
  namespace fs = std::filesystem;
  nlohmann::json j = toJson(p);
  auto absolutise = [](nlohmann::json& capture, const Capture& c) {
    if (c.resolvedPath.empty() || isNoCapture(c)) return;  // the placeholder is not a path
    std::error_code ec;  // absolute() throws if the working directory no longer exists
    const fs::path a = fs::absolute(c.resolvedPath, ec);
    capture["file"] = (ec ? c.resolvedPath : a).string();
  };
  const PathPreset* paths[2] = {&p.a, &p.b};
  const char* names[2] = {"a", "b"};
  for (int k = 0; k < 2; ++k)
    for (std::size_t i = 0; i < paths[k]->blocks.size(); ++i)
      if (const auto* nam = dynamic_cast<const NamBlockParams*>(paths[k]->blocks[i].params.get()))
        absolutise(j["paths"][names[k]]["blocks"][i]["model"], nam->model);
  if (p.cab.mode == CabMode::Shared) {
    absolutise(j["cab"]["ir"], p.cab.ir);
  } else {
    absolutise(j["cab"]["irA"], p.cab.irA);
    absolutise(j["cab"]["irB"], p.cab.irB);
  }
  return j.dump(2);
}

}  // namespace sawblade::plugin
