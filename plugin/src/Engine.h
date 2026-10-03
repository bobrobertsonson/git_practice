#pragma once

// JUCE-free audio engine of the plugin: one preset's Chain plus the real-time sample-rate
// conversion that lets it run at the models' training rate while the host runs at its own.
//
//   host in -> [RtResampler host->model] -> Chain (model rate) -> [RtResampler model->host] -> host out
//
// An Engine is built completely off the audio thread (Engine::build: loads models and IRs,
// prepares the chain, sizes every buffer) and handed to the audio thread through a SwapSlot. Its
// process() and setParams() are real-time safe: no allocation, locks, I/O or exceptions.
//
// Latency (exact, in host-rate samples, reported through setLatencySamples):
//   total = H1 + (C + H2 + s2/L2) * L2/M2
// where H1 is the input converter's delay in host samples, C the chain's reported latency in
// model-rate samples, H2 the output converter's delay in model samples and s2 a sub-sample phase
// offset of the output converter, chosen at build time so that the total is a whole number of
// host samples (see RtResampler). The same-rate case has no converters: total = C.
// The measured impulse delay of process() equals this exactly (tests).

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "PresetMapping.h"
#include "sawblade/chain.h"
#include "sawblade/rt_resample.h"

namespace sawblade::plugin {

struct EngineLatency {
  int total = 0;               // host-rate samples, what the host is told
  int chainModelSamples = 0;   // C: Chain::latencySamples(), model-rate samples
  int resamplerHostSamples = 0;  // total - round(C * host/model): the converters' share, host samples
  bool resampling = false;
};

class Engine {
 public:
  // Builds an engine for `preset` at the host rate. maxBlock is the largest block the host is
  // expected to use (process() accepts any size and splits it). Throws std::exception (with a
  // message naming the offending JSON path or file) if the preset cannot be loaded or prepared.
  static std::unique_ptr<Engine> build(const Preset& preset, double hostRate, int maxBlock);

  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // `in` and `out` may alias; any n >= 0.
  void process(const float* in, float* out, int n) noexcept;

  // Applies the parameter values (RT-safe; cheap when nothing changed). Gains, blend and post-EQ
  // are smoothed inside the chain; the gate threshold moves immediately.
  void setParams(const ParamValues& v) noexcept;

  double hostRate() const noexcept { return hostRate_; }
  double modelRate() const noexcept { return modelRate_; }
  const EngineLatency& latency() const noexcept { return latency_; }
  int latencySamples() const noexcept { return latency_.total; }
  const ChainInfo& chainInfo() const noexcept { return info_; }
  const std::string& presetName() const noexcept { return name_; }
  // Output samples the FIFO could not supply (must stay 0: the converters never need look-ahead).
  std::uint64_t underruns() const noexcept { return underruns_; }

 private:
  Engine() = default;

  double hostRate_ = 0.0, modelRate_ = 0.0;
  bool resampling_ = false;
  int hostMax_ = 0;
  std::unique_ptr<Chain> chain_;
  ChainInfo info_;
  std::string name_;
  EngineLatency latency_;
  LiveParams baseline_;
  SlotBands slotBand_{};
  RtResampler down_, up_;  // host -> model, model -> host
  std::vector<float> mod_, tmp_, fifo_;
  int fifoCount_ = 0;
  std::uint64_t underruns_ = 0;
};

}  // namespace sawblade::plugin
