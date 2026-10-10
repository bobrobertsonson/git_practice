#pragma once

#include "sawblade/processor.h"

namespace sawblade {

// Parameters and defaults per docs/PRESET_SCHEMA.md ("Bus compressor").
struct BusCompParams {
  bool enabled = false;
  double thresholdDb = -12.0;
  double ratio = 2.0;
  double kneeDb = 6.0;
  double attackMs = 10.0;
  double releaseMs = 100.0;
  double makeupDb = 0.0;
  bool operator==(const BusCompParams&) const = default;
};

// Release above which the compressor is flagged "not NAM-trainable".
constexpr double kBusCompMaxTrainableReleaseMs = 150.0;

// Feed-forward compressor, peak detector, soft knee.
//   detector: peak follower of |x| in the linear domain: instant attack (it jumps to any larger
//             |x|), exponential decay with the release time constant;
//   gain computer: Giannoulis/Massberg/Reiss soft-knee static curve on the detector level in dB;
//   smoothing: the gain reduction (dB) moves toward a larger reduction with the attack time
//             constant and recovers at the detector's release rate;
//   gain = smoothed gain reduction + makeup, applied to the same sample (feed-forward).
// A steady tone therefore settles to the static curve at its peak level (to within the detector's
// small per-cycle droop). The `enabled` flag is policy for the owner (Chain skips a disabled
// compressor); process() always compresses.
class BusCompressor : public Processor {
 public:
  void setParams(const BusCompParams& p) noexcept { params_ = p; }
  const BusCompParams& params() const noexcept { return params_; }

  // Static curve: output gain in dB (excluding makeup) for a steady level of `levelDb` dBFS.
  static double staticGainDb(const BusCompParams& p, double levelDb) noexcept;

  void prepare(const ProcessSpec& spec) override;
  void reset() override { env_ = 0.0; grDb_ = 0.0; }
  void process(float* io, int numSamples) noexcept override;

 private:
  BusCompParams params_{};
  double atk_ = 0.0, rel_ = 0.0;
  double env_ = 0.0;
  double grDb_ = 0.0;  // smoothed gain reduction (<= 0)
};

}  // namespace sawblade
