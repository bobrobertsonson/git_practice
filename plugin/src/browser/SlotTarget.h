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
  // InsertNamBlock (v0.4 Task B): USE adds a NEW nam block (slot "pedal") to the pedalboard of `path` at `blockIndex`, instead of
  // replacing a capture.
  enum class Kind { NamBlock, CabShared, CabA, CabB, InsertNamBlock } kind = Kind::NamBlock;
  char path = 'a';        // NamBlock / InsertNamBlock: 'a' or 'b'
  int blockIndex = -1;    // NamBlock: index in the path's blocks; InsertNamBlock: the index the new block gets
  std::string label;      // "SAW CAB" etc. (for the per-path buttons)
  bool isIr() const noexcept { return kind == Kind::CabShared || kind == Kind::CabA || kind == Kind::CabB; }
};

// Where an insert-mode browser puts its capture: path 'a' / 'b' and the index in that path's blocks (before the amp).
struct InsertPoint {
  char path = 'a';
  int index = 0;
};

// The targets a slot has in `preset`: one for pedals / amps / the shared cab, two (irA, irB) for per-path
// cabs, none if the preset has no matching block (then `why` says so).
// v0.4 Task D: `pinnedBlockId` (pedal and amp slots only; "" = none) names the block the user picked on the pedalboard: the target is
// exactly that block (in the slot's path) when it is a `nam` block, and there is no target (with a `why`) when it is a modeled
// circuit or no longer exists. Without it the slot's usual heuristic picks the block.
// `insert` (pedal slots only): the single target is an InsertNamBlock at that point (none, with a reason, when the path has 8 blocks).
std::vector<SlotTarget> slotTargets(const Preset& preset, Slot slot, std::string* why = nullptr, const std::string& pinnedBlockId = {},
                                    const std::optional<InsertPoint>& insert = std::nullopt);

// Copy of `preset` with the capture at `target` replaced by the fetched file (file = resolvedPath = path,
// sha256, source); for an InsertNamBlock target a new nam block (slot "pedal", fresh id) holding it is inserted. Everything else, including the block's gains, is kept. Empty optional and `error` set on
// a kind mismatch (an `ir` result for a block, a `nam` result for the cab) or a stale target.
std::optional<Preset> withCapture(const Preset& preset, const SlotTarget& target, const t3k::FetchResult& fetched, std::string& error);

const char* slotName(Slot s);          // "SAW PEDAL"
const char* slotGear(Slot s);          // CLI gear: pedal / amp / ir

}  // namespace sawblade::plugin
