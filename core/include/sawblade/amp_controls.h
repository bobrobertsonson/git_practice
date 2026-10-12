#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "sawblade/eq.h"
#include "sawblade/processor.h"

// v0.2 Task A: the per-path amp control set (GAIN / BASS / MID / TREBLE / PRESENCE / LEVEL).
// A path-level stage owned by the Chain (not a block type): GAIN is a drive before the path's amp
// block, the tone stack and LEVEL come right after it. See docs/PRESET_SCHEMA.md "Amp controls".
namespace sawblade {

constexpr double kAmpKnobMin = 0.0, kAmpKnobMax = 10.0, kAmpKnobDefault = 5.0;

// The six knobs, 0..10, 5 = neutral. Plain data (no heap) so it can live in LiveParams.
struct AmpKnobs {
  double gain = kAmpKnobDefault, bass = kAmpKnobDefault, mid = kAmpKnobDefault;
  double treble = kAmpKnobDefault, presence = kAmpKnobDefault, level = kAmpKnobDefault;
  bool isDefault() const noexcept {
    return gain == kAmpKnobDefault && bass == kAmpKnobDefault && mid == kAmpKnobDefault &&
           treble == kAmpKnobDefault && presence == kAmpKnobDefault && level == kAmpKnobDefault;
  }
  bool operator==(const AmpKnobs&) const = default;
};

// The preset's `paths.<a|b>.ampControls`: the knobs plus the chosen gain-ladder rung (a TONE3000
// model id; parsed and round-tripped only, it has no effect before v0.2 Task B).
struct AmpControls : AmpKnobs {
  std::string gainStep;
  bool isDefault() const noexcept { return AmpKnobs::isDefault() && gainStep.empty(); }
  const AmpKnobs& knobs() const noexcept { return *this; }
  bool operator==(const AmpControls&) const = default;
};

// Knob -> dB. GAIN / BASS / MID / TREBLE / LEVEL: (k - 5) * 2.4 (+-12 dB); PRESENCE (k - 5) * 1.8 (+-9 dB).
// Exactly 0.0 at k == 5.
constexpr double kAmpDbPerKnob = 2.4, kAmpPresenceDbPerKnob = 1.8;
constexpr double ampKnobDb(double k) noexcept { return (k - kAmpKnobDefault) * kAmpDbPerKnob; }
constexpr double ampPresenceDb(double k) noexcept { return (k - kAmpKnobDefault) * kAmpPresenceDbPerKnob; }

// The tone stack's four filters (in processing order), designed for knob dB values.
constexpr int kAmpToneBands = 4;
EqBand ampToneBand(int index, double gainDb);  // 0 bass, 1 mid, 2 treble, 3 presence

// The stage. Neutral (every knob 5, no ramp running) is skipped entirely: legacy presets render
// bit-identically, no filter runs. Knob changes ramp per sample (GAIN / LEVEL, in dB) and on a fixed
// kGrid-sample grid counted from prepare() (the tone filters: ramp advanced and biquads redesigned on the
// grid), so the output does not depend on how the host splits the stream into blocks.
// Linear and time-invariant at fixed knobs, latency 0 (NAM-trainable: kAmpControlsTraits).
class AmpStage {
 public:
  static constexpr int kGrid = 32;

  // Not RT-safe. Settles every ramp on its target, clears the filters and restarts the grid.
  void prepare(const ProcessSpec& spec);
  void reset() noexcept;

  // Not RT-safe in spirit (no ramp): sets the knobs at once. For construction / before prepare().
  void setKnobsNow(const AmpKnobs& k) noexcept;
  // RT-safe. Ramps to `k` over `rampSamples`. Values outside [0, 10] are clamped, non-finite ones ignored
  // (the field keeps its value). An unchanged knob set is a no-op.
  void setKnobs(const AmpKnobs& k, int rampSamples) noexcept;

  // GAIN, in place; call before the amp block. LEVEL + tone stack; call right after it. RT-safe.
  void processPre(float* io, int n) noexcept;
  void processPost(float* io, int n) noexcept;

  const AmpKnobs& knobs() const noexcept { return target_; }  // the targets
  bool neutral() const noexcept { return !gain_.active() && post_.idle; }

 private:
  struct DbGain {  // per-sample dB ramp -> linear gain
    double cur = 0.0, target = 0.0, step = 0.0;
    int remaining = 0;
    float lin = 1.0f;
    bool active() const noexcept { return remaining > 0 || cur != 0.0; }
    void setNow(double db) noexcept;
    void rampTo(double db, int samples) noexcept;
    void process(float* io, int n) noexcept;
  };
  struct GridRamp {  // advanced once per grid point
    double cur = 0.0, target = 0.0, step = 0.0;
    int remaining = 0;
  };
  struct Post {
    DbGain level;
    std::array<GridRamp, kAmpToneBands> band{};
    std::array<Biquad, kAmpToneBands> filter{};
    std::array<bool, kAmpToneBands> on{};
    bool idle = true;
  };

  void design(int band) noexcept;
  void gridUpdate() noexcept;
  void settle() noexcept;

  double fs_ = 48000.0;
  AmpKnobs target_{};
  DbGain gain_;
  Post post_;
  std::uint64_t counter_ = 0;  // samples since prepare() / reset(): the tone-filter grid is absolute
};

}  // namespace sawblade
