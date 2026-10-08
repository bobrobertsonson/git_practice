#pragma once

#include <vector>

#include "sawblade/block_registry.h"
#include "sawblade/pedal_common.h"
#include "sawblade/preset.h"

// Preset-side parameters of the modeled pedal blocks (docs/PRESET_SCHEMA.md).
//  * pedal.ts: three 0..10 knobs, modelVersion 1.
//  * pedal.hm: modelVersion 3 (the calibrated voicing of phase 7c part 3), 2 (the phase 7b voicing) and
//    1 (phase 7: may set only the four stock knobs, maps onto the v2 defaults). Versions 1 and 2 render
//    bit-identically to before version 3 existed; a block built from HmParams{} is version 3.
//  * pedal.muff: modelVersion 1.
//  * pedal.rat: modelVersion 1.
// Any other modelVersion is a PresetError, so a later re-fit can bump it without silently changing
// old presets.
namespace sawblade {

constexpr int kPedalModelVersion = 1;  // pedal.ts
constexpr int kHmModelVersion = 3;     // pedal.hm: current (1 and 2 are still read)
constexpr int kHmModelVersionV2 = 2;
constexpr int kMuffModelVersion = 1;   // pedal.muff
constexpr int kRatModelVersion = 1;    // pedal.rat
constexpr double kKnobMin = 0.0, kKnobMax = 10.0, kKnobDefault = 5.0;
// pedal.rat stock VOLUME: 8 = unity on the shared level map (pedalLevelDb). With the voicing's fixed +6 dB output
// trim the stock pedal (DIST 5, FILTER 5, silicon) returns the RMS of a -12 dBFS-RMS DI riff within 1 dB
// (tests/test_pedal_rat.cpp measures and checks it).
constexpr double kRatStockVolume = 8.0;

enum class HmMode : int { Stock = 0, Custom = 1, Modded = 2 };
inline constexpr const char* kHmModeNames[3] = {"stock", "custom", "modded"};

struct HmParams {
  double level = 5.0, low = 5.0, high = 5.0, distortion = 5.0;
  double tightness = 0.0, mix = 100.0;
  HmMode mode = HmMode::Stock;
  ClipType clip = ClipType::Silicon;
  Clip2Type clip2 = Clip2Type::Follow;
  double lowFreq = 100.0, lowQ = 0.8, highFreq = 1000.0, highSpread = 1.5;
  double presenceFreq = 4800.0, presenceDb = 8.0, rolloffHz = 16000.0;  // v3 default (v2: 9000)
  double gain1Db = 0.0, gain2Db = 0.0, bias = 0.0;
  // Version 3 only: preset-static trims of `custom` mode (not live, not on the face or drawer).
  double customLowDb = 3.2, customHighDb = 3.0;
  // 3 = the calibrated voicing (default), 2 = the phase 7 / 7b voicing (a v1 preset reads as 2).
  int modelVersion = 3;
  bool operator==(const HmParams&) const = default;
  // The phase 7b parameter set: version 2, roll-off 9 kHz.
  static HmParams v2() noexcept {
    HmParams p;
    p.modelVersion = 2;
    p.rolloffHz = 9000.0;
    return p;
  }
};

struct MuffParams {
  double volume = 5.0, sustain = 5.0, tone = 5.0, scoop = 3.0, crunch = 5.0, voice = 5.0;
  double tightness = 0.0, mix = 100.0;
  ClipType clip = ClipType::Silicon;
  Clip2Type clip2 = Clip2Type::Follow;
  double stackRatio = 4.4, rolloffHz = 10000.0, gain2Db = 0.0, bias = 0.0;
  bool operator==(const MuffParams&) const = default;
};

struct TsParams {
  double drive = kKnobDefault, tone = kKnobDefault, level = kKnobDefault;
  bool operator==(const TsParams&) const = default;
};

// pedal.rat clipper set. Its own enum: the shared ClipType has no `none`, and `asymmetric` here is a 1+2 diode string.
enum class RatClip : int { Silicon = 0, Led = 1, None = 2, Asymmetric = 3 };
constexpr int kNumRatClips = 4;
inline constexpr const char* kRatClipNames[kNumRatClips] = {"silicon", "led", "none", "asymmetric"};

// Rat-style distortion (pedal.rat, display name VERMIN).
struct RatParams {
  double distortion = kKnobDefault, filter = kKnobDefault, volume = kRatStockVolume;
  double tightness = 0.0, mix = 100.0;
  RatClip clip = RatClip::Silicon;
  bool ruetz = false;
  bool operator==(const RatParams&) const = default;
};

struct HmBlockParams : BlockParams {
  HmParams p;
  bool equals(const BlockParams& other) const override;
  nlohmann::json toJson() const override;
};

struct MuffBlockParams : BlockParams {
  MuffParams p;
  bool equals(const BlockParams& other) const override;
  nlohmann::json toJson() const override;
};

struct RatBlockParams : BlockParams {
  RatParams p;
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
std::shared_ptr<const BlockParams> parseMuffBlock(JsonObject& o, const std::filesystem::path&);
std::shared_ptr<const BlockParams> parseRatBlock(JsonObject& o, const std::filesystem::path&);
std::shared_ptr<const BlockParams> parseTsBlock(JsonObject& o, const std::filesystem::path&);

// 0..10 knob -> level in dB: 3*level - 24 (0 dB at 8).
inline double pedalLevelDb(double level) { return 3.0 * level - 24.0; }

// ---- live parameters (Processor::setLiveParams order) -------------------------------------------------
enum HmLive : int {
  kHmLevel, kHmLow, kHmHigh, kHmDistortion, kHmTightness, kHmMix, kHmMode, kHmClip, kHmClip2, kHmLowFreq,
  kHmLowQ, kHmHighFreq, kHmHighSpread, kHmPresenceFreq, kHmPresenceDb, kHmRolloffHz, kHmGain1Db, kHmGain2Db,
  kHmBias, kHmNumLive
};
enum MuffLive : int {
  kMuffVolume, kMuffSustain, kMuffTone, kMuffScoop, kMuffCrunch, kMuffVoice, kMuffTightness, kMuffMix,
  kMuffClip, kMuffClip2, kMuffStackRatio, kMuffRolloffHz, kMuffGain2Db, kMuffBias, kMuffNumLive
};

enum RatLive : int { kRatDistortion, kRatFilter, kRatVolume, kRatTightness, kRatMix, kRatClip, kRatRuetz, kRatNumLive };

// Descriptors in enum order (key = the JSON key; ranges and defaults are the preset schema's).
std::vector<LiveParamDesc> hmLiveParamDescs();
std::vector<LiveParamDesc> muffLiveParamDescs();
std::vector<LiveParamDesc> ratLiveParamDescs();

// Pure converters. RT-safe, never throw: a missing (index >= count) or non-finite entry takes the
// default, everything is clamped into its range, enum indexes are rounded.
HmParams hmParamsFromLive(const float* values, int count) noexcept;
void hmLiveFromParams(const HmParams& p, float* values /* kHmNumLive */) noexcept;
MuffParams muffParamsFromLive(const float* values, int count) noexcept;
void muffLiveFromParams(const MuffParams& p, float* values /* kMuffNumLive */) noexcept;
RatParams ratParamsFromLive(const float* values, int count) noexcept;
void ratLiveFromParams(const RatParams& p, float* values /* kRatNumLive */) noexcept;

}  // namespace sawblade
