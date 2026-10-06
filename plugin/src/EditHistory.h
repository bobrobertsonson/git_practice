#pragma once

// v0.3 Task D: the undo / redo history of rig edits. Pure data (no JUCE): whole-preset snapshots, 64 steps, message-thread use.
//
//   step      the preset as it was BEFORE one edit (undo restores it), how to restore it (see Kind), and which host parameters the step
//             changed: undo / redo put back only THOSE parameters, so a parameter the host automated meanwhile is never rewritten.
//   undo      pops a step, pushes the current preset onto the redo stack and returns the popped one.
//   redo      the mirror image.
//   record    pushes a step and clears the redo stack (a new edit after an undo ends the redo branch); a 65th step drops the oldest.
//   gesture   a drag (mouse down .. mouse up) is ONE step however many edits it submits: beginGesture() stores the preset the drag
//             started from, endGesture() hands it back when the outermost gesture closes, and the owner records it if the rig is
//             different by then. Gestures nest (a BLEND-knob drag holds one open across the fill that follows its mouse-up).
//   patch     an asynchronous completion that belongs to the rig as such (a fetched gain ladder, the BLEND fill's amp arriving) is
//             applied to the stored snapshots too, so undoing / redoing never takes it away again; it adds no step.
//
// Not persisted (the plugin state is the preset); it lives in the processor so a closed and reopened editor keeps it.

#include <bitset>
#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "PresetMapping.h"
#include "sawblade/preset.h"

namespace sawblade::plugin {

class EditHistory {
 public:
  static constexpr std::size_t kMaxSteps = 64;
  // Edit: restored like a rig edit (the transient mute / solo state stays). Load: restored like a preset load (it resets them).
  enum class Kind { Edit, Load };
  using ParamMask = std::bitset<kNumParams>;
  struct Step {
    Preset preset;
    Kind kind = Kind::Edit;
    ParamMask params;  // the parameters the step changed (set = restore from `preset`, clear = keep the current value)
  };

  // The parameters whose value differs between two presets.
  static ParamMask changedParams(const Preset& a, const Preset& b) {
    const ParamValues pa = paramsFromPreset(a), pb = paramsFromPreset(b);
    ParamMask m;
    for (std::size_t i = 0; i < m.size(); ++i) m[i] = pa[i] != pb[i];
    return m;
  }

  void record(Preset before, Kind kind, const ParamMask& params) {
    redo_.clear();
    undo_.push_back({std::move(before), kind, params});
    if (undo_.size() > kMaxSteps) undo_.erase(undo_.begin());
  }
  bool canUndo() const noexcept { return !undo_.empty(); }
  bool canRedo() const noexcept { return !redo_.empty(); }
  std::size_t undoCount() const noexcept { return undo_.size(); }
  std::size_t redoCount() const noexcept { return redo_.size(); }

  // `current` is the preset as it is now: it goes onto the opposite stack.
  std::optional<Step> popUndo(Preset current) {
    if (undo_.empty()) return std::nullopt;
    Step s = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({std::move(current), s.kind, s.params});
    return s;
  }
  std::optional<Step> popRedo(Preset current) {
    if (redo_.empty()) return std::nullopt;
    Step s = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({std::move(current), s.kind, s.params});
    if (undo_.size() > kMaxSteps) undo_.erase(undo_.begin());
    return s;
  }
  void clear() {
    undo_.clear();
    redo_.clear();
    depth_ = 0;
    gestureBefore_.reset();
  }

  bool inGesture() const noexcept { return depth_ > 0; }
  void beginGesture(Preset before) {
    if (depth_++ == 0) gestureBefore_ = std::move(before);
  }
  // The preset the outermost gesture started from, when this call closes it; nullopt while an outer gesture is still open (or none).
  std::optional<Preset> endGesture() {
    if (depth_ <= 0) return std::nullopt;
    if (--depth_ > 0) return std::nullopt;
    std::optional<Preset> b = std::move(gestureBefore_);
    gestureBefore_.reset();
    return b;
  }
  // Closes every open gesture at once (the editor was destroyed mid-drag).
  std::optional<Preset> abortGestures() {
    depth_ = 0;
    std::optional<Preset> b = std::move(gestureBefore_);
    gestureBefore_.reset();
    return b;
  }

  void patchAll(const std::function<void(Preset&)>& f) {
    for (Step& s : undo_) f(s.preset);
    for (Step& s : redo_) f(s.preset);
    if (gestureBefore_) f(*gestureBefore_);
  }

 private:
  std::vector<Step> undo_, redo_;
  int depth_ = 0;
  std::optional<Preset> gestureBefore_;
};

}  // namespace sawblade::plugin
