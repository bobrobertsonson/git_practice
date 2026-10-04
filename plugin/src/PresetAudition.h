#pragma once

// Audition / A-B / apply of a match candidate (docs/PLUGIN.md "Record + Match"). Message thread only.
//
// Auditioning loads the candidate's resolved preset through the processor's normal off-thread load path
// (loadPreset: the EngineLoader builds it, the audio thread cross-fades to it). The preset that was current
// when the audition started is kept, so A (the original) and B (the candidate) can be toggled; auditioning
// another candidate keeps A. Apply makes the candidate the current preset (it is what a normal preset load
// would leave), Revert goes back to A.

#include <filesystem>
#include <optional>
#include <string>

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
  bool apply();      // false when no audition is active; leaves the candidate loaded and ends the audition
  bool revert();     // back to A and ends the audition
  State state() const { return state_; }
  // The candidate file whose preset is the current one (auditioned-on-B or applied), for the exporter.
  std::optional<std::filesystem::path> currentCandidateFile() const;

 private:
  SawbladeProcessor& proc_;
  State state_;
  Preset original_;
  std::filesystem::path applied_;
  std::string appliedName_;
};

}  // namespace sawblade::plugin
