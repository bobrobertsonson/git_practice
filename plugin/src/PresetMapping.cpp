#include "PresetMapping.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

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
    for (int k = 0; k < kPostEqSlots; ++k)
      a[static_cast<std::size_t>(kPostEqFirst + k)] = {"postEq" + std::to_string(k + 1),
                                                       "Post EQ " + std::to_string(k + 1) + " Gain", "dB", -18.0, 18.0, 0.0};
    return a;
  }();
  return s;
}

double clampTo(int index, double v) {
  const ParamSpec& s = paramSpec(index);
  if (!std::isfinite(v)) return s.def;
  return std::clamp(v, s.min, s.max);
}

bool hasGain(EqType t) { return t != EqType::HighPass && t != EqType::LowPass; }

}  // namespace

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
  v[kGateThreshold] = clampTo(kGateThreshold, p.gate.thresholdDb);
  v[kBlend] = clampTo(kBlend, p.blend);
  v[kLevelA] = clampTo(kLevelA, p.a.levelDb);
  v[kLevelB] = clampTo(kLevelB, p.b.levelDb);
  const SlotBands bands = postEqSlotBands(p);
  for (int k = 0; k < kPostEqSlots; ++k) {
    const int b = bands[static_cast<std::size_t>(k)];
    v[static_cast<std::size_t>(kPostEqFirst + k)] =
        b < 0 ? 0.0 : clampTo(kPostEqFirst + k, p.postEq[static_cast<std::size_t>(b)].gainDb);
  }
  return v;
}

void applyParams(Preset& p, const ParamValues& v) {
  p.inputGainDb = v[kInputGain];
  p.outputGainDb = v[kOutputGain];
  p.gate.thresholdDb = v[kGateThreshold];
  p.blend = v[kBlend];
  p.a.levelDb = v[kLevelA];
  p.b.levelDb = v[kLevelB];
  const SlotBands bands = postEqSlotBands(p);
  for (int k = 0; k < kPostEqSlots; ++k) {
    const int b = bands[static_cast<std::size_t>(k)];
    if (b >= 0) p.postEq[static_cast<std::size_t>(b)].gainDb = v[static_cast<std::size_t>(kPostEqFirst + k)];
  }
}

Preset makeInitPreset() {
  Preset p;
  p.name = "Init";
  p.cab.enabled = false;
  p.align.mode = AlignMode::Off;
  p.cab.ir.file = "(none)";  // the schema requires a file even when the cab is disabled; it is never loaded
  p.cab.ir.resolvedPath = p.cab.ir.file;
  return p;
}

Preset clampedToParams(Preset p) {
  applyParams(p, paramsFromPreset(p));
  return p;
}

std::string presetToStateJson(const Preset& p) {
  namespace fs = std::filesystem;
  nlohmann::json j = toJson(p);
  auto absolutise = [](nlohmann::json& capture, const Capture& c) {
    if (c.resolvedPath.empty()) return;
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
