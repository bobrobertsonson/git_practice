#include "PresetAudition.h"

#include "PluginProcessor.h"

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

std::optional<fs::path> PresetAudition::currentCandidateFile() const {
  const std::string current = proc_.status().presetName;
  if (state_.active && state_.onCandidate) return state_.candidate;
  if (!applied_.empty() && current == appliedName_) return applied_;
  return std::nullopt;
}

}  // namespace sawblade::plugin
