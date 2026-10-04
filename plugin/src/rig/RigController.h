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
#include <string>
#include <vector>

#include <juce_events/juce_events.h>

#include "PluginProcessor.h"
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
  void flushPending();
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
  void setTopology(Topology t);

  // --- monitoring (5.3) -----------------------------------------------------------------------------
  void setMute(int path, bool on);  // path 0 = A, 1 = B
  void setSolo(int path, bool on);
  bool muted(int path);
  bool solo(int path);

  // --- align / LEARN --------------------------------------------------------------------------------
  void remeasure();
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
  void resetTransient();
  AlignResult measuredAlign() const;

  SawbladeProcessor& proc_;
  std::vector<EditFn> pending_;
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
