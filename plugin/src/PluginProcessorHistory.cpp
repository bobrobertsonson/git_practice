// v0.3 Task D: undo / redo of rig edits (see the "undo / redo" block in PluginProcessor.h). Message-thread code: nothing here runs
// on the audio thread, and nothing here allocates on it.
#include "PluginProcessor.h"

#include <utility>

#include "PresetMapping.h"

namespace sawblade::plugin {

void SawbladeProcessor::HistoryParamListener::parameterGestureChanged(int index, bool starting) {
  // A user's gesture on a host parameter (SliderAttachment / ParameterAttachment, the rig controller's beginParam). Host automation
  // never starts one. Off the message thread (never the audio thread in practice) it is ignored: the history is message-thread data.
  if (!juce::MessageManager::existsAndIsCurrentThread()) return;
  if (index < 0 || index >= static_cast<int>(p_.paramGestureToken_.size())) return;
  auto& token = p_.paramGestureToken_[static_cast<std::size_t>(index)];
  if (starting) token = p_.historyGestureBegin();
  else p_.historyGestureEnd(token);
}

bool SawbladeProcessor::canUndo() const {
  std::lock_guard<std::mutex> lk(historyMutex_);
  return history_.canUndo();
}
bool SawbladeProcessor::canRedo() const {
  std::lock_guard<std::mutex> lk(historyMutex_);
  return history_.canRedo();
}
std::size_t SawbladeProcessor::undoSteps() const {
  std::lock_guard<std::mutex> lk(historyMutex_);
  return history_.undoCount();
}
std::size_t SawbladeProcessor::redoSteps() const {
  std::lock_guard<std::mutex> lk(historyMutex_);
  return history_.redoCount();
}
bool SawbladeProcessor::historyInGesture() const {
  std::lock_guard<std::mutex> lk(historyMutex_);
  return history_.inGesture();
}
void SawbladeProcessor::historyClear() {
  std::lock_guard<std::mutex> lk(historyMutex_);
  history_.clear();
}
void SawbladeProcessor::setHistoryFlusher(std::function<void()> f, const void* owner) {
  std::lock_guard<std::mutex> lk(historyMutex_);
  historyFlusher_ = std::move(f);
  historyFlusherOwner_ = owner;
}
void SawbladeProcessor::clearHistoryFlusher(const void* owner) {
  std::lock_guard<std::mutex> lk(historyMutex_);
  if (historyFlusherOwner_ != owner) return;  // a newer controller took over: its flusher stays
  historyFlusher_ = {};
  historyFlusherOwner_ = nullptr;
}
void SawbladeProcessor::patchHistory(const std::function<void(Preset&)>& f) {
  std::lock_guard<std::mutex> lk(historyMutex_);
  history_.patchAll(f);
}

void SawbladeProcessor::historyRecord(Preset before, const Preset& after, HistoryKind kind) {
  const EditHistory::ParamMask mask = EditHistory::changedParams(before, after);
  std::lock_guard<std::mutex> lk(historyMutex_);
  history_.record(std::move(before), kind, mask);
}

SawbladeProcessor::GestureToken SawbladeProcessor::historyGestureBegin() {
  std::function<void()> flush;
  {
    std::lock_guard<std::mutex> lk(historyMutex_);
    if (!history_.inGesture()) flush = historyFlusher_;
  }
  if (flush) flush();  // an edit still waiting for its debounce is a step of its own, not part of this gesture
  return historyGestureBegin(editBasePreset());
}

SawbladeProcessor::GestureToken SawbladeProcessor::historyGestureBegin(Preset before) {
  std::lock_guard<std::mutex> lk(historyMutex_);
  return history_.beginGesture(std::move(before));
}

void SawbladeProcessor::historyGestureEnd(GestureToken token) {
  std::optional<Preset> before;
  {
    std::lock_guard<std::mutex> lk(historyMutex_);
    before = history_.endGesture(token);
  }
  if (before) finishGesture(std::move(*before));
}

void SawbladeProcessor::historyAbortGestures() {
  std::optional<Preset> before;
  {
    std::lock_guard<std::mutex> lk(historyMutex_);
    before = history_.abortGestures();
  }
  if (before) finishGesture(std::move(*before));
}

// The outermost gesture closed: one step if the rig is different from what it started as.
void SawbladeProcessor::finishGesture(Preset before) {
  const Preset now = editBasePreset();
  if (now != before) historyRecord(std::move(before), now, HistoryKind::Edit);
}

void SawbladeProcessor::loadPresetUndoable(Preset preset, HistoryKind kind, bool keepMonitor, std::optional<double> provisionalTrimDb) {
  if (!historyInGesture()) {  // inside a gesture the gesture's end decides
    Preset before = editBasePreset();
    const Preset after = clampedToParams(preset);
    if (before != after) historyRecord(std::move(before), after, kind);
  }
  loadPreset(std::move(preset), keepMonitor, provisionalTrimDb);
}

bool SawbladeProcessor::undo() { return stepHistory(true); }
bool SawbladeProcessor::redo() { return stepHistory(false); }

// Restores the stored preset and keeps the one it replaces on the opposite stack. The restore is a load of the whole preset (the engine
// is rebuilt and cross-faded in); the level trim never drops to 0 on the way: an Edit step is a keepMonitor load (the running trim
// stays until the restored rig's own trim is known: the stored stamp if it is fresh, else a measurement), a Load step starts from the
// running trim as its provisional value (levelOnLoad still prefers a known / fresh one).
bool SawbladeProcessor::stepHistory(bool undo) {
  std::function<void()> flush;
  {
    std::lock_guard<std::mutex> lk(historyMutex_);
    if (history_.inGesture()) return false;  // mid-drag: the drag's own step is not complete yet
    flush = historyFlusher_;
  }
  if (flush) flush();
  std::optional<EditHistory::Step> step;
  const Preset now = editBasePreset();
  {
    std::lock_guard<std::mutex> lk(historyMutex_);
    step = undo ? history_.popUndo(now) : history_.popRedo(now);
  }
  if (!step) return false;
  // Only the parameters the step changed come back from the snapshot; the others keep their current value (host automation moves on).
  const ParamValues snap = paramsFromPreset(step->preset), cur = paramsFromPreset(now);
  ParamValues mixed = cur;
  for (std::size_t i = 0; i < mixed.size(); ++i)
    if (step->params[i]) mixed[i] = snap[i];
  applyParams(step->preset, mixed);
  // gainStep (the active gain-ladder rung) is preset state that follows the GAIN parameter (the ladder write-back records it). A path whose
  // GAIN is not restored from the snapshot keeps the current gainStep (and ladder), as long as the amp is the same capture: otherwise the rung
  // would name a model the (host-automated) GAIN is no longer on.
  for (int path = 0; path < 2; ++path) {
    if (step->params[static_cast<std::size_t>(ampParam(path, kAmpGain))]) continue;
    PathPreset& dst = path == 0 ? step->preset.a : step->preset.b;
    const PathPreset& cur = path == 0 ? now.a : now.b;
    const int di = ampIndex(dst), ci = ampIndex(cur);
    if (di < 0 || ci < 0) continue;
    const auto* dn = dynamic_cast<const NamBlockParams*>(dst.blocks[static_cast<std::size_t>(di)].params.get());
    const auto* cn = dynamic_cast<const NamBlockParams*>(cur.blocks[static_cast<std::size_t>(ci)].params.get());
    if (!dn || !cn || !dn->model.source || !cn->model.source || dn->model.source->id != cn->model.source->id) continue;
    dst.ampControls.gainStep = cur.ampControls.gainStep;
    if (!cn->model.ladder.empty() && dn->model.ladder != cn->model.ladder) {
      auto copy = std::make_shared<NamBlockParams>(*dn);
      copy->model.ladder = cn->model.ladder;
      dst.blocks[static_cast<std::size_t>(di)].params = std::move(copy);
    }
  }
  const Status st = status();
  const bool load = step->kind == HistoryKind::Load;
  loadPreset(std::move(step->preset), /*keepMonitor=*/!load, st.levelMatchOn ? std::optional<double>(st.trimDb) : std::nullopt);
  return true;
}

}  // namespace sawblade::plugin
