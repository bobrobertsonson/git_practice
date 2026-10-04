#pragma once

#include "sawblade/preset.h"

// Preset-side parameters of the modeled pedal blocks (docs/PRESET_SCHEMA.md). All knobs are
// 0..10 numbers; defaults are 5. `modelVersion` is 1; any other value is a PresetError, so a
// later re-fit of the models can bump it without silently changing old presets.
namespace sawblade {

constexpr int kPedalModelVersion = 1;
constexpr double kKnobMin = 0.0, kKnobMax = 10.0, kKnobDefault = 5.0;

struct HmParams {
  double level = kKnobDefault, low = kKnobDefault, high = kKnobDefault, distortion = kKnobDefault;
  bool operator==(const HmParams&) const = default;
};

struct TsParams {
  double drive = kKnobDefault, tone = kKnobDefault, level = kKnobDefault;
  bool operator==(const TsParams&) const = default;
};

struct HmBlockParams : BlockParams {
  HmParams p;
  bool equals(const BlockParams& other) const override;
  nlohmann::json toJson() const override;
};

struct TsBlockParams : BlockParams {
  TsParams p;
  bool equals(const BlockParams& other) const override;
  nlohmann::json toJson() const override;
};

// Registry parse hooks (type-specific members of the block object; the caller calls finish()).
std::shared_ptr<const BlockParams> parseHmBlock(JsonObject& o, const std::filesystem::path&);
std::shared_ptr<const BlockParams> parseTsBlock(JsonObject& o, const std::filesystem::path&);

// 0..10 knob -> level in dB: 3*level - 24 (0 dB at 8).
inline double pedalLevelDb(double level) { return 3.0 * level - 24.0; }

}  // namespace sawblade
