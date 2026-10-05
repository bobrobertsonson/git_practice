#pragma once

// Message-thread glue between the recorder, the play-along, the job runner and the audition: what MATCH and
// EXPORT NAM start from. Kept out of the UI so it is testable.

#include <filesystem>
#include <optional>
#include <string>

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

// Why a preset cannot be exported as written: a capture (NAM model, or the cab IR when the cab is on) with no file path.
// "" = fine.
std::string exportBlockedReason(const Preset& p);

struct ExportSource {
  bool ok = false;
  std::string message;
  std::filesystem::path file;          // resolved preset JSON handed to sawblade-export
  std::string sha256;                  // of the file's bytes: the "same rig" key
  std::string description;
};
// The auditioned / applied candidate's resolved preset if that is what is loaded, else the current preset
// written to <jobs>/inputs/<sha256 prefix>.preset.json (so the export is exactly what is playing, parameter changes
// included, and the same rig is always the same file). `dropComp`: the bus comp is switched off in what is written
// (a no-cab export cannot contain it; sawblade-export refuses a no-cab export of a rig with the comp on otherwise).
// `write` false: nothing is written (the key and the path the file would have).
ExportSource prepareExportSource(SawbladeProcessor& p, bool dropComp = false, bool write = true);

}  // namespace sawblade::plugin
