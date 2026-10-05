#pragma once

#include <vector>

#include "sawblade/block_registry.h"
#include "sawblade/pedal_common.h"
#include "sawblade/preset.h"

// Preset-side parameters of the chainsaw-family blocks pedal.hmx ("modded chainsaw distortion")
// and pedal.eye ("one-knob chainsaw"); phase 7c, docs/specs/phase7c_chainsaw_family.md. Conventions
// identical to phase 7b: knobs 0..10 doubles, `tightness` 0..10 (default 0), `mix` 0..100 %
// (default 100), enums as lower-case strings, modelVersion 1. The clip type is 7b's four-way
// ClipType (pedal_common.h): silicon | led | asymmetric | soft.
namespace sawblade {

constexpr double kSawTightnessDefault = 0.0, kSawMixMin = 0.0, kSawMixMax = 100.0, kSawMixDefault = 100.0;

enum class MidVoice : int { Stock = 0, Low = 1, High = 2 };
constexpr int kNumMidVoices = 3;
inline constexpr const char* kMidVoiceNames[kNumMidVoices] = {"stock", "low", "high"};

struct HmxParams {
  double level = 5.0, low = 5.0, lowMid = 5.0, highMid = 5.0, high = 5.0, distortion = 5.0, presence = 5.0;
  double tightness = kSawTightnessDefault, mix = kSawMixDefault;
  ClipType clip = ClipType::Silicon;
  bool boost = false;
  double lowMidFreq = 5.0, highMidFreq = 5.0;
  MidVoice midVoice = MidVoice::Stock;  // base centre and Q of the HIGH-MID band (7c part 3, 3.7)
  bool operator==(const HmxParams&) const = default;
};

struct EyeParams {
  double gain = 5.0, level = 5.0, tightness = kSawTightnessDefault;
  bool operator==(const EyeParams&) const = default;
};

struct HmxBlockParams : BlockParams {
  HmxParams p;
  bool equals(const BlockParams& other) const override;
  nlohmann::json toJson() const override;
};

struct EyeBlockParams : BlockParams {
  EyeParams p;
  bool equals(const BlockParams& other) const override;
  nlohmann::json toJson() const override;
};

std::shared_ptr<const BlockParams> parseHmxBlock(JsonObject& o, const std::filesystem::path&);
std::shared_ptr<const BlockParams> parseEyeBlock(JsonObject& o, const std::filesystem::path&);

// Live parameter index order (the order of the spec tables), the Processor::setLiveParams order.
// Doubles are narrowed to float, enums are the choice index (clip: silicon 0, led 1, asymmetric 2,
// soft 3), boost is 0/1, midVoice is the choice index (stock 0, low 1, high 2). The *FromLive converters read min(n, kNumLive) values (missing ones keep their
// defaults), clamp knobs to their ranges, round enums to the nearest valid index and treat
// boost >= 0.5 as on; they never throw.
enum HmxLive {
  kHmxLevel = 0,
  kHmxLow,
  kHmxLowMid,
  kHmxHighMid,
  kHmxHigh,
  kHmxDistortion,
  kHmxPresence,
  kHmxTightness,
  kHmxMix,
  kHmxClip,
  kHmxBoost,
  kHmxLowMidFreq,
  kHmxHighMidFreq,
  kHmxMidVoice,  // appended last (7c part 3): choice 0/1/2 = stock/low/high
  kHmxNumLive
};
enum EyeLive { kEyeGain = 0, kEyeLevel, kEyeTightness, kEyeNumLive };

// Descriptors in HmxLive / EyeLive order (key = the JSON key; ranges and defaults are the schema's;
// `choices` for the enums clip and boost). The registry rows and the plugin read these.
std::vector<LiveParamDesc> hmxLiveParamDescs();
std::vector<LiveParamDesc> eyeLiveParamDescs();

HmxParams hmxParamsFromLive(const float* v, int n) noexcept;
void hmxLiveFromParams(const HmxParams& p, float* v) noexcept;  // writes kHmxNumLive values
EyeParams eyeParamsFromLive(const float* v, int n) noexcept;
void eyeLiveFromParams(const EyeParams& p, float* v) noexcept;  // writes kEyeNumLive values

}  // namespace sawblade
