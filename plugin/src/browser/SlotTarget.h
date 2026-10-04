#pragma once

// Maps a rig piece (slot) to the preset capture it stands for, and substitutes a fetched capture
// (docs/specs/phase8_capture_browser.md "Slot <-> preset block mapping" and "Swap"). Pure, no JUCE.

#include <optional>
#include <string>
#include <vector>

#include "T3kJson.h"
#include "sawblade/preset.h"

namespace sawblade::plugin {

enum class Slot { SawPedal, BodyPedal, SawAmp, BodyAmp, Cab };

struct SlotTarget {
  enum class Kind { NamBlock, CabShared, CabA, CabB } kind = Kind::NamBlock;
  char path = 'a';        // NamBlock: 'a' or 'b'
  int blockIndex = -1;    // NamBlock: index in the path's blocks
  std::string label;      // "SAW CAB" etc. (for the per-path buttons)
  bool isIr() const noexcept { return kind != Kind::NamBlock; }
};

// The targets a slot has in `preset`: one for pedals / amps / the shared cab, two (irA, irB) for per-path
// cabs, none if the preset has no matching block (then `why` says so).
std::vector<SlotTarget> slotTargets(const Preset& preset, Slot slot, std::string* why = nullptr);

// Copy of `preset` with the capture at `target` replaced by the fetched file (file = resolvedPath = path,
// sha256, source). Everything else, including the block's gains, is kept. Empty optional and `error` set on
// a kind mismatch (an `ir` result for a block, a `nam` result for the cab) or a stale target.
std::optional<Preset> withCapture(const Preset& preset, const SlotTarget& target, const t3k::FetchResult& fetched, std::string& error);

const char* slotName(Slot s);          // "SAW PEDAL"
const char* slotGear(Slot s);          // CLI gear: pedal / amp / ir

}  // namespace sawblade::plugin
