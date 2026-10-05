#include "PreviewPlayer.h"

#include <algorithm>
#include <cmath>

namespace sawblade::plugin {

void PreviewPlayer::prepare(double sampleRate) noexcept {
  rate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  fadeLen_ = std::max(1, static_cast<int>(std::lround(kFadeSeconds * rate_)));
  active_ = false;
  playing_.store(false, std::memory_order_relaxed);
}

void PreviewPlayer::start(std::vector<float> samples, double sampleRate) {
  std::lock_guard<std::mutex> lk(producerMutex_);
  auto b = std::make_unique<Buf>();
  b->samples = std::move(samples);
  b->rate = sampleRate;
  b->serial = nextSerial_++;
  b->stopGen = stopGen_.load(std::memory_order_acquire);
  slot_.publish(std::move(b));
  slot_.collectGarbage();
}

void PreviewPlayer::stop() noexcept { stopGen_.fetch_add(1, std::memory_order_acq_rel); }

void PreviewPlayer::collect() {
  std::lock_guard<std::mutex> lk(producerMutex_);
  slot_.collectGarbage();
}

void PreviewPlayer::process(float* const* out, int numChannels, int numSamples) noexcept {
  const Buf* b = slot_.current();
  const std::uint64_t stopNow = stopGen_.load(std::memory_order_acquire);

  if (b != nullptr && b->serial != curSerial_) {  // a new preview
    curSerial_ = b->serial;
    pos_ = 0;
    fadeIn_ = 0;
    fadeOut_ = -1;
    // A stop() after the start() cancels it; one before it is already obsolete.
    active_ = std::fabs(b->rate - rate_) < 1e-6 && !b->samples.empty() && stopNow == b->stopGen;
    seenStop_ = stopNow;
  } else if (active_ && stopNow != seenStop_) {
    seenStop_ = stopNow;
    if (fadeOut_ < 0) fadeOut_ = 0;
  }
  if (!active_ || b == nullptr || numChannels <= 0) {
    playing_.store(false, std::memory_order_relaxed);
    return;
  }

  const std::size_t total = b->samples.size();
  const float inv = 1.0f / static_cast<float>(fadeLen_);
  for (int i = 0; i < numSamples; ++i) {
    if (!active_) break;
    float g = 1.0f;
    if (fadeIn_ < fadeLen_) g = static_cast<float>(fadeIn_++) * inv;
    // Natural end: ramp down over the last fadeLen_ samples of the buffer.
    const std::size_t left = total - pos_;
    if (left <= static_cast<std::size_t>(fadeLen_)) g = std::min(g, static_cast<float>(left) * inv);
    if (fadeOut_ >= 0) {
      g = std::min(g, 1.0f - static_cast<float>(fadeOut_) * inv);
      ++fadeOut_;
    }
    g = std::clamp(g, 0.0f, 1.0f);
    const float v = b->samples[pos_] * g;
    for (int ch = 0; ch < numChannels; ++ch) out[ch][i] = out[ch][i] * (1.0f - g) + v;
    ++pos_;
    if (pos_ >= total || (fadeOut_ >= static_cast<int>(fadeLen_))) active_ = false;
  }
  playing_.store(active_, std::memory_order_relaxed);
}

}  // namespace sawblade::plugin
