#pragma once

#include <memory>

#include "sawblade/block_registry.h"
#include "sawblade/pedal_common.h"
#include "sawblade/pedal_hm.h"
#include "sawblade/pedal_params.h"
#include "sawblade/pedal_saw_params.h"
#include "sawblade/processor.h"

namespace sawblade {

// "One-knob chainsaw" (pedal.eye; docs/specs/phase7c_chainsaw_family.md section 3.5, phase 7.1): a sealed
// fixed-knob chainsaw. It IS the v3 core (HmPedal with HmVoicing::v3()) at colour knobs L = 6.2, H = 7.1,
// the drive mapped from one knob, D = 3 + 0.5 * gain (D 3..8), plus `level` and a `tightness` low cut;
// no mix (the pedal runs at mix 100, so the dry branch is skipped), no clip choice (silicon). The
// pre-clip high-pass is the v3 voicing's. Static, nonlinear, time-invariant: NAM-trainable.
// Latency: 50 samples at every rate (the HmPedal's: oversampler round trip + two ADAA2 samples).
//
// Live parameters (setLiveParams): mapped to the inner pedal's live path, so the ramps, the single
// filter redesign per block and the "identical values are ignored" rule are the HmPedal's.
struct EyeConstants {
  static constexpr double eqLow = 6.2, eqHigh = 7.1;               // the fixed colour-mix EQ
  static constexpr double driveBase = 3.0, drivePerGain = 0.5;     // D = 3 + 0.5 * gain
};

// The v3 hm parameter set an eye block stands for (also what the tests compare against).
HmParams eyeAsHmParams(const EyeParams& p) noexcept;

class EyePedal : public Processor {
 public:
  explicit EyePedal(const EyeParams& p, PedalImplConfig cfg = {});
  void prepare(const ProcessSpec& spec) override { inner_.prepare(spec); }
  void reset() override { inner_.reset(); }
  void process(float* io, int numSamples) noexcept override { inner_.process(io, numSamples); }
  int latencySamples() const noexcept override { return inner_.latencySamples(); }
  void setLiveParams(const float* values, int count) noexcept override;

 private:
  HmPedal inner_;
  std::array<float, kEyeNumLive> liveTarget_{};
};

std::unique_ptr<Processor> createEye(const Block& b, const BlockBuildContext& ctx);

}  // namespace sawblade
