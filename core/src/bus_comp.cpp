#include "sawblade/bus_comp.h"

#include <algorithm>
#include <cmath>

namespace sawblade {

double BusCompressor::staticGainDb(const BusCompParams& p, double levelDb) noexcept {
  const double over = levelDb - p.thresholdDb;
  const double slope = 1.0 / p.ratio - 1.0;
  if (p.kneeDb <= 0.0) return over > 0.0 ? slope * over : 0.0;
  if (2.0 * over < -p.kneeDb) return 0.0;
  if (2.0 * std::fabs(over) <= p.kneeDb) {
    const double t = over + 0.5 * p.kneeDb;
    return slope * t * t / (2.0 * p.kneeDb);
  }
  return slope * over;
}

void BusCompressor::prepare(const ProcessSpec& spec) {
  atk_ = std::exp(-1.0 / (std::max(params_.attackMs, 1e-3) * 1e-3 * spec.sampleRate));
  rel_ = std::exp(-1.0 / (std::max(params_.releaseMs, 1e-3) * 1e-3 * spec.sampleRate));
  reset();
}

void BusCompressor::process(float* io, int numSamples) noexcept {
  double env = env_, gr = grDb_;
  const double makeup = params_.makeupDb;
  for (int i = 0; i < numSamples; ++i) {
    const double x = io[i];
    const double a = std::fabs(x);
    env = a > env ? a : rel_ * env;
    const double target = staticGainDb(params_, 20.0 * std::log10(std::max(env, 1e-12)));
    // More reduction: attack. Less reduction: the detector's release already shapes the target.
    gr = target < gr ? atk_ * gr + (1.0 - atk_) * target : target;
    io[i] = static_cast<float>(x * std::pow(10.0, (gr + makeup) / 20.0));
  }
  env_ = env;
  grDb_ = gr;
}

}  // namespace sawblade
