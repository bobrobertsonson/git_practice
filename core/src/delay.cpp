#include "sawblade/delay.h"

#include <algorithm>
#include <cmath>

namespace sawblade {

void Gain::setGainDb(double db) noexcept { gain_ = static_cast<float>(std::pow(10.0, db / 20.0)); }

void DelayLine::prepare(const ProcessSpec&) {
  buf_.assign(static_cast<std::size_t>(max_) + 1, 0.0f);
  delay_ = std::min(delay_, max_);
  write_ = 0;
}

void DelayLine::reset() noexcept {
  std::fill(buf_.begin(), buf_.end(), 0.0f);
  write_ = 0;
}

void DelayLine::process(float* io, int numSamples) noexcept {
  const int size = static_cast<int>(buf_.size());
  if (size == 0) return;  // not prepared: pass through
  float* b = buf_.data();
  int w = write_;
  const int d = std::min(delay_, size - 1);
  for (int i = 0; i < numSamples; ++i) {
    b[w] = io[i];
    int r = w - d;
    if (r < 0) r += size;
    io[i] = b[r];
    if (++w == size) w = 0;
  }
  write_ = w;
}

}  // namespace sawblade
