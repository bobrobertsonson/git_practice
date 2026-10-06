#include "rig/RigController.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sawblade::plugin::rig {

RigController::RigController(SawbladeProcessor& p) : proc_(p), body_(p), loadSerial_(p.userLoadSerial()) {
  body_.onBodyChanged = [this](const Preset& swapped) {  // the async amp swap: the fill's result moves with it
    if (undo_) undo_->post = swapped;
  };
  debounce_.fn = [this] { flushPending(); };
  learnTimer_.fn = [this] { finishLearn(); };
  lastBlend_ = [&] {
    const double b = proc_.editBasePreset().blend;
    return b > 0.0 ? b : 0.5;
  }();
}

RigController::~RigController() {
  debounce_.stopTimer();
  learnTimer_.stopTimer();
}

Preset RigController::view() const {
  Preset p = proc_.editBasePreset();
  for (const auto& e : pending_) e.fn(p);
  return p;
}

void RigController::edit(const EditFn& f) {
  pending_.push_back({nullptr, f});
  flushPending();
}

void RigController::editDebounced(const EditFn& f) {
  pending_.push_back({nullptr, f});
  ++timerStarts_;
  debounce_.startTimer(kDebounceMs);
}

void RigController::editThrottled(const void* key, const EditFn& f) {
  auto it = std::find_if(pending_.begin(), pending_.end(), [key](const Pending& e) { return e.key == key; });
  if (it != pending_.end()) it->fn = f;
  else pending_.push_back({key, f});
  if (!debounce_.isTimerRunning()) {
    ++timerStarts_;
    debounce_.startTimer(kDebounceMs);
  }
}

bool RigController::flushTimerForTests() {
  if (!debounce_.isTimerRunning()) return false;
  debounce_.timerCallback();
  return true;
}

void RigController::flushPending() {
  debounce_.stopTimer();
  if (pending_.empty()) return;
  Preset p = proc_.editBasePreset();
  const PathPreset bBefore = p.b;
  for (const auto& e : pending_) e.fn(p);
  pending_.clear();
  if (undo_ && p.b != bBefore) undo_.reset();  // path B was edited (blocks, BLEND on / off, ...): the fill is no longer the last word
  proc_.loadPreset(std::move(p), /*keepMonitor=*/true);
}

void RigController::live(const EditFn& f) { proc_.applyLiveEdit(f); }

int RigController::postSlotOfBand(int band) const {
  const SlotBands s = proc_.postEqSlots();
  for (int k = 0; k < kPostEqSlots; ++k)
    if (s[static_cast<std::size_t>(k)] == band) return k;
  return -1;
}

void RigController::eqLive(EqTarget t, int band, double freq, double gainDb, double q) {
  if (t == EqTarget::Post) {
    const int slot = postSlotOfBand(band);
    if (slot >= 0) {
      // The gain belongs to the host-visible parameter; the preset keeps its own value untouched.
      setParam(kPostEqFirst + slot, gainDb);
      proc_.applyLiveEdit([=](Preset& p) {
        if (band < 0 || band >= static_cast<int>(p.postEq.size())) return;
        setBandLive(p, t, band, freq, p.postEq[static_cast<std::size_t>(band)].gainDb, q);
      });
      return;
    }
  }
  proc_.applyLiveEdit([=](Preset& p) { setBandLive(p, t, band, freq, gainDb, q); });
}

void RigController::beginParam(int i) {
  if (auto* prm = proc_.parameters().getParameter(paramSpec(i).id)) prm->beginChangeGesture();
}

void RigController::setParam(int i, double v) {
  if (auto* prm = proc_.parameters().getParameter(paramSpec(i).id))
    prm->setValueNotifyingHost(prm->convertTo0to1(static_cast<float>(snapParam(std::clamp(v, paramSpec(i).min, paramSpec(i).max)))));
}

void RigController::endParam(int i) {
  if (auto* prm = proc_.parameters().getParameter(paramSpec(i).id)) prm->endChangeGesture();
}

// --- transient state -------------------------------------------------------------------------------
void RigController::resetTransient() {
  singlePlus_ = false;
  mute_[0] = mute_[1] = solo_[0] = solo_[1] = false;
  applied_ = proc_.monitor();
  const double b = proc_.editBasePreset().blend;
  lastBlend_ = b > 0.0 ? b : 0.5;
}

void RigController::sync() {
  if (const auto serial = proc_.userLoadSerial(); serial != loadSerial_) {
    loadSerial_ = serial;
    resetTransient();
    undo_.reset();  // another preset: the BLEND fill can no longer be undone, and its suggestion no longer applies
    body_.cancel();
  }
  body_.tick();
  if (!pending_.empty()) return;
  const Preset p = proc_.editBasePreset();
  if (p.b.enabled && p.blend > 0.0) lastBlend_ = p.blend;
  const auto m = proc_.monitor();
  if (m.muteA != applied_.muteA || m.muteB != applied_.muteB) {  // cleared behind our back
    applied_ = m;
    mute_[0] = m.muteA;
    mute_[1] = m.muteB;
    solo_[0] = solo_[1] = false;
  }
}

Topology RigController::topology() {
  sync();
  const Topology t = topologyOf(view());
  return (t == Topology::Single && singlePlus_) ? Topology::SinglePlusTwoPedals : t;
}

void RigController::setTopology(Topology t) { applyTopology(t, std::nullopt); }

