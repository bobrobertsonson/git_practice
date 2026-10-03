#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "sawblade/block_registry.h"
#include "sawblade/bus_comp.h"
#include "sawblade/convolver.h"
#include "sawblade/delay.h"
#include "sawblade/eq.h"
#include "sawblade/gate.h"
#include "sawblade/preset.h"
#include "sawblade/processor.h"

namespace sawblade {

class CaptureCache;

// A built block plus the static facts the Chain needs (it never inspects the block `type`).
struct LoadedBlock {
  std::string id;
  std::string type;
  BlockTraits traits;
  bool bypass = false;  // bypassed blocks are skipped and contribute no latency
  std::unique_ptr<Processor> processor;
};

// Everything slow to build: models, IRs, convolvers. Built off the audio thread by
// loadResources(); handed to a Chain, which owns it afterwards.
struct ChainResources {
  double sampleRate = 0.0;
  std::array<std::vector<LoadedBlock>, 2> blocks;  // [0] = path A, [1] = path B; preset order
  std::unique_ptr<Convolver> cabShared, cabA, cabB;
  std::vector<std::string> warnings;
};

// Load-time. Builds every block through the BlockRegistry, loads IRs (resampled to
// `sampleRate`), verifies optional sha256 hashes. Throws std::runtime_error (JSON path or file
// in the message) on I/O, hash or model errors; those are CaptureError (carrying the JSON path
// of the capture's `file`). With a `cache`, NAM models and IRs are taken from / added to it
// instead of being read again; the result is bit-identical to the uncached load.
ChainResources loadResources(const Preset& preset, double sampleRate, CaptureCache* cache = nullptr);

struct AlignResult {
  int delaySamplesB = 0;  // +n delays B, -n delays A by n
  bool invertB = false;
  double peakCorrelation = 0.0;  // |normalized cross-correlation| at the chosen lag, 0..1
};

struct ChainInfo {
  std::array<int, 2> pathLatency{};      // sum of block latencies (+ own cab in perPath mode)
  std::array<int, 2> compensationDelay{};  // delay added so the shorter path meets the longer
  std::array<int, 2> alignDelay{};       // delay added by alignment (only one of A/B is non-zero)
  int latencySamples = 0;                // processing latency reported to the host (excludes alignDelay)
  AlignMode alignMode = AlignMode::Auto;
  AlignResult align;                     // the values in effect (resolved for auto)
  bool liveCompatible = false;           // cab.mode == shared
  struct Exactness {
    bool withCab = true;
    bool noCab = false;
  } exportExactness;
  std::vector<std::string> warnings;
};

// The two-path signal graph of CLAUDE.md:
//   in -> input gain -> gate (keyed on the DI) -> split
//     path: pre-EQ -> blocks -> path EQ -> level(+invert) -> [perPath cab] -> delay
//   -> blend -> [shared cab] -> post EQ -> bus comp -> output gain
// where `delay` is the latency compensation plus the alignment delay.
//
// Latency accounting: latencySamples() = max(pathLatencyA, pathLatencyB) (+ shared cab latency):
// processing latency only. The alignment delay (up to +-maxLagMs) is treated as part of the tone,
// like mic distance, and is reported separately as ChainInfo::alignDelay.
//
// Threading: ctor/prepare()/reset()/resolveAlignment()/info() are not RT-safe. Only process() is.
class Chain {
 public:
  Chain(const Preset& preset, ChainResources&& resources);  // throws std::runtime_error on mismatches
  ~Chain();
  Chain(const Chain&) = delete;
  Chain& operator=(const Chain&) = delete;

  // Prepares every block, sizes all buffers, and (align.mode == auto) runs resolveAlignment().
  // spec.sampleRate must equal the rate the resources were built for.
  void prepare(const ProcessSpec& spec);
  void reset();  // clears all state (not RT-safe: NAM prewarm)

  // `in` and `out` may alias. Any n >= 0 is accepted (split internally to maxBlockSize).
  void process(const float* in, float* out, int n) noexcept;

  int latencySamples() const noexcept { return latency_; }

  // Measures the alignment with the deterministic probe (see chain.cpp for the exact recipe),
  // at the blend point, gate bypassed, current latency compensation, no alignment delay.
  // Does not change the chain's settings; resets all state afterwards. Needs prepare().
  AlignResult resolveAlignment();

  ChainInfo info() const;

  // Probe definition (exposed for documentation and tests).
  static constexpr double kProbeSeconds = 1.0;
  static constexpr double kProbeLevelDbfs = -18.0;  // RMS of the white noise before band-passing
  static constexpr double kProbeLowHz = 80.0, kProbeHighHz = 5000.0;
  static constexpr unsigned long long kProbeSeed = 1;

 private:
  struct Path {
    ParametricEq preEq, eq;
    std::vector<LoadedBlock> blocks;
    Gain level;
    std::unique_ptr<Convolver> cab;  // perPath mode only
    DelayLine delay;
    bool enabled = true;
    int latency = 0;
    int compDelay = 0;
    int alignDelay = 0;
  };

  void processChunk(const float* in, float* out, int n) noexcept;
  void renderPath(Path& p, float* io, int n) noexcept;
  void applyAlignment(const AlignResult& r);
  void resetAll();

  Preset preset_;
  ChainResources res_;
  std::array<Path, 2> path_;
  Gain inGain_, outGain_;
  Gate gate_;
  bool gateOn_ = false;
  std::unique_ptr<Convolver> cabShared_;
  ParametricEq postEq_;
  BusCompressor comp_;
  bool compOn_ = false;
  float blendA_ = 0.5f, blendB_ = 0.5f;
  AlignResult align_;
  int latency_ = 0;
  int maxBlock_ = 0;
  bool prepared_ = false;
  std::vector<float> work_, bufA_, bufB_;
  std::vector<std::string> warnings_;       // static, from the preset and resources
  std::vector<std::string> alignWarnings_;  // from the last alignment
};

}  // namespace sawblade
