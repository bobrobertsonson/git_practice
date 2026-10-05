#pragma once

// Message-thread glue between the recorder, the play-along, the job runner and the audition: what MATCH and
// EXPORT NAM start from. Kept out of the UI so it is testable.

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "JobRunner.h"
#include "TakeRecorder.h"
#include "sawblade/preset.h"

namespace sawblade::plugin {

class SawbladeProcessor;

std::optional<TakeInfo> selectedTake(SawbladeProcessor& p);  // the take chosen with "Use for MATCH" (if it still exists)

struct MatchPlan {
  bool ok = false;
  std::string message;                 // why MATCH cannot start / a caution
  MatchRequest request;
  ReferenceChoice reference;
  std::optional<TakeInfo> take;
  std::string offsetNote;              // how the DI is placed in the song, for the screen
};
// reference = the loaded song's guitar stem (else other, else the mix / first file), DI = the selected take,
// offset = the take's stem sample index at its first sample (only if it was recorded against this song).
MatchPlan planMatch(SawbladeProcessor& p);
// The loaded song's name: the folder name, or the song file's stem (never the separation cache directory). "" = none.
std::string activeSongName(SawbladeProcessor& p);

// Why a preset cannot be exported as written: a capture (NAM model, or the cab IR when the cab is on) with no file path.
// "" = fine.
std::string exportBlockedReason(const Preset& p);

struct ExportSource {
  bool ok = false;
  std::string message;
  std::filesystem::path file;          // resolved preset JSON handed to sawblade-export
  std::string description;
};
// The auditioned / applied candidate's resolved preset if that is what is loaded, else the current preset
// written to <jobs>/inputs/ (so the export is exactly what is playing, parameter changes included).
ExportSource prepareExportSource(SawbladeProcessor& p);

// ---- two-pass MATCH (docs/specs/phase6a_1_quick_then_thorough.md) ----------------------------------------------------

// Pure: are two preset JSONs the same chain? Used for auto-promote (an applied quick candidate that is the same chain
// as the thorough best only changes its badge; nothing is loaded). Same chain means:
//  - the same structure: the same keys, the same arrays of the same length (paths, blocks in order and their types and
//    slots, EQ bands, cab mode), the same flags and strings;
//  - the same captures in the same slots: a capture compares by its TONE3000 source (provider, id, modelId) when both
//    have one, else by file name (the directory is ignored);
//  - every dB-valued parameter (any number whose key ends in "Db": level, gain, input / output gain, EQ gain,
//    threshold ...) within kSameChainDbTolerance;
//  - every other number equal within a small tolerance: `blend` within kSameChainBlendTolerance (absolute), the rest
//    (frequencies, q, times, ratios, align samples) within a relative kSameChainRelTolerance.
// Names, notes and block ids, and the plugin-only "playAlong" object, are ignored. Anything else that differs is a
// different chain: the function errs on the side of "different", which only costs a badge.
constexpr double kSameChainDbTolerance = 0.5;
constexpr double kSameChainBlendTolerance = 0.01;
constexpr double kSameChainRelTolerance = 1e-3;
bool sameChain(const nlohmann::json& a, const nlohmann::json& b);
bool sameChainFiles(const std::filesystem::path& a, const std::filesystem::path& b);  // false if either cannot be read

// USE FOR MATCH: remembers the take and, if it is another take than before, cancels a running refinement (its result
// would belong to the old take).
void chooseTakeForMatch(SawbladeProcessor& p, const std::string& name);

// Rename / delete of a take go through here: if it is the take selected for MATCH, a running or pending refinement is
// cancelled first and the selection follows (the new name / none).
bool renameTakeForMatch(SawbladeProcessor& p, const std::string& oldName, const std::string& newName, std::string* error = nullptr);
bool deleteTakeForMatch(SawbladeProcessor& p, const std::string& name);

// Loads the thorough pass's best candidate through the normal audition path and applies it (APPLY REFINED BEST).
// False (and *error) if there is no refined result or it cannot be loaded.
bool applyRefinedBest(SawbladeProcessor& p, std::string* error = nullptr);

// Is the applied preset a quick candidate that is the same chain as the refined best? (Cheap enough for the UI timer:
// the answer is remembered per pair of files.)
bool appliedQuickIsRefinedBest(SawbladeProcessor& p, const JobSnapshot& quick, const JobSnapshot& refine);

}  // namespace sawblade::plugin
