#pragma once

#include <vector>

namespace sawblade {

// Combined IR of the `irMix` cab mode: h = (1 - mix) * a + mix * b, the shorter IR zero-padded to
// the longer one's length. The sum is NOT re-normalised (each input is normalised at load, when
// the cab asks for it). `mix` is clamped to [0, 1]. Load-time only (allocates).
std::vector<float> mixIrs(const std::vector<float>& a, const std::vector<float>& b, double mix);

}  // namespace sawblade