// The BLEND knob was turned off full SAW by a user gesture (the editor's drag end): the same switch as the rig section's BLEND, with
// the same fill and the same single undo step. `blendBefore` is the knob's value when the gesture started (0): the preset an undo
// restores has the knob back there. When path B is already a blend whose body amp is missing and the last fill failed, the gesture
// retries the fill instead. Returns true when it did something.
bool RigController::blendTurnedUp(double blendBefore) {
  sync();
  const Preset cur = view();
  if (topologyOf(cur) == Topology::Blend) {
    if (ampIndex(cur.b) < 0 && body_.status().kind == FillStatus::Kind::Failed) {
      body_.retry();
      return true;
    }
    return false;
  }
  Preset pre = cur;
  pre.blend = blendBefore;
  applyTopology(Topology::Blend, pre);
  return true;
}

void RigController::applyTopology(Topology t, const std::optional<Preset>& preBlend) {
  sync();
  const Preset cur = view();
  if (topologyOf(cur) == Topology::Blend && cur.blend > 0.0) lastBlend_ = cur.blend;
  singlePlus_ = (t == Topology::SinglePlusTwoPedals);
  const double restore = lastBlend_;
  const bool fill = t == Topology::Blend && !cur.b.enabled && cur.b.blocks.empty();
  if (!fill) {
    edit([t, restore](Preset& p) { rig::setTopology(p, t, restore); });
    if (t == Topology::Blend && !cur.b.enabled && ampIndex(cur.b) < 0) {  // path B kept blocks but has no amp: never a silent boost-only path
      const Preset post = proc_.editBasePreset();
      undo_ = UndoEntry{preBlend.value_or(cur), post};
      body_.begin(post);
    }
    return;
  }
  const std::optional<Capture> amp = cachedToneCapture(kFallbackBodyTone);
  edit([t, restore, amp](Preset& p) {
    rig::setTopology(p, t, restore);
    fillBodyPath(p, amp);
  });
  const Preset post = proc_.editBasePreset();
  undo_ = UndoEntry{preBlend.value_or(cur), post};  // replaces any older entry: a second fill never resurrects the first one's snapshot
  body_.begin(post);
}

bool RigController::undo() {
  sync();
  if (!undo_) return false;
  flushPending();  // an edit still waiting for its debounce counts as an edit (and may drop the entry)
  if (!undo_) return false;
  UndoEntry e = std::move(*undo_);
  undo_.reset();
  body_.cancel();
  if (proc_.editBasePreset() != e.post) return false;  // something else changed since the fill: undoing would lose it
  proc_.loadPreset(std::move(e.pre), /*keepMonitor=*/true);
  return true;
}

void RigController::applyMonitor() {
  const bool a = mute_[0] || (solo_[1] && !solo_[0]);
  const bool b = mute_[1] || (solo_[0] && !solo_[1]);
  applied_ = {a, b};
  proc_.setMonitor(a, b);
}

void RigController::setMute(int path, bool on) {
  sync();
  mute_[path & 1] = on;
  applyMonitor();
}

void RigController::setSolo(int path, bool on) {
  sync();
  solo_[path & 1] = on;
  applyMonitor();
}

bool RigController::muted(int path) {
  sync();
  return mute_[path & 1];
}

bool RigController::solo(int path) {
  sync();
  return solo_[path & 1];
}

// --- alignment / LEARN -----------------------------------------------------------------------------
AlignResult RigController::measuredAlign() const { return proc_.status().info.align; }

void RigController::remeasure() { proc_.remeasureAlignment(); }

void RigController::matchLevels() { proc_.matchLevels(); }

void RigController::setBlendLaw(BlendLaw law) {
  // Live when the running engine has a measured make-up curve (or for LINEAR, which needs none); a legacy
  // preset toggled to CONSTANT is a structural edit, so the rebuild probes.
  if (law == BlendLaw::ConstantLoudness && !proc_.status().info.levelMeasured)
    edit([law](Preset& p) { p.blendLaw = law; });
  else
    live([law](Preset& p) { p.blendLaw = law; });
}

void RigController::nudgeAlign(int samples) {
  const AlignResult m = measuredAlign();
  edit([=](Preset& p) { rig::nudgeAlign(p, samples, m); });
}

void RigController::setInvertB(bool invert) {
  const AlignResult m = measuredAlign();
  edit([=](Preset& p) { rig::setInvertB(p, invert, m); });
}

void RigController::learnGate() {
  if (learning_) return;
  learning_ = true;
  learnStart_ = proc_.inputMeter().counter();
  learnStatus_ = "LEARN: don't play for 1 s";
  learnTimer_.startTimer(kLearnMs);
}

void RigController::finishLearn() {
  learnTimer_.stopTimer();
  if (!learning_) return;
  learning_ = false;
  const std::vector<float> peaks = proc_.inputMeter().since(learnStart_);
  if (peaks.empty()) {
    learnStatus_ = "no input";
    return;
  }
  const double peak = *std::max_element(peaks.begin(), peaks.end());
  const double db = peak > 0.0 ? 20.0 * std::log10(peak) : -200.0;
  // The gate sits after the input gain (DI -> input gain -> gate), the meter before it.
  const double inGain = proc_.parameters().getRawParameterValue(paramSpec(kInputGain).id)->load();
  const double thr = std::clamp(db + inGain + 6.0, -80.0, -20.0);
  beginParam(kGateThreshold);
  setParam(kGateThreshold, thr);
  endParam(kGateThreshold);
  char buf[64];
  std::snprintf(buf, sizeof buf, "thr set %.1f dB", snapParam(thr));
  learnStatus_ = buf;
}

}  // namespace sawblade::plugin::rig
