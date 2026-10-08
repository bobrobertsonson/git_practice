#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <string>
#include <string_view>

#include "sawblade/gate.h"

// v0.8 I4b part 1: the stereo DI chooser (docs/specs/v0_8-I4b-plugin.md section 1). A guitar on one channel of a stereo track used to
// arrive at -6 dB through the plain 0.5 * (L + R) sum, which moves every planned NAM drive. This unit turns a stereo input into the one
// mono DI the rig sees: the plugin calls it exactly where the sum used to be, so the input meter, the DI recorder, the drift tap and the
// gate key all see the chosen signal. JUCE-free; process() allocates nothing, locks nothing and does no I/O.
//
//   Mode      Auto (default) | Left | Right | Mix. Forced modes take that channel (Mix = the old 0.5 * (L + R)) with no detection.
//   Auto      starts as Mix. Once there are about kStereoLatchPlayedS of PLAYED frames in which one channel sits >= kStereoGapDb below
//             the other, it latches the louder channel. "Played" is the drift tap's test (drift.h windowPlayed: a 50 ms window whose peak
//             is >= 12 dB above the DI's own noise floor, tracked by the same Gate floor follower), and the channel levels are measured
//             over those played windows only. The latch is applied only at the start of a block whose played state is false (never
//             mid-note). Once latched it is re-evaluated only on prepare() / requestRestart() (a bus-layout change), or after a
//             sustained reversal: the latched channel >= kStereoGapDb below the other for >= kStereoReversalPlayedS of played windows.
//   Fade      every change of the chosen signal (a latch, a reversal, a forced-mode change from Settings) is cross-faded linearly over
//             kStereoFadeMs (>= 20 ms), so the output never steps.
//   Mono      a mono input layout never reaches this class: the plugin keeps its plain copy, bit-identical to before.
//
// The decision (Mix / Left / Right, auto or forced) is published through an atomic for the UI. Detection counts window boundaries in
// samples, so what is detected does not depend on the block size; only the block a switch lands on does.
namespace sawblade {

enum class InputChannelMode : int { Auto = 0, Left = 1, Right = 2, Mix = 3 };
enum class InputChannel : int { Mix = 0, Left = 1, Right = 2 };

constexpr double kStereoLatchPlayedS = 2.0;       // played time in which one channel is >= kStereoGapDb below the other
constexpr double kStereoGapDb = 30.0;
constexpr double kStereoReversalPlayedS = 10.0;   // played time of a sustained reversal
constexpr double kStereoFadeMs = 30.0;            // cross-fade length (the spec asks for >= 20 ms)

// "auto" | "left" | "right" | "mix" (the settings file's spelling).
const char* inputChannelModeName(InputChannelMode m) noexcept;
std::optional<InputChannelMode> parseInputChannelMode(std::string_view s) noexcept;
// The read-only UI line: "Input: L only (auto)", "Input: L+R mix (auto)", "Input: R only (forced)", ...
std::string inputChannelText(InputChannelMode mode, InputChannel decision);

class StereoInputChooser {
 public:
  StereoInputChooser();

  // Off the audio thread, before audio runs. Restarts the detection (Auto starts as Mix again) and keeps the mode.
  void prepare(double sampleRate);

  // Any thread. A change fades in over kStereoFadeMs at the next block.
  void setMode(InputChannelMode m) noexcept { mode_.store(static_cast<int>(m), std::memory_order_relaxed); }
  InputChannelMode mode() const noexcept { return static_cast<InputChannelMode>(mode_.load(std::memory_order_relaxed)); }
  // Any thread: what the output follows right now (the target while a fade is running).
  InputChannel decision() const noexcept { return static_cast<InputChannel>(decision_.load(std::memory_order_relaxed)); }
  // Any thread: re-evaluate from scratch at the next block (a bus-layout change).
  void requestRestart() noexcept { restart_.store(true, std::memory_order_relaxed); }

  // Audio thread. `out` may alias `l` or `r`.
  void process(const float* l, const float* r, float* out, int n) noexcept;

 private:
  static constexpr int kSub = 256;
  InputChannel forcedChannel(InputChannelMode m) const noexcept;
  void restartState() noexcept;
  void applyAtBlockStart() noexcept;
  void windowEnd() noexcept;
  void render(const float* l, const float* r, float* out, int n) noexcept;
  static float source(InputChannel c, float l, float r) noexcept;

  std::atomic<int> mode_{static_cast<int>(InputChannelMode::Auto)};
  std::atomic<int> decision_{static_cast<int>(InputChannel::Mix)};
  std::atomic<bool> restart_{false};

  // Audio thread only.
  double sampleRate_ = 48000.0;
  int winLen_ = 2400, fadeLen_ = 1440;
  int latchNeedFrames_ = 96000, reversalNeedWindows_ = 200;
  Gate floor_;                              // the drift tap's floor follower, on max(|L|, |R|)
  InputChannelMode appliedMode_ = InputChannelMode::Auto;
  InputChannel settled_ = InputChannel::Mix;  // what the output follows once any fade has finished
  InputChannel fadeFrom_ = InputChannel::Mix, fadeTo_ = InputChannel::Mix;
  bool fading_ = false;
  int fadePos_ = 0;
  InputChannel latched_ = InputChannel::Mix;  // auto: the channel the detection settled on (Mix = none yet)
  std::optional<InputChannel> pending_;       // auto: a decision waiting for its moment
  bool pendingUrgent_ = false;                // a reversal does not wait for an unplayed block
  bool playedNow_ = false;                    // the last finished window was played
  // The window being measured.
  int pos_ = 0;
  float winPeak_ = 0.0f;
  double winEL_ = 0.0, winER_ = 0.0;
  // Detection epochs.
  double epochL_ = 0.0, epochR_ = 0.0;
  int epochFrames_ = 0;
  int reversalWindows_ = 0;
};

}  // namespace sawblade
