#pragma once

#include <algorithm>
#include <optional>
#include <string>

#include "IrPack.h"
#include "sawblade/preset.h"

// The state behind the mic page: the loaded IR pack and which of its models the cab uses (one mic, or two when BLEND
// is on). JUCE-free. Every action returns the Preset to load (built from `base`, normally the processor's
// currentPreset(), with only the cab replaced) or nullopt when there is nothing to do; the caller hands it to
// processor.loadPreset(), i.e. the normal off-thread loader and the 30 ms equal-power swap.
//
// The session keeps its own copy of the cab captures, so several quick actions (the loader is "latest wins") never
// lose one another even while the processor's preset has not caught up.
namespace sawblade::plugin::mic {

class MicSession {
 public:
  MicSession();

  // --- pack ---
  void setPack(IrPack pack);                   // the layout of the current cab is applied
  void setLayout(const CabLayout& layout);     // the cab image's sidecar (4x12 or 2x12)
  const IrPack& pack() const { return pack_; }
  // If the cab's TONE3000 tone has a cached manifest (<appdata>/sawblade/packs/<toneId>.json), loads it as the pack.
  bool loadCachedPack(std::string* error = nullptr);
  // The TONE3000 tone id of the cab IR ("" if none): what LOAD PACK would fetch.
  std::string toneId() const { return toneIdOf(cap_[0]); }

  // --- the cab ---
  // Takes the cab of `p` (mode, captures, mix). Returns true if anything the page shows changed. A pack of another
  // tone (or a single IR) is replaced by the single IR of the new cab; a folder pack is kept.
  bool adopt(const Preset& p);
  bool readOnly() const { return perPath_; }  // studio blend (perPath): the page only shows
  bool blend() const { return blend_; }
  int active() const { return active_; }      // the mic the fields describe: 0 or 1
  void setActive(int mic) { active_ = mic == 1 && blend_ ? 1 : 0; }
  int model(int mic) const;                   // the pack model of mic 0 / 1, -1 if its IR is not in the pack
  const Capture& capture(int mic) const { return cap_[mic == 1 ? 1 : 0]; }
  double mix() const { return mix_; }
  const CabPreset& cab() const { return cab_; }  // the cab as last adopted / built (enabled, normalize)

  // --- actions ---
  std::optional<Preset> choose(const Preset& base, int mic, int model);
  std::optional<Preset> setBlend(const Preset& base, bool on);
  void setMixValue(double mix) { mix_ = std::clamp(mix, 0.0, 1.0); }  // no preset yet (the fader's throttle submits)
  std::optional<Preset> setMix(const Preset& base, double mix);
  std::optional<Preset> toggleAB(const Preset& base);        // the active mic's last two chosen IRs
  std::optional<Preset> nextPosition(const Preset& base);    // the next dot, for the active mic
  // `base` with the cab written from the session's state.
  Preset build(const Preset& base) const;

  bool canToggleAB() const { return !readOnly() && prev_[active_].has_value(); }

 private:
  void refreshPackFor(const Capture& a);

  IrPack pack_;
  CabLayout layout_;
  CabPreset cab_;
  Capture cap_[2];
  std::optional<Capture> prev_[2];
  bool blend_ = false, perPath_ = false;
  int active_ = 0;
  double mix_ = 0.5;
};

// True if the cabs are the same as far as the sound goes: mode, enabled, normalize and the captures / mix the mode uses.
bool sameCab(const CabPreset& a, const CabPreset& b);

}  // namespace sawblade::plugin::mic
