#include "sawblade/pedal_params.h"

#include <limits>

namespace sawblade {
namespace {

int parseModelVersion(JsonObject& o) {
  const int v = o.integer("modelVersion", kPedalModelVersion, std::numeric_limits<int>::min(),
                          std::numeric_limits<int>::max());
  if (v != kPedalModelVersion)
    throw PresetError(o.child("modelVersion"), "unsupported modelVersion " + std::to_string(v) +
                                                   " (this build supports " + std::to_string(kPedalModelVersion) + ")");
  return v;
}

double knob(JsonObject& po, const char* key) { return po.number(key, kKnobDefault, kKnobMin, kKnobMax); }

}  // namespace

std::shared_ptr<const BlockParams> parseHmBlock(JsonObject& o, const std::filesystem::path&) {
  auto b = std::make_shared<HmBlockParams>();
  parseModelVersion(o);
  if (auto po = o.optionalObject("params")) {
    b->p.level = knob(*po, "level");
    b->p.low = knob(*po, "low");
    b->p.high = knob(*po, "high");
    b->p.distortion = knob(*po, "distortion");
    po->finish();
  }
  return b;
}

std::shared_ptr<const BlockParams> parseTsBlock(JsonObject& o, const std::filesystem::path&) {
  auto b = std::make_shared<TsBlockParams>();
  parseModelVersion(o);
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
  return {{"modelVersion", kPedalModelVersion},
          {"params", {{"level", p.level}, {"low", p.low}, {"high", p.high}, {"distortion", p.distortion}}}};
}

bool TsBlockParams::equals(const BlockParams& other) const {
  const auto* o = dynamic_cast<const TsBlockParams*>(&other);
  return o && p == o->p;
}
nlohmann::json TsBlockParams::toJson() const {
  return {{"modelVersion", kPedalModelVersion}, {"params", {{"drive", p.drive}, {"tone", p.tone}, {"level", p.level}}}};
}

}  // namespace sawblade
