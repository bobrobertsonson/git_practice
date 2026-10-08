#include "sawblade/stereo_input.h"

#include <algorithm>
#include <cmath>

#include "sawblade/drift.h"

namespace sawblade {

namespace {
constexpr double kGapPower = 1000.0;  // kStereoGapDb = 30 dB in power
static_assert(kStereoGapDb == 30.0, "kGapPower is 10^(kStereoGapDb / 10)");
}  // namespace

const char* inputChannelModeName(InputChannelMode m) noexcept {
  switch (m) {
    case InputChannelMode::Auto: return "auto";
    case InputChannelMode::Left: return "left";
    case InputChannelMode::Right: return "right";
    case InputChannelMode::Mix: return "mix";
  }
  return "auto";
}

std::optional<InputChannelMode> parseInputChannelMode(std::string_view s) noexcept {
  if (s == "auto") return InputChannelMode::Auto;
  if (s == "left") return InputChannelMode::Left;
  if (s == "right") return InputChannelMode::Right;
  if (s == "mix") return InputChannelMode::Mix;
  return std::nullopt;
}

std::string inputChannelText(InputChannelMode mode, InputChannel decision) {
  const char* what = decision == InputChannel::Left ? "L only" : decision == InputChannel::Right ? "R only" : "L+R mix";
  return std::string("Input: ") + what + (mode == InputChannelMode::Auto ? " (auto)" : " (forced)");
}

StereoInputChooser::StereoInputChooser() { drift::configureFloorFollower(floor_); }

InputChannel StereoInputChooser::forcedChannel(InputChannelMode m) const noexcept {
  return m == InputChannelMode::Left ? InputChannel::Left : m == InputChannelMode::Right ? InputChannel::Right : InputChannel::Mix;
}

void StereoInputChooser::prepare(double sampleRate) {
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  winLen_ = std::max(1, static_cast<int>(std::llround(drift::kWindowMs * 0.001 * sampleRate_)));
  fadeLen_ = std::max(1, static_cast<int>(std::llround(kStereoFadeMs * 0.001 * sampleRate_)));
  latchNeedFrames_ = std::max(1, static_cast<int>(std::llround(kStereoLatchPlayedS * sampleRate_)));
  reversalNeedWindows_ = std::max(1, static_cast<int>(std::llround(kStereoReversalPlayedS * 1000.0 / drift::kWindowMs)));
  floor_.prepare({sampleRate_, winLen_});
  appliedMode_ = mode();
  restartState();
  // A fresh stream: no fade, the output follows the mode's channel at once.
  settled_ = appliedMode_ == InputChannelMode::Auto ? InputChannel::Mix : forcedChannel(appliedMode_);
  fadeFrom_ = fadeTo_ = settled_;
  fading_ = false;
  fadePos_ = 0;
  decision_.store(static_cast<int>(settled_), std::memory_order_relaxed);
  restart_.store(false, std::memory_order_relaxed);
}

// Detection back to "no evidence yet"; the floor follower starts over from its seed.
void StereoInputChooser::restartState() noexcept {
  floor_.reset();
  latched_ = InputChannel::Mix;
  pending_.reset();
  pendingUrgent_ = false;
  playedNow_ = false;
  pos_ = 0;
  winPeak_ = 0.0f;
  winEL_ = winER_ = 0.0;
  epochL_ = epochR_ = 0.0;
  epochFrames_ = 0;
  reversalWindows_ = 0;
}

float StereoInputChooser::source(InputChannel c, float l, float r) noexcept {
  return c == InputChannel::Left ? l : c == InputChannel::Right ? r : 0.5f * (l + r);
}

// Start of a block: pick up a changed mode, a restart request, and whatever decision is allowed to land now.
void StereoInputChooser::applyAtBlockStart() noexcept {
  if (restart_.exchange(false, std::memory_order_relaxed)) {
    restartState();
  }
  const InputChannelMode m = mode();
  if (m != appliedMode_) {  // a change from Settings: detection starts over, the fade below carries the switch
    appliedMode_ = m;
    restartState();
  }
  if (fading_) return;  // one fade at a time; whatever is wanted is looked at again when it ends
  InputChannel want;
  if (appliedMode_ != InputChannelMode::Auto) {
    want = forcedChannel(appliedMode_);
  } else {
    if (pending_ && (pendingUrgent_ || !playedNow_)) {  // never switch mid-note, except after a sustained reversal
      latched_ = *pending_;
      pending_.reset();
      pendingUrgent_ = false;
      epochL_ = epochR_ = 0.0;
      epochFrames_ = 0;
      reversalWindows_ = 0;
    }
    want = latched_;
  }
  if (want != settled_) {
    fadeFrom_ = settled_;
    fadeTo_ = want;
    fading_ = true;
    fadePos_ = 0;
    decision_.store(static_cast<int>(want), std::memory_order_relaxed);
  } else {
    decision_.store(static_cast<int>(settled_), std::memory_order_relaxed);
  }
}

// One finished 50 ms window of an Auto stream.
void StereoInputChooser::windowEnd() noexcept {
  playedNow_ = drift::windowPlayed(static_cast<double>(winPeak_), floor_);
  if (playedNow_) {
    if (latched_ == InputChannel::Mix) {
      if (!pending_) {
        epochL_ += winEL_;
        epochR_ += winER_;
        epochFrames_ += winLen_;
        if (epochFrames_ >= latchNeedFrames_) {
          if (epochL_ > 0.0 && epochR_ * kGapPower <= epochL_) pending_ = InputChannel::Left;
          else if (epochR_ > 0.0 && epochL_ * kGapPower <= epochR_) pending_ = InputChannel::Right;
          pendingUrgent_ = false;
          epochL_ = epochR_ = 0.0;
          epochFrames_ = 0;
        }
      }
    } else if (!pending_) {
      const double mine = latched_ == InputChannel::Left ? winEL_ : winER_;
      const double other = latched_ == InputChannel::Left ? winER_ : winEL_;
      if (other > 0.0 && mine * kGapPower <= other) {
        if (++reversalWindows_ >= reversalNeedWindows_) {
          pending_ = latched_ == InputChannel::Left ? InputChannel::Right : InputChannel::Left;
          pendingUrgent_ = true;
          reversalWindows_ = 0;
        }
      } else {
        reversalWindows_ = 0;
      }
    }
  }
  pos_ = 0;
  winPeak_ = 0.0f;
  winEL_ = winER_ = 0.0;
}

void StereoInputChooser::render(const float* l, const float* r, float* out, int n) noexcept {
  int i = 0;
  if (fading_) {
    const float inv = 1.0f / static_cast<float>(fadeLen_);
    for (; i < n && fading_; ++i) {
      const float w = static_cast<float>(fadePos_ + 1) * inv;
      const float a = source(fadeFrom_, l[i], r[i]), b = source(fadeTo_, l[i], r[i]);
      out[i] = a + (b - a) * w;
      if (++fadePos_ >= fadeLen_) {
        fading_ = false;
        settled_ = fadeTo_;
      }
    }
  }
  switch (settled_) {
    case InputChannel::Left:
      for (; i < n; ++i) out[i] = l[i];
      break;
    case InputChannel::Right:
      for (; i < n; ++i) out[i] = r[i];
      break;
    case InputChannel::Mix:
      for (; i < n; ++i) out[i] = 0.5f * (l[i] + r[i]);
      break;
  }
}

void StereoInputChooser::process(const float* l, const float* r, float* out, int n) noexcept {
  applyAtBlockStart();
  const bool detect = appliedMode_ == InputChannelMode::Auto;
  std::array<float, kSub> det;
  int i = 0;
  while (i < n) {
    const int len = std::min({n - i, kSub, winLen_ - pos_});
    if (detect) {  // before render(): `out` may alias an input
      float pk = winPeak_;
      double eL = 0.0, eR = 0.0;
      for (int k = 0; k < len; ++k) {
        const float a = l[i + k], b = r[i + k];
        const float m = std::max(std::fabs(a), std::fabs(b));
        det[static_cast<std::size_t>(k)] = m;
        pk = std::max(pk, m);
        eL += static_cast<double>(a) * a;
        eR += static_cast<double>(b) * b;
      }
      winPeak_ = pk;
      winEL_ += eL;
      winER_ += eR;
      floor_.followFloor(det.data(), len);
    }
    render(l + i, r + i, out + i, len);
    pos_ += len;
    i += len;
    if (pos_ >= winLen_) {
      if (detect) windowEnd();
      else {
        pos_ = 0;
        winPeak_ = 0.0f;
        winEL_ = winER_ = 0.0;
      }
    }
  }
}

}  // namespace sawblade
