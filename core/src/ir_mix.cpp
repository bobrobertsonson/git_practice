#include "sawblade/ir_mix.h"

#include <algorithm>

namespace sawblade {

std::vector<float> mixIrs(const std::vector<float>& a, const std::vector<float>& b, double mix, int offsetB,
                          bool invertB) {
  mix = std::clamp(mix, 0.0, 1.0);
  const double wa = 1.0 - mix, wb = mix;
  std::vector<float> h(std::max(a.size(), b.size()), 0.0f);
  for (std::size_t i = 0; i < h.size(); ++i) {
    const double x = i < a.size() ? static_cast<double>(a[i]) : 0.0;
    const long long j = static_cast<long long>(i) - offsetB;
    const double yb = j >= 0 && j < static_cast<long long>(b.size()) ? static_cast<double>(b[static_cast<std::size_t>(j)]) : 0.0;
    const double y = invertB ? -yb : yb;
    h[i] = static_cast<float>(wa * x + wb * y);
  }
  return h;
}

}  // namespace sawblade
