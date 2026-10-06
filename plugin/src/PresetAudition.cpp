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
  // Applying is the one point where the audition becomes the instance's saved state: the whole preset was swapped by a
  // single load (every parameter notified the host as a preset load does), and the non-parameter part of the state (the
  // blocks, captures, cab) changed too, so the host is told its project needs saving. (Auditioning and A/B are previews
  // and do not set this.)
  proc_.updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
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
  return stillCurrent(file);
}

std::optional<fs::path> PresetAudition::appliedCandidateFile() const {
  if (applied_.empty()) return std::nullopt;
  return stillCurrent(applied_);
}

std::optional<fs::path> PresetAudition::stillCurrent(const fs::path& file) const {
  const auto st = proc_.status();
  if (st.loading) return file;
  // The comparison parses the candidate and serialises two presets: it is redone only when something it depends on
  // changed (the file, the loaded engine, the preset name or a parameter value), not on every UI refresh.
  CacheKey key;
  key.file = file;
  std::error_code ec;
  key.mtime = fs::last_write_time(file, ec);
  key.builds = proc_.engineBuilds();
  key.presetName = st.presetName;
  for (int i = 0; i < kNumParams; ++i) key.params.push_back(proc_.parameters().getRawParameterValue(paramSpec(i).id)->load());
  if (cacheKey_ && *cacheKey_ == key) return cacheResult_ ? std::optional<fs::path>(file) : std::nullopt;
  bool same = false;
  try {
    same = presetToStateJson(clampedToParams(sawblade::loadPresetFile(file))) == presetToStateJson(proc_.currentPreset());
  } catch (...) {
  }
  cacheKey_ = std::move(key);
  cacheResult_ = same;
  return same ? std::optional<fs::path>(file) : std::nullopt;
}

}  // namespace sawblade::plugin
