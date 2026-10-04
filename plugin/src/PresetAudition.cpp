#include "PresetAudition.h"

#include "PluginProcessor.h"
#include "PresetMapping.h"

#include "sawblade/preset.h"

namespace sawblade::plugin {
namespace fs = std::filesystem;

bool PresetAudition::audition(const fs::path& file, std::string* error) {
  Preset candidate;
  try {
    candidate = sawblade::loadPresetFile(file);
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
  if (!state_.active) {
    original_ = proc_.currentPreset();
    state_.originalName = original_.name;
    state_.active = true;
  }
  state_.candidate = file;
  state_.candidateName = candidate.name;
  state_.onCandidate = true;
  proc_.loadPreset(std::move(candidate));
  return true;
}

bool PresetAudition::toggleAB() {
  if (!state_.active) return false;
  if (state_.onCandidate) {
    proc_.loadPreset(original_);
    state_.onCandidate = false;
    return true;
  }
  std::string err;
  if (!proc_.loadPresetFile(state_.candidate, &err)) return false;
  state_.onCandidate = true;
  return true;
}

bool PresetAudition::apply() {
  if (!state_.active) return false;
  if (!state_.onCandidate) {
    std::string err;
    if (!proc_.loadPresetFile(state_.candidate, &err)) return false;
  }
  applied_ = state_.candidate;
  appliedName_ = state_.candidateName;
  state_ = State{};
  return true;
}

bool PresetAudition::revert() {
  if (!state_.active) return false;
  if (state_.onCandidate) proc_.loadPreset(original_);
  state_ = State{};
  return true;
}

// The candidate's file counts only while the current preset IS that candidate: any later preset load or parameter
// change makes the saved state differ from the candidate as it would load, and then the exporter must take the
// current state instead. (Compared as state JSON; while a load is still in flight the association is kept.)
std::optional<fs::path> PresetAudition::currentCandidateFile() const {
  fs::path file;
  if (state_.active && state_.onCandidate) file = state_.candidate;
  else if (!applied_.empty()) file = applied_;
  if (file.empty()) return std::nullopt;
  if (proc_.status().loading) return file;
  try {
    const std::string wanted = presetToStateJson(clampedToParams(sawblade::loadPresetFile(file)));
    if (wanted == presetToStateJson(proc_.currentPreset())) return file;
  } catch (...) {
  }
  return std::nullopt;
}

}  // namespace sawblade::plugin
