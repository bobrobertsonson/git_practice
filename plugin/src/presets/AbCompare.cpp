#include "AbCompare.h"

#include <chrono>

namespace sawblade::plugin {

void AbCompare::storeActive() {
  // A load that is still building would store the old preset: let it finish (a few tens of ms).
  if (proc_.status().loading) proc_.waitForLoader(std::chrono::milliseconds(3000));
  slot_[active_] = proc_.currentPreset();
}

void AbCompare::toggle() {
  storeActive();
  const int other = 1 - active_;
  if (!slot_[other]) slot_[other] = slot_[active_];
  active_ = other;
  proc_.loadPreset(*slot_[active_]);
}

void AbCompare::copy(int from, int to) {
  storeActive();
  slot_[to] = slot_[from] ? slot_[from] : slot_[active_];
  if (to == active_) proc_.loadPreset(*slot_[to]);  // the active slot changed: load it
}

void AbCompare::reset() {
  storeActive();
  Preset cur = *slot_[active_];
  slot_[0] = cur;
  slot_[1].reset();
  active_ = 0;
}

}  // namespace sawblade::plugin
