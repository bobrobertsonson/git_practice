#pragma once

#include <vector>

#include "sawblade/adaa_clipper.h"
#include "sawblade/eq.h"
#include "sawblade/oversampler.h"
#include "sawblade/pedal_common.h"
#include "sawblade/pedal_params.h"
#include "sawblade/processor.h"

namespace sawblade {

// Fixed voicing constants of the pedal.muff model (docs/specs/phase7b_chainsaw_pedal.md, 2.2).
struct MuffVoicing {
  static constexpr double tightHz0 = 20.0, tightDecadeDiv = 10.0;  // input HPF 20 * 10^(tightness/10)
  static constexpr double aHpfHz = 80.0, aLpfHz = 4500.0;           // stage A pre (4x)
  static constexpr double bHpfHz = 30.0, bLpfHz = 4500.0;           // stage B pre (4x)
  static constexpr double postLpfHz = 8000.0, postQ = 0.7071067811865476;  // 2nd-order Butterworth (4x)
  static constexpr double gainABaseDb = 6.0, gainASlope = 3.0;      // GA dB = base + slope * sustain
  static constexpr double gainBDb = 18.0;                           // stage B gain (gain2Db trims it)
  static constexpr double crunchBase = 1.2, crunchSlope = 0.08;     // knee factor 1.2 - 0.08 * crunch
  static constexpr double voiceCentreHz = 860.0;                    // stack centre at voice 5; octave per 5 units
  static constexpr double scoopDbPerUnit = -1.6, peakQ = 0.8;
  static constexpr double recoveryDb = 6.0;
};

// "Big fuzz" (pedal.muff, model version 1): Big-Muff-family topology. Input buffer -> 4x oversampled
// [stage A pre -> gain -> clip -> stage B pre -> gain -> clip -> 2nd-order LPF] -> passive tone stack
// (LPF/HPF blend) + scoop notch -> recovery roll-off -> volume + clean mix. NAM-trainable.
// Latency: as pedal.hm (oversampler round trip + two ADAA2 stages, padded) = 50 samples; the clean
// mix is delayed by exactly latencySamples(). Live parameters follow the pedal.hm rules (the tone
// blend ramps too).
class MuffPedal : public Processor {
 public:
  explicit MuffPedal(const MuffParams& p, PedalImplConfig cfg = {});
  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;
  int latencySamples() const noexcept override { return latency_; }
  void setLiveParams(const float* values, int count) noexcept override;

 private:
  void designFilters(const MuffParams& p) noexcept;
  void setShapes(const MuffParams& p) noexcept;
  void retarget(bool immediate) noexcept;

  MuffParams target_, applied_;
  bool dirty_ = false;
  PedalImplConfig cfg_;
  double fs_ = 48000.0, fsOs_ = 192000.0;
  int latency_ = 0, pad_ = 0;
  int rampOs_ = 0, rampBase_ = 0;
  GainRamp gA_, gB_, wet_, dry_, tone_;
  OnePole inHpf_, aHpf_, aLpf_, bHpf_, bLpf_, stackLp_, stackHp_, rolloff_;
  Biquad post_, scoop_;
  AdaaClipper clipA_, clipB_;
  ShortDelay padDelay_;
  DryDelay dryDelay_;
  Oversampler4x os_;
  std::vector<float> osBuf_, tmp_;
};

}  // namespace sawblade
