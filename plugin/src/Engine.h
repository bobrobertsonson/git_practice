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
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "PresetMapping.h"
#include "pedals/CircuitParams.h"
#include "sawblade/chain.h"
#include "sawblade/rt_resample.h"

namespace sawblade {
class CaptureCache;
}

namespace sawblade::plugin {

struct EngineLatency {
  int total = 0;               // host-rate samples, what the host is told
  int chainModelSamples = 0;   // C: Chain::latencySamples(), model-rate samples
  int resamplerHostSamples = 0;  // total - round(C * host/model): the converters' share, host samples
  bool resampling = false;
};

// What travels through the processor's live SwapSlot: the live values of the preset (EQ design,
// block gains, mutes) for the engine built for request `generation`.
struct LiveSnapshot {
  std::uint64_t generation = 0;
  LiveParams live;
};

class Engine {
 public:
  // Builds an engine for `preset` at the host rate. maxBlock is the largest block the host is
  // expected to use (process() accepts any size and splits it). Throws std::exception (with a
  // message naming the offending JSON path or file) if the preset cannot be loaded or prepared.
  // With a `cache`, models and IRs are taken from / added to it (the loader keeps one for its whole
  // life, so structural edits do not re-read files); without, a private cache is used.
  static std::unique_ptr<Engine> build(const Preset& preset, double hostRate, int maxBlock, CaptureCache* cache = nullptr);

  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // `in` and `out` may alias; any n >= 0.
  void process(const float* in, float* out, int n) noexcept;

  // Applies the parameter values (RT-safe; cheap when nothing changed). Gains, blend and post-EQ
  // are smoothed inside the chain; the gate threshold moves immediately. The values of the circuit
  // set that belongs to this engine's first pedal block (docs/specs/phase7b, 5.1) are forwarded to
  // the block as live parameters when any of them changed.
  // `extras` (the live snapshot of this engine's generation, or null) replaces the baseline as the
  // starting point; the parameter values are overlaid on it.
  // `mutesFrom` (any snapshot) supplies the monitor mutes when `extras` is null (an outgoing engine).
  void setParams(const ParamValues& v, const LiveParams* extras = nullptr, const LiveParams* mutesFrom = nullptr) noexcept;

  // v0.3 level matching: the auto trim in dB (a plain gain after the OUTPUT knob; Chain::setAutoTrimDb). RT-safe. The first call
  // takes effect at once, later changes ramp over Chain::kAutoTrimRampMs. 0 = no trim.
  void setAutoTrimDb(double db) noexcept { chain_->setAutoTrimDb(db); }

  // Before the engine is published: start with these paths muted (no ramp), so a rebuild keeps a mute / solo.
  void setInitialMutes(bool a, bool b) {
    baseline_.muteA = a;
    baseline_.muteB = b;
    chain_->presetMutes(a, b);
  }

  // The id of the loader request this engine was built for (0 until the loader tags it).
  std::uint64_t generation() const noexcept { return generation_; }
  void setGeneration(std::uint64_t g) noexcept { generation_ = g; }

  // The values the chain currently applies, and the ones it was built with (tests: after the
  // parameters have been applied they must still be equal; see PresetMapping.h, snapParam).
  const LiveParams& liveParams() const noexcept { return chain_->liveParams(); }
  const LiveParams& baseline() const noexcept { return baseline_; }

  double hostRate() const noexcept { return hostRate_; }
  double modelRate() const noexcept { return modelRate_; }
  const EngineLatency& latency() const noexcept { return latency_; }
  int latencySamples() const noexcept { return latency_.total; }
  const ChainInfo& chainInfo() const noexcept { return info_; }
  const std::string& presetName() const noexcept { return name_; }

  // --- gain ladders (v0.2 Task B) -------------------------------------------------------------------
  // State of path 0 = a / 1 = b's gain ladder: any thread (atomics).
  LadderState ladderState(int path) const noexcept { return chain_->ladderState(path); }
  // The clamped preset the engine was built from (its ladders and gainSteps).
  const Preset& builtPreset() const noexcept { return preset_; }
  // Worker thread only (never the audio thread, never two at once): loads the cached rung models nearest the rung the GAIN
  // knob asks for (at most kMaxLoadedRungs; the sounding rung always stays), hands them to the audio thread through the
  // block's SwapSlot, evicts the others, and retries a hand-over that is still staged. Returns the number of wanted rungs
  // whose model is not in the capture cache (those stay "pending": GAIN is drive-only until they are). Failures are
  // recorded in ladderMessages().
  int refreshRungs(CaptureCache* cache);
  std::vector<std::string> ladderMessages() const;
  // Output samples the FIFO could not supply (must stay 0: the converters never need look-ahead).
  std::uint64_t underruns() const noexcept { return underruns_; }

 private:
  Engine() = default;

  Preset preset_;           // clamped; set at build
  ProcessSpec spec_{};      // what the chain was prepared with (rung models are prepared the same way)
  mutable std::mutex ladderMutex_;
  std::vector<std::string> ladderMessages_;
  double hostRate_ = 0.0, modelRate_ = 0.0;
  bool resampling_ = false;
  int hostMax_ = 0;
  std::unique_ptr<Chain> chain_;
  ChainInfo info_;
  std::string name_;
  EngineLatency latency_;
  LiveParams baseline_;
  SlotBands slotBand_{};
  std::optional<CircuitSlot> circuit_;                // the first pedal block, recorded at build
  std::array<float, kMaxCircuitLive> circuitLive_{};  // what the block currently applies
  std::array<float, kMaxCircuitLive> circuitScratch_{};
  int circuitLiveCount_ = 0;
  RtResampler down_, up_;  // host -> model, model -> host
  std::vector<float> mod_, tmp_, fifo_;
  int fifoCount_ = 0;
  std::uint64_t underruns_ = 0;
  std::uint64_t generation_ = 0;
};

}  // namespace sawblade::plugin
