#pragma once

// Message-thread facade between the rig editor and the SawbladeProcessor (spec phase 10, section 5).
// No GUI classes; it only uses juce::Timer for the debounce and the LEARN wait.
//
//   structural edits  edit() / editDebounced(): applied to processor.editBasePreset() and handed to
//                     processor.loadPreset() (loader thread build, 30 ms cross-fade). The editor's
//                     knobs call edit() on drag end and editDebounced() for wheel / typed values.
//   live edits        live(): processor.applyLiveEdit(), no rebuild (EQ freq / Q, block gains).
//   parameters        beginParam/setParam/endParam: APVTS parameters with host gestures.
//   monitoring        setMute / setSolo -> processor.setMonitor().
//   alignment         remeasure() -> processor.remeasureAlignment().
//   gate LEARN        learnGate() waits 1 s, reads the InputMeter, sets the gateThreshold parameter.
//   transient state   the remembered blend and the SINGLE + 2 PEDALS choice (never saved).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <juce_events/juce_events.h>

#include "PluginProcessor.h"
#include "rig/BodyFill.h"
#include "rig/RigModel.h"

namespace sawblade::plugin::rig {

class RigController {
 public:
  using EditFn = std::function<void(Preset&)>;
  static constexpr int kDebounceMs = 150;
  static constexpr int kLearnMs = 1000;

  explicit RigController(SawbladeProcessor& p);
  ~RigController();
  RigController(const RigController&) = delete;
  RigController& operator=(const RigController&) = delete;

  SawbladeProcessor& processor() noexcept { return proc_; }

  // The preset the editor shows: the edit base with the debounced edits that are still pending.
  Preset view() const;

  // --- structural -----------------------------------------------------------------------------------
  void edit(const EditFn& f);           // flushes pending edits, applies f, submits one rebuild
  void editDebounced(const EditFn& f);  // latest wins: submitted kDebounceMs after the last call
  // Throttle for a drag: keeps only the LATEST closure per `key` (one entry per knob, no growth per mouse move) and starts the
  // timer only when it is not running, so a flush (one background rebuild) happens at most every kDebounceMs during motion.
  // (editThrottled and editDebounced share one timer: a throttled flush also applies a pending debounced edit early; intended.)
  void editThrottled(const void* key, const EditFn& f);
  void flushPending();
  // Test hooks: fire the debounce / throttle timer's callback now if it is running (what the timer does after kDebounceMs), so
  // tests need no wall-clock waits; and the number of times that timer was started.
  bool flushTimerForTests();
  int timerStartsForTests() const noexcept { return timerStarts_; }
  bool hasPending() const noexcept { return !pending_.empty(); }

  // --- live -----------------------------------------------------------------------------------------
  void live(const EditFn& f);
  // One EQ band edit (drag): freq / Q (and gain) live; a post band with a parameter slot takes its
  // gain from the parameter instead (begin/endParam around the drag are the caller's).
  void eqLive(EqTarget t, int band, double freq, double gainDb, double q);
  // Post EQ band -> parameter slot (0..5), -1 when the band has no slot.
  int postSlotOfBand(int band) const;

  // --- parameters (host automatable) ----------------------------------------------------------------
  void beginParam(int paramIndex);
  void setParam(int paramIndex, double value);
  void endParam(int paramIndex);

