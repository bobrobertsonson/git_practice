#include "sawblade/pedal_eye.h"

namespace sawblade {

HmParams eyeAsHmParams(const EyeParams& p) noexcept {
  HmParams h;  // v3, stock mode, silicon, mix 100
  h.low = EyeConstants::eqLow;
  h.high = EyeConstants::eqHigh;
  h.distortion = EyeConstants::driveBase + EyeConstants::drivePerGain * p.gain;
  h.level = p.level;
  h.tightness = p.tightness;
  return h;
}

EyePedal::EyePedal(const EyeParams& p, PedalImplConfig cfg) : inner_(eyeAsHmParams(p), cfg) {
  eyeLiveFromParams(p, liveTarget_.data());
}

void EyePedal::setLiveParams(const float* values, int count) noexcept {
  const EyeParams np = eyeParamsFromLive(values, count);
  std::array<float, kEyeNumLive> nv;
  eyeLiveFromParams(np, nv.data());
  if (nv == liveTarget_) return;
  liveTarget_ = nv;
  std::array<float, kHmNumLive> hv;
  hmLiveFromParams(eyeAsHmParams(np), hv.data());
  inner_.setLiveParams(hv.data(), kHmNumLive);
}

std::unique_ptr<Processor> createEye(const Block& b, const BlockBuildContext&) {
  return std::make_unique<EyePedal>(static_cast<const EyeBlockParams&>(*b.params).p);
}

}  // namespace sawblade
