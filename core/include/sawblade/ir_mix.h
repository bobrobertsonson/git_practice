#pragma once

#include <vector>

namespace sawblade {

// Combined IR of the `irMix` cab mode: h = (1 - mix) * a + mix * b, the shorter IR zero-padded to
// the longer one's length. The sum is NOT re-normalised (each input is normalised at load, when
// the cab asks for it). `mix` is clamped to [0, 1]. Load-time only (allocates).
//
// Optional alignment of b, applied before the sum: b'[i] = (invertB ? -1 : 1) * b[i - offsetB],
// with b[j] = 0 outside [0, b.size()). offsetB > 0 delays b (offsetB zeros are prepended);
// offsetB < 0 advances b (its first |offsetB| samples are dropped). The length rule is unchanged:
// h.size() == max(a.size(), b.size()) with the ORIGINAL sizes, so a delayed b loses its last
// offsetB samples (never grows the IR past the load-time 2 s cap) and an advanced b is zero-padded
// at the end. offsetB == 0 and !invertB is bit-identical to the plain mix.
std::vector<float> mixIrs(const std::vector<float>& a, const std::vector<float>& b, double mix, int offsetB = 0,
                          bool invertB = false);

}  // namespace sawblade
