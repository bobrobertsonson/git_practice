#include "IrNameParser.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace sawblade::plugin::mic {
namespace {

struct Entry {
  const char* key;
  const char* name;
};

// Keys are lower case with no separators (multi-token spellings are joined: "v 30" -> "v30").
constexpr Entry kSpeakers[] = {
    {"v30", "V30"},         {"vintage30", "V30"},   {"g12t75", "G12T75"},   {"t75", "G12T75"},     {"g12m", "G12M"},
    {"g12m25", "G12M"},     {"g12h", "G12H"},       {"g12h30", "G12H"},     {"greenback", "Greenback"}, {"greenbacks", "Greenback"},
    {"gb", "Greenback"},    {"creamback", "Creamback"}, {"g12m65", "Creamback"}, {"evm12l", "EVM12L"}, {"evm", "EVM12L"},
    {"k100", "K100"},       {"c12n", "C12N"},       {"g12", "G12"},
};

struct MicEntry {
  const char* key;
  const char* name;
  MicType type;
};
constexpr MicEntry kMics[] = {
    {"sm57", "SM57", MicType::Dynamic},    {"sm58", "SM58", MicType::Dynamic},     {"sm7b", "SM7B", MicType::Dynamic},
    {"sm7", "SM7B", MicType::Dynamic},     {"md421", "MD421", MicType::Dynamic},   {"md441", "MD441", MicType::Dynamic},
    {"e906", "e906", MicType::Dynamic},    {"e609", "e609", MicType::Dynamic},     {"e835", "e835", MicType::Dynamic},
    {"m201", "M201", MicType::Dynamic},    {"i5", "i5", MicType::Dynamic},         {"pr30", "PR30", MicType::Dynamic},
    {"beta57", "Beta57", MicType::Dynamic}, {"r121", "R121", MicType::Ribbon},     {"r122", "R122", MicType::Ribbon},
    {"royer121", "R121", MicType::Ribbon}, {"m160", "M160", MicType::Ribbon},      {"u87", "U87", MicType::Condenser},
    {"u87ai", "U87", MicType::Condenser},  {"u47", "U47", MicType::Condenser},     {"c414", "C414", MicType::Condenser},
    {"c451", "C451", MicType::Condenser},  {"ksm32", "KSM32", MicType::Condenser}, {"sm81", "SM81", MicType::Condenser},
};

struct SlotEntry {
  const char* key;
  int slot;
};
constexpr SlotEntry kSlots[] = {{"ul", 1}, {"ur", 2}, {"ll", 3}, {"lr", 4}, {"tl", 1}, {"tr", 2}, {"bl", 3}, {"br", 4}};

bool isSep(char c) {
  switch (c) {
    case ' ': case '\t': case '_': case '-': case ',': case '(': case ')': case '[': case ']': case '/': case '|': case '+':
    case ';': case ':': case '\r': case '\n':
      return true;
    default:
      return false;
  }
}

// Lower-cases ASCII, maps the UTF-8 middle dot to a space and the curly / prime inch marks to '"'.
std::string normalise(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    const auto next = [&](std::size_t k) { return i + k < s.size() ? static_cast<unsigned char>(s[i + k]) : 0u; };
    if (c == 0xC2 && next(1) == 0xB7) {  // middle dot
      out.push_back(' ');
      ++i;
    } else if (c == 0xE2 && next(1) == 0x80 && (next(2) == 0x9D || next(2) == 0x9C)) {  // curly double quotes
      out.push_back('"');
      i += 2;
    } else if (c == 0xE2 && next(1) == 0x80 && next(2) == 0xB3) {  // double prime
      out.push_back('"');
      i += 2;
    } else if (c == 0xC2 && next(1) == 0xB0) {  // degree sign: keep as one byte marker
      out.push_back('\x01');
      ++i;
    } else {
      out.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  return out;
}

std::vector<std::string> tokenise(const std::string& s) {
  std::vector<std::string> t;
  std::string cur;
  for (char c : s) {
    if (isSep(c)) {
      if (!cur.empty()) t.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) t.push_back(std::move(cur));
  return t;
}

bool isDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

// number = digits [ '.' digits ]
bool parseNumber(const std::string& s, double& v) {
  if (s.empty()) return false;
  std::size_t dots = 0;
  for (char c : s) {
    if (c == '.') ++dots;
    else if (c < '0' || c > '9') return false;
  }
  if (dots > 1 || s.front() == '.' || s.back() == '.') return false;
  v = std::strtod(s.c_str(), nullptr);
  return true;
}

// "0.50in", "1\"", "25mm", "2.5cm" -> inches.
bool parseDistanceToken(const std::string& s, double& inches) {
  std::size_t n = 0;
  while (n < s.size() && ((s[n] >= '0' && s[n] <= '9') || s[n] == '.')) ++n;
  if (n == 0 || n == s.size()) return false;
  double v;
  if (!parseNumber(s.substr(0, n), v)) return false;
  const std::string unit = s.substr(n);
  if (unit == "in" || unit == "inch" || unit == "inches" || unit == "\"") inches = v;
  else if (unit == "mm") inches = v / 25.4;
  else if (unit == "cm") inches = v / 2.54;
  else return false;
  return true;
}

bool isUnit(const std::string& s, double& scale) {
  if (s == "in" || s == "inch" || s == "inches" || s == "\"") scale = 1.0;
  else if (s == "mm") scale = 1.0 / 25.4;
  else if (s == "cm") scale = 1.0 / 2.54;
  else return false;
  return true;
}

const Entry* findSpeaker(const std::string& k) {
  for (const auto& e : kSpeakers)
    if (k == e.key) return &e;
  return nullptr;
}
const MicEntry* findMic(const std::string& k) {
  for (const auto& e : kMics)
    if (k == e.key) return &e;
  return nullptr;
}
int findSlot(const std::string& k) {
  for (const auto& e : kSlots)
    if (k == e.key) return e.slot;
  return 0;
}

// "4x12" style cab size; only 1x12 / 2x12 / 4x12 are cab sizes.
std::string cabSizeOf(const std::string& t) {
  if (t.size() == 4 && t[1] == 'x' && t.compare(2, 2, "12") == 0 && (t[0] == '1' || t[0] == '2' || t[0] == '4')) return t;
  return {};
}

// "spk2", "speaker3", "spkr1" -> slot.
int spkSlotToken(const std::string& t) {
  for (const char* p : {"speaker", "spkr", "spk"}) {
    const std::string pre(p);
    if (t.size() == pre.size() + 1 && t.compare(0, pre.size(), pre) == 0 && t.back() >= '1' && t.back() <= '4') return t.back() - '0';
  }
  return 0;
}

enum class Kind { None, Speaker, Mic, Other };

MicShot parse(std::string_view name) {
  MicShot m;
  m.raw = std::string(name);
  const std::vector<std::string> tok = tokenise(normalise(name));
  Kind prev = Kind::None;
  for (std::size_t i = 0; i < tok.size(); ++i) {
    const std::string& t = tok[i];
    const std::string* next = i + 1 < tok.size() ? &tok[i + 1] : nullptr;
    const std::string pair = next ? t + *next : std::string();

    // --- speaker / mic types: the joined pair first ("v","30" / "sm","57" / "g12","t75"), then the token.
    if (next) {
      if (const Entry* e = findSpeaker(pair)) {
        if (m.speaker == "unknown") m.speaker = e->name;
        prev = Kind::Speaker;
        ++i;
        continue;
      }
      if (const MicEntry* e = findMic(pair)) {
        if (m.mic == "unknown") {
          m.mic = e->name;
          m.micType = e->type;
        }
        prev = Kind::Mic;
        ++i;
        continue;
      }
    }
    if (const Entry* e = findSpeaker(t)) {
      if (m.speaker == "unknown") m.speaker = e->name;
      prev = Kind::Speaker;
      continue;
    }
    if (const MicEntry* e = findMic(t)) {
      if (m.mic == "unknown") {
        m.mic = e->name;
        m.micType = e->type;
      }
      prev = Kind::Mic;
      continue;
    }

    // --- cab size
    if (const std::string cs = cabSizeOf(t); !cs.empty()) {
      if (m.cabSize.empty()) m.cabSize = cs;
      prev = Kind::Other;
      continue;
    }

    // --- speaker slot words
    if (const int s = findSlot(t); s != 0) {
      m.speakerSlot = s;
      prev = Kind::Other;
      continue;
    }
    if (const int s = spkSlotToken(t); s != 0) {
      m.speakerSlot = s;
      prev = Kind::Other;
      continue;
    }
    if (next && (t == "speaker" || t == "spkr" || t == "spk") && isDigits(*next)) {
      const int n = std::atoi(next->c_str());
      if (n >= 1 && n <= 4) m.speakerSlot = n;
      ++i;
      prev = Kind::Other;
      continue;
    }

    // --- distance: "0.50in" / "1\"" / "25mm" in one token, or a number followed by a unit token
    double inches = 0.0;
    if (parseDistanceToken(t, inches)) {
      m.distanceIn = inches;
      prev = Kind::Other;
      continue;
    }
    double num = 0.0, scale = 1.0;
    if (next && parseNumber(t, num) && isUnit(*next, scale)) {
      m.distanceIn = num * scale;
      ++i;
      prev = Kind::Other;
      continue;
    }

    // --- position words
    if (t == "cap" || t == "dustcap" || t == "center" || t == "centre" || t == "dome") {
      if (next && *next == "edge") {
        m.position = MicPosition::CapEdge;
        ++i;
      } else if (m.position == MicPosition::Unknown) {
        m.position = MicPosition::Cap;
      }
      prev = Kind::Other;
      continue;
    }
    if (t == "capedge" || t == "cape" || t == "ce") {
      m.position = MicPosition::CapEdge;
      prev = Kind::Other;
      continue;
    }
    if (t == "cone") {
      m.position = MicPosition::Cone;
      prev = Kind::Other;
      continue;
    }
    if (t == "edge") {
      if (m.position == MicPosition::Unknown) m.position = MicPosition::Edge;
      prev = Kind::Other;
      continue;
    }
    if (t == "offaxis" || t == "oa" || (t == "off" && next && *next == "axis")) {
      m.position = MicPosition::OffAxis;
      if (t == "off") ++i;
      prev = Kind::Other;
      continue;
    }
    {  // "45°" / "45deg": an angle off the axis
      std::size_t n = 0;
      while (n < t.size() && t[n] >= '0' && t[n] <= '9') ++n;
      const std::string unit = t.substr(n);
      if (n > 0 && (unit == "\x01" || unit == "deg") && std::atoi(t.substr(0, n).c_str()) >= 15) {
        m.position = MicPosition::OffAxis;
        prev = Kind::Other;
        continue;
      }
    }

    // --- bare integers: a slot right after the speaker type, otherwise a position index after a mic / speaker
    if (isDigits(t) && t.size() <= 3) {
      const int n = std::atoi(t.c_str());
      if (prev == Kind::Speaker) {
        if (m.speakerSlot == 0 && n >= 1 && n <= 4) m.speakerSlot = n;
        else if (m.positionIndex == 0 && n >= 1) m.positionIndex = n;
      } else if (prev == Kind::Mic) {
        if (m.positionIndex == 0 && n >= 1) m.positionIndex = n;
      }
      prev = Kind::Other;
      continue;
    }
    prev = Kind::Other;  // unknown token: ignored
  }
  return m;
}

}  // namespace

MicShot parseIrName(std::string_view name) noexcept {
  try {
    return parse(name);
  } catch (...) {
    MicShot m;
    try {
      m.raw = std::string(name);
    } catch (...) {
    }
    return m;
  }
}

const char* micTypeName(MicType t) noexcept {
  switch (t) {
    case MicType::Dynamic: return "dynamic";
    case MicType::Ribbon: return "ribbon";
    case MicType::Condenser: return "condenser";
    case MicType::Unknown: break;
  }
  return "unknown";
}

const char* positionName(MicPosition p) noexcept {
  switch (p) {
    case MicPosition::Cap: return "CAP";
    case MicPosition::CapEdge: return "CAP EDGE";
    case MicPosition::Cone: return "CONE";
    case MicPosition::Edge: return "EDGE";
    case MicPosition::OffAxis: return "OFF AXIS";
    case MicPosition::Unknown: break;
  }
  return "UNKNOWN";
}

}  // namespace sawblade::plugin::mic
