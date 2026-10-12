#pragma once

// Gain ladders in the plugin (v0.2 Task B): the pure parts. `sawblade-t3k ladder <tone id> --size S --json` prints
// {"tone_id", "size", "rungs": [{"model_id", "gain", "name"}, ...] | null}; these helpers parse that, find the amp captures
// that still need a ladder and apply a fetched one to a preset. JUCE-free; the processor runs the tool (T3kTool).

#include <optional>
#include <string>
#include <vector>

#include "sawblade/preset.h"

namespace sawblade::plugin {

struct LadderFetchResult {
  bool ok = false;               // the output was a ladder document (rungs may still be "none")
  std::string error;             // !ok: why
  std::string toneId;
  std::vector<LadderRung> rungs;  // ascending gain; empty = "no ladder" (rungs null in the document)
};

// The model size passed to the tool. The capture's own size is not recorded in the preset, so this is the tool's
// default; a ladder whose own model is not a rung of that size is simply not used.
constexpr const char* kLadderSize = "standard";

std::vector<std::string> ladderArgs(const std::string& toneId, const std::string& size = kLadderSize);

// Parses the tool's output (stdout + stderr merged: the JSON document is the span from the first '{' to the last '}').
// Anything that is not a well-formed ladder document, or a ladder with fewer than 2 rungs / duplicate ids or gains /
// a gain outside [0, 100], is not ok (never guessed).
LadderFetchResult parseLadderOutput(const std::string& output);

// TONE3000 tone ids of the preset's amp blocks (ampIndex of an enabled or disabled path) whose capture has a source id
// and model id but no ladder yet, without duplicates.
std::vector<std::string> toneIdsNeedingLadder(const Preset& p);

// Gives every amp block of `p` that is a capture of tone `toneId` and has no ladder the fetched `rungs`, provided the block's
// own model id is one of them. A path whose GAIN knob is untouched (5) and has no gainStep gets the knob placed at the own
// rung's position, so its sound is unchanged. Returns whether anything changed.
bool applyLadderToPreset(Preset& p, const std::string& toneId, const std::vector<LadderRung>& rungs);

}  // namespace sawblade::plugin
