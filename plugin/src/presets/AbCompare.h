#pragma once

#include <optional>

#include "../PluginProcessor.h"

// A/B compare (docs/specs/phase9b_preset_browser.md section 4): two slots, each a complete Preset (with its parameter values).
// A is active at start. Switching stores processor.currentPreset() into the active slot, makes the other slot active (an empty
// one first becomes a copy of the stored preset, so B starts identical) and loads it through the normal loader (one engine
// build, 30 ms swap). A preset loaded from the browser or by the stepping buttons simply becomes the active slot's content
// (it is what currentPreset() returns at the next switch). Not saved in the plugin state. Message thread only.
namespace sawblade::plugin {

class AbCompare {
 public:
  explicit AbCompare(SawbladeProcessor& p) : proc_(p) {}

  int active() const { return active_; }                 // 0 = A, 1 = B
  const char* label() const { return active_ == 0 ? "A" : "B"; }
  bool hasOther() const { return slot_[1 - active_].has_value(); }
  // The stored slot (the active slot's content is stale until the next switch: it is whatever was stored last).
  const std::optional<Preset>& slot(int i) const { return slot_[i]; }

  void toggle();
  void copyAToB() { copy(0, 1); }
  void copyBToA() { copy(1, 0); }
  void reset();  // slots cleared; the current preset stays and A is active

 private:
  void storeActive();
  void copy(int from, int to);
  SawbladeProcessor& proc_;
  std::optional<Preset> slot_[2];
  int active_ = 0;
};

}  // namespace sawblade::plugin
