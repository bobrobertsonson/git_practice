#pragma once

// Audition / A-B / apply of a match candidate (docs/PLUGIN.md "Record + Match"). Message thread only.
//
// Auditioning loads the candidate's resolved preset through the processor's normal off-thread load path
// (loadPreset: the EngineLoader builds it, the audio thread cross-fades to it). The preset that was current
// when the audition started is kept, so A (the original) and B (the candidate) can be toggled; auditioning
// another candidate keeps A. Apply makes the candidate the current preset (it is what a normal preset load
// would leave), Revert goes back to A.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "sawblade/preset.h"

namespace sawblade::plugin {

class SawbladeProcessor;

class PresetAudition {
 public:
  struct State {
    bool active = false;                 // an audition is in progress (a preset is kept for A/B)
    bool onCandidate = false;            // B is loaded (else A)
    std::filesystem::path candidate;     // the candidate's resolved preset file
    std::string candidateName;           // its preset name
    std::string originalName;            // A's preset name
  };

  explicit PresetAudition(SawbladeProcessor& p) : proc_(p) {}

  bool audition(const std::filesystem::path& resolvedPreset, std::string* error = nullptr);
  bool toggleAB();   // false when no audition is active
  // apply() and takeUndoStep() are message-thread only: the undo step has no lock (nor does any other member of this class).
  bool apply();      // false when no audition is active; leaves the candidate loaded and ends the audition
  bool revert();     // back to A and ends the audition
  State state() const { return state_; }
  // The one undo step an apply() leaves behind (the rig's Cmd / Ctrl + Z, RigController::undo): the preset that was current when
  // the audition started and the applied candidate as loaded. takeUndoStep() hands it out once; it is dropped (nullopt) when a
  // user preset load happened since the apply (`userLoadSerial` is the processor's current serial). Auditions and A/B leave none.
  struct UndoStep {
    Preset pre, post;
    std::uint64_t loadSerial = 0;
  };
  std::optional<UndoStep> takeUndoStep(std::uint64_t userLoadSerial);
  // The candidate file whose preset is the current one (auditioned-on-B or applied), for the exporter.
  std::optional<std::filesystem::path> currentCandidateFile() const;
  // The same, but only for a candidate that was applied (not one that is merely being auditioned).
  std::optional<std::filesystem::path> appliedCandidateFile() const;

 private:
  struct CacheKey {
    std::filesystem::path file;
    std::filesystem::file_time_type mtime{};
    std::uint64_t builds = 0;
    std::string presetName;
    std::vector<float> params;
    bool operator==(const CacheKey&) const = default;
  };
  std::optional<std::filesystem::path> stillCurrent(const std::filesystem::path& file) const;
  SawbladeProcessor& proc_;
  mutable std::optional<CacheKey> cacheKey_;
  mutable bool cacheResult_ = false;
  State state_;
  Preset original_;
  std::optional<UndoStep> undoStep_;
  std::filesystem::path applied_;
  std::string appliedName_;
};

}  // namespace sawblade::plugin