  // --- topology (5.5) -------------------------------------------------------------------------------
  Topology topology();
  // -> Blend on a path B with no blocks fills it (v0.2 Task C): TS boost + the fallback amp (if cached), level match
  // auto; the suggestion of `sawblade-t3k suggest-body` follows asynchronously (BodyFill) and swaps the amp in if path B is
  // untouched. The whole thing is one undo step.
  void setTopology(Topology t);
  // v0.3 Task C: the BLEND knob was turned up from full SAW by a user gesture (never host automation or a state restore: the editor
  // calls this from the knob's drag end). Enables the blend topology through the same code path as setTopology(Blend), so it fills
  // path B and is one undo step (restoring the knob to `blendBefore`). When path B is a blend that has no amp because the last
  // fill failed, it retries the fill. Moving BLEND back to 0 never touches path B. True when it acted.
  bool blendTurnedUp(double blendBefore);
  // --- undo / redo (v0.3 Task D) --------------------------------------------------------------------------------------------------------
  // The history itself lives in the processor (SawbladeProcessor "undo / redo": 64 whole-preset steps that survive a closed editor);
  // this is its face for the rig editor. Every edit this controller applies is one step: edit() / editDebounced() flush = one step
  // (a debounced burst of wheel notches is one flush), live() = one step, a drag = ONE step however many flushes / live edits it makes
  // (beginGesture() .. endGesture(), which PresetKnob and the EQ graph call around a mouse drag), applyTopology = one step with its
  // BLEND fill. Asynchronous completions (the fill's amp arriving, a trim write-back) are never steps.
  // A BLEND fill still in flight follows the rig: an undo / redo that leaves path B without blocks cancels it (the late answer is dropped
  // either way: BodyFill only swaps an amp into a path B that is still what the fill made); a redo into a blend path B that never got its
  // amp starts the fill again.
  bool canUndo() const noexcept { return proc_.canUndo(); }
  bool canRedo() const noexcept { return proc_.canRedo(); }
  bool undo();
  bool redo();
  // One undo step around a drag: nested calls are one gesture. The controller closes the ones it still has open when it is destroyed.
  void beginGesture();
  void endGesture();
  BodyFill& bodyFill() noexcept { return body_; }

  // --- monitoring (5.3) -----------------------------------------------------------------------------
  void setMute(int path, bool on);  // path 0 = A, 1 = B
  void setSolo(int path, bool on);
  bool muted(int path);
  bool solo(int path);

  // --- align / LEARN --------------------------------------------------------------------------------
  void remeasure();
  // Phase 10.1: MATCH LEVELS measures the path trims (levelMatch -> manual with the measured values);
  // the blend law is a live edit (no rebuild).
  void matchLevels();
  void setBlendLaw(BlendLaw law);
  // Alignment edits that switch Auto to Manual seed it with the measured values.
  void nudgeAlign(int samples);
  void setInvertB(bool invert);
  void learnGate();
  void finishLearn();  // normally the 1 s timer; public as a test hook
  bool learning() const noexcept { return learning_; }
  const std::string& learnStatus() const noexcept { return learnStatus_; }

  // Call from the editor's refresh tick: re-syncs the transient state after loads and restores.
  void sync();

 private:
  struct Timer : juce::Timer {
    std::function<void()> fn;
    void timerCallback() override {
      stopTimer();
      if (fn) fn();
    }
  };
  void applyMonitor();
  void applyTopology(Topology t, const std::optional<Preset>& preBlend);
  bool stepHistory(bool undo);
  void reconcileFill(const PathPreset& bBefore);
  void liveRecorded(const EditFn& f, const std::function<void()>& beforeEdit = {});
  void resetTransient();
  AlignResult measuredAlign() const;

  int timerStarts_ = 0;
  SawbladeProcessor& proc_;
  BodyFill body_;
  int gestures_ = 0;  // beginGesture() calls not yet ended
  struct Pending {
    const void* key;  // non-null: a throttled edit, replaced in place by the next one with the same key
    EditFn fn;
  };
  std::vector<Pending> pending_;
  Timer debounce_, learnTimer_;

  double lastBlend_ = 0.5;
  bool singlePlus_ = false;
  bool mute_[2] = {false, false}, solo_[2] = {false, false};
  SawbladeProcessor::Monitor applied_;
  std::uint64_t loadSerial_ = 0;

  bool learning_ = false;
  std::uint32_t learnStart_ = 0;
  std::string learnStatus_;
};

}  // namespace sawblade::plugin::rig
