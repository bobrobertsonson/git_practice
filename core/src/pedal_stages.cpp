#include "sawblade/pedal_stages.h"

namespace sawblade::stages {

ClipKnees clipKnees(ClipType t) noexcept {
  switch (t) {
    case ClipType::Silicon: return {0.5, 0.5};
    case ClipType::Led: return {1.4, 1.4};
    case ClipType::Asymmetric: return {0.5, 0.3};
  }
  return {0.5, 0.5};
}

const char* clipTypeName(ClipType t) noexcept {
  switch (t) {
    case ClipType::Silicon: return "silicon";
    case ClipType::Led: return "led";
    case ClipType::Asymmetric: return "asymmetric";
  }
  return "silicon";
}

std::optional<ClipType> parseClipType(std::string_view s) {
  for (ClipType t : {ClipType::Silicon, ClipType::Led, ClipType::Asymmetric})
    if (s == clipTypeName(t)) return t;
  return std::nullopt;
}

}  // namespace sawblade::stages
