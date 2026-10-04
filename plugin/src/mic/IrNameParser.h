#pragma once

#include <limits>
#include <string>
#include <string_view>

// JUCE-free parser of IR model names ("V30 UL 4FB 4x12 SM57 0.50in", "SM57_CapEdge_1in", ...) into a mic
// shot description (docs/specs/phase9a_mic_page.md section 3). Tolerant: case-insensitive, any separator,
// unknown tokens are ignored, never throws.
namespace sawblade::plugin::mic {

enum class MicType { Unknown, Dynamic, Ribbon, Condenser };
enum class MicPosition { Unknown, Cap, CapEdge, Cone, Edge, OffAxis };

struct MicShot {
  std::string speaker = "unknown";  // "V30", "G12T75", "G12M", "G12H", "G12", "Greenback", "Creamback", "EVM12L", ... or "unknown"
  int speakerSlot = 0;              // 1..4 (UL/UR/LL/LR, TL/TR/BL/BR, "V30 1", "spk2"), 0 = unknown
  std::string mic = "unknown";      // "SM57", "MD421", "e906", "R121", "U87", "M160", "SM7B", "i5", "C414", ... or "unknown"
  MicType micType = MicType::Unknown;
  double distanceIn = std::numeric_limits<double>::quiet_NaN();  // inches, NaN if unknown
  MicPosition position = MicPosition::Unknown;
  int positionIndex = 0;            // a bare numeric position ("SM57 3", trailing "- V30 3"), 0 = none
  std::string cabSize;              // "4x12", "2x12", "1x12" or "" (unknown); "4FB" is not a cab size
  std::string raw;                  // the original name
};

MicShot parseIrName(std::string_view name) noexcept;

const char* micTypeName(MicType t) noexcept;      // "dynamic" / "ribbon" / "condenser" / "unknown"
const char* positionName(MicPosition p) noexcept;  // "CAP" / "CAP EDGE" / "CONE" / "EDGE" / "OFF AXIS" / "UNKNOWN"

}  // namespace sawblade::plugin::mic
