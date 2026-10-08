#pragma once

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sawblade/amp_controls.h"
#include "sawblade/block_registry.h"
#include "sawblade/bus_comp.h"
#include "sawblade/calibration.h"
#include "sawblade/convolver.h"
#include "sawblade/delay.h"
#include "sawblade/drift.h"
#include "sawblade/eq.h"
#include "sawblade/gain_ladder.h"
#include "sawblade/gate.h"
#include "sawblade/preset.h"
#include "sawblade/processor.h"
#include "sawblade/swap_slot.h"

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

// The rate a model is assumed to run at when it records none (NAM convention: 48 kHz).
constexpr double kAssumedNamSampleRate = 48000.0;

struct NamRateProbe {
  std::string where;  // "paths.a.blocks[0] (id)"
  double hz = 0.0;    // recorded training rate, or kAssumedNamSampleRate
  bool recorded = true;
};

// Training rates of the NAM blocks that will actually run (not bypassed, on an enabled path). A
// model that records no rate counts as kAssumedNamSampleRate. Loads the models (through `cache`
// if given). Throws CaptureError (JSON path of the `file` member) if a model cannot be loaded.
// Shared by tonerender's `--render-rate auto` and the plugin so both pick the same rate.
std::vector<NamRateProbe> probeNamRates(const Preset& preset, CaptureCache* cache = nullptr);

struct ModelRate {
  std::optional<double> hz;  // the common rate; none if no NAM block runs or they disagree
  bool ambiguous = false;    // blocks disagree
  std::string listing;       // when ambiguous: "paths.a.blocks[0] (a1): 48000 Hz, ..."
};
ModelRate commonModelRate(const std::vector<NamRateProbe>& probes);

struct AlignResult {
  int delaySamplesB = 0;  // +n delays B, -n delays A by n
  bool invertB = false;
  double peakCorrelation = 0.0;  // |normalized cross-correlation| at the chosen lag, 0..1
};

// Phase 10.1 level-match probe result (see Chain::resolveLevelMatch()).
struct LevelMatchResult {
  bool measured = false;                    // false: pass skipped (a path disabled / silent)
  std::array<double, 2> measuredTrimDb{};   // from the probe, always (>= 0, one of them 0)
  std::array<double, 2> trimDb{};           // in effect for the preset's mode (auto: measured)
  std::array<double, 2> lufs{kNoLufs, kNoLufs};  // guitar segment per path (levelDb applied, no trim)
  double sumLufs = kNoLufs;                 // aligned linear sum at blend 0.5 after trims
  std::array<double, 5> makeupDb{};         // constant-loudness make-up at b = 0, .25, .5, .75, 1
  std::vector<std::string> warnings;
  static constexpr double kNoLufs = -1000.0;  // "not measured" (JSON null)
};

struct ChainInfo {
  std::array<int, 2> pathLatency{};      // sum of block latencies (+ own cab in perPath mode)
  std::array<int, 2> compensationDelay{};  // delay added so the shorter path meets the longer
  std::array<int, 2> alignDelay{};       // delay added by alignment (only one of A/B is non-zero)
  int latencySamples = 0;                // processing latency reported to the host (excludes alignDelay)
  AlignMode alignMode = AlignMode::Auto;
  AlignResult align;                     // the values in effect (resolved for auto)
  std::string cabMode = "shared";        // "shared" | "perPath" | "irMix"
  LevelMatchMode levelMatchMode = LevelMatchMode::Off;
  bool levelMeasured = false;            // the level-match probe ran and measured both paths
  std::array<double, 2> trimDb{};        // level trims in effect (dB)
  std::array<double, 2> lufs{LevelMatchResult::kNoLufs, LevelMatchResult::kNoLufs};
  double sumLufs = LevelMatchResult::kNoLufs;
  std::array<double, 5> makeupDb{};      // constant-loudness make-up at b = 0, .25, .5, .75, 1
  BlendLaw blendLaw = BlendLaw::Linear;  // the preset's law (the live law may differ)
  bool liveCompatible = false;           // no cab (cab.enabled false), or cab.mode shared / irMix (one cab IR after the blend)
  struct Exactness {
    bool withCab = true;
    bool noCab = false;
  } exportExactness;
  std::vector<std::string> warnings;
};

// v0.8 input calibration at the chain level (opt-in: off by default, and off leaves every sample bit-identical). With it on
// each NAM block's input gain becomes planned + intent: planned = (level arriving at the block) - (the capture's
// input_level_dbu), the reference starting at the interface's dBu at 0 dBFS (kAssumedDeviceDbu when not calibrated);
// intent = the block's own inputGainDb (preset / matcher / live), with INPUT and the amp GAIN knob still on top. Between NAM
// blocks of a path the hop is planned from the metadata alone: a block that feeds another NAM block keeps its outputGainDb
// but drops normalizeLoudness and the capture-swap make-up; only the last NAM block of a path keeps them. The gate is keyed
// on the DI before any of this.
struct ChainCalibration {
  bool enabled = false;
  calibration::DeviceCalibration device;
  calibration::CalibrationDefaults defaults = calibration::defaultCalibrationDefaults();
};

// One block of a path as the plan sees it (preset block order).
struct CalibrationBlockPlan {
  std::string id;
  bool planned = false;  // a non-bypassed block of an enabled path (only these take part in the plan)
  calibration::LevelKind kind = calibration::LevelKind::Neutral;
  calibration::GearKind gear = calibration::GearKind::Unknown;
  double refBeforeDbu = 0.0;
  double gainInDb = 0.0;  // planned input gain (a gain-ladder block: its starting rung's)
  bool feedsNam = false;
  bool inputMissing = false, outputMissing = false;
  std::optional<double> captureInputDbu, captureOutputDbu;
};

struct CalibrationPlan {
  bool enabled = false;
  double deviceDbu = calibration::kAssumedDeviceDbu;  // the level planned with
  bool deviceAssumed = true;                          // kAssumedDeviceDbu was used (device not calibrated)
  bool anyUncalibrated = false;                       // a planned NAM block lacks input or output metadata
  std::array<std::vector<CalibrationBlockPlan>, 2> blocks;  // [0] = path A, [1] = path B; one entry per preset block
  std::uint64_t seq = 0;                              // publishCalibration() order
  ChainCalibration setting;
};

// Gain-ladder state of one path's amp block (v0.2 Task B), read from any thread (atomics of the LadderBlock).
struct LadderState {
  bool has = false;        // the path's amp block has a gain ladder
  int rungCount = 0;
  int own = -1;            // the rung that is the block's own capture
  int active = -1;         // the rung sounding
  int committed = -1;      // the rung sounding or being faded to
  int target = -1;         // the rung the GAIN knob asks for
  bool pending = false;    // target != committed: that rung's model is not loaded yet (GAIN is drive-only)
  double position = 0.0;   // GAIN-knob position of the committed rung
  std::uint64_t loadedMask = 0, rejectedMask = 0;
};

struct LiveEqBand {
  double freq = 0.0, gainDb = 0.0, q = 0.0;  // freq 0 = no band at that index
  bool operator==(const LiveEqBand&) const = default;
};
using LiveEq = std::array<LiveEqBand, ParametricEq::kMaxBands>;

struct LiveBlock {
  double inputGainDb = 0.0, outputGainDb = 0.0;
  double makeupDb = 0.0;  // the capture-swap make-up, apart from outputGainDb (input calibration drops it on a hop)
  bool operator==(const LiveBlock&) const = default;
};

// The continuous controls that can change while the chain runs, without rebuilding it (the
// plugin's parameters). Defaults come from the preset (LiveParams::fromPreset).
struct LiveParams {
  double inputGainDb = 0.0;
  double outputGainDb = 0.0;
  double gateThresholdDb = -55.0;  // only audible when the active gate is enabled and absolute
  // The active dynamics set (gate + bus comp, preset v4). Applied as one object, between blocks: a change of mode (record <->
  // live) never leaves a block with half of each. Moves the gate threshold immediately like gateThresholdDb.
  DynamicsSet dynamics{};
  double blend = 0.5;              // 0 = path A only, 1 = path B only
  BlendLaw blendLaw = BlendLaw::Linear;  // live: switching never rebuilds the chain
  double levelDbA = 0.0;
  double levelDbB = 0.0;
  // Live design of each band of the post EQ and of each path's pre / path EQ, by band index
  // (freq 0 = no band at that index; pass bands ignore gainDb; changing a band's type, enabled flag
  // or the number of bands is structural and not live). [0] = path A, [1] = path B.
  LiveEq postEq{};
  std::array<LiveEq, 2> preEq{}, pathEq{};
  // Input / output gain of each block (by block index; `nam` blocks only, others ignore it).
  std::array<std::array<LiveBlock, kMaxBlocksPerPath>, 2> blocks{};
  // Amp controls of each path ([0] = a, [1] = b); they act only on a path that has an amp block (ampIndex).
  // Ramped like the other live gains (see AmpStage); out-of-range values are clamped, non-finite ones ignored.
  std::array<AmpKnobs, 2> amp{};
  // Monitoring (not preset state): ramp the path's level to silence and back.
  bool muteA = false, muteB = false;

  static LiveParams fromPreset(const Preset& p);
  bool operator==(const LiveParams&) const = default;
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
// Live parameters: setLiveParams() changes the LiveParams controls in place (no model reload, no
// allocation). Gains and the blend are smoothed with sample-accurate linear ramps over
// kLiveRampMs; EQ bands (post, and each path's pre / path EQ) ramp gain in dB, freq in log2(Hz)
// and Q in log, redesigning the band every kEqSubBlock samples on an absolute sample grid; block
// input / output gains ramp linearly; a mute ramps the path level to 0 and back.
// The gate threshold moves immediately (it is a state-machine threshold, not an audio gain).
// The alignment (resolved at prepare()) is not re-resolved when levels change.
//
// Threading: ctor/prepare()/reset()/resolveAlignment()/info() are not RT-safe. process() and
// setLiveParams() are (call both from the audio thread, or otherwise serialise them).
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

  static constexpr double kLiveRampMs = 20.0;
  static constexpr int kEqSubBlock = 32;
  // RT-safe. Only changed fields act, so calling this every block with the same values is cheap.
  void setLiveParams(const LiveParams& p) noexcept;  // invalid (non-finite / out-of-range) fields are ignored
  const LiveParams& liveParams() const noexcept { return live_; }
  // v0.3 level matching (auto_trim.h): a plain gain after the output gain, in dB (not part of LiveParams, the preset or the
  // trained chain: NAM export and the matcher render without it). The first call sets it at once; later changes ramp over
  // kAutoTrimRampMs. Non-finite values are ignored. RT-safe, cheap when unchanged. Unity (the default) leaves every sample
  // bit-identical.
  void setAutoTrimDb(double db) noexcept;
  double autoTrimDb() const noexcept { return autoTrimNowDb_; }
  static constexpr double kAutoTrimRampMs = 250.0;
  // --- v0.8 I2 live-gate floor seed ---
  // Where the live gate's floor follower starts (default -70 dBFS; see Gate::setFloorSeedDb). Not RT-safe against a running
  // process(): call before audio starts (the plugin does it before the engine is published).
  void setGateFloorSeedDb(double db) noexcept { gate_.setFloorSeedDb(db); }
  // The floor the live gate has learned (dBFS), or NaN while it has not filled its 3 s window (or the gate is off / not floor
  // relative). Published by process() through a relaxed atomic float: any thread may read it, nothing is allocated or locked.
  double gateFloorSeedDb() const noexcept { return gate_.floorSeedDb(); }  // where the follower starts (default -70 dBFS)
  float learnedGateFloorDb() const noexcept { return gateFloorOut_.load(std::memory_order_relaxed); }

  // --- v0.8 I3 input-level drift check ---
  // Off by default (process() then does exactly what it did before). On: process() feeds the tap with the DI (before INPUT and
  // before any calibration gain) over the frames where the live gate is open; with the gate off there is no statistic. Any thread.
  void setDriftTapEnabled(bool on) noexcept { driftTap_.setEnabled(on); }
  const drift::PeakTap& driftTap() const noexcept { return driftTap_; }

  // --- v0.8 input calibration (see ChainCalibration) ---
  // Plans the levels of the built blocks. Allocates; reads only data that is immutable after construction, so any thread.
  CalibrationPlan planCalibration(const ChainCalibration& c) const;
  // Plans and applies at once (a ramp over kLiveRampMs once prepared, immediate before). Not RT-safe and not safe against a
  // running process(): call before audio starts or serialise with the audio thread. Off (enabled false) restores the plain
  // gains. calibrationPlan() then returns what was applied.
  void setCalibration(const ChainCalibration& c);
  const CalibrationPlan& calibrationPlan() const noexcept { return calPlan_; }
  // Swap-safe while audio runs: plans on the calling (producer) thread and hands the plan to the audio thread through a
  // SwapSlot; process() applies it at the start of the next block, smoothed over kLiveRampMs. One producer thread at a time.
  // A plan published mid-stream takes effect at the start of the next process() call, so the sample it starts on depends on the
  // host's block size (inherent to a lock-free hand-over; the ramp itself is sample-accurate).
  // Returns the plan that was published (for the UI / report).
  CalibrationPlan publishCalibration(const ChainCalibration& c);
  // Not RT-safe (call before audio): start with the paths muted, without a ramp.
  void presetMutes(bool a, bool b) noexcept;

  // RT-safe. Forwards `n` values (the block type's liveParams order) to block `blockIndex` (preset
  // order) of path 0 = a / 1 = b. Out-of-range path/index: ignored. Bypass is still handled in process().
  void setBlockLiveParams(int path, int blockIndex, const float* v, int n) noexcept;

  // Measures the alignment with the deterministic probe (see chain.cpp for the exact recipe),
  // at the blend point, gate bypassed, current latency compensation, no alignment delay.
  // Does not change the chain's settings; resets all state afterwards. Needs prepare().
  AlignResult resolveAlignment();

  // Phase 10.1: renders the guitar-shaped probe segment through both paths at the blend point (same
  // tap as alignment, gate bypassed, alignment applied as currently stored, each path's levelDb
  // included, no trim), measures BS.1770 loudness per path and derives the trims and the
  // constant-loudness make-up curve. Pure measurement: does not change the chain's settings except
  // that prepare() then applies the result. prepare() runs it only when both paths are enabled and
  // (levelMatch.mode != off or blendLaw == constantLoudness). Needs prepare(). Not RT-safe. With a path disabled
  // returns a skipped (all-zero) result.
  LevelMatchResult resolveLevelMatch();

  ChainInfo info() const;

  // Gain ladder of path 0 = a / 1 = b: state, and the block (null if none) to hand preloaded rung models to.
  LadderState ladderState(int path) const noexcept;
  LadderBlock* ladderBlock(int path) noexcept;

  // Probe definition (exposed for documentation and tests).
  static constexpr double kProbeSeconds = 1.0;
  static constexpr double kProbeLevelDbfs = -18.0;  // RMS of the white noise before band-passing
  static constexpr double kProbeLowHz = 80.0, kProbeHighHz = 5000.0;
  static constexpr unsigned long long kProbeSeed = 1;
  // Level-match guitar segment (Karplus-Strong palm-mute hits, see chain.cpp).
  static constexpr double kLevelProbeSeconds = 1.5;
  static constexpr double kLevelProbePeakDbfs = -12.0;
  static constexpr unsigned long long kLevelProbeSeed = 2;
  static constexpr int kLevelProbeHits = 8;
  // The sum node runs 6 dB down (exactly x0.5); the output stage gives it back (exactly x2). The bus
  // compressor threshold is referred to the pre-headroom level so presets keep their behaviour.
  static constexpr double kHeadroomDb = -6.020599913279624;

 private:
  struct Path {
    ParametricEq preEq, eq;
    std::vector<LoadedBlock> blocks;
    AmpStage amp;       // GAIN before / tone stack + LEVEL after block `ampBlock`
    int ampBlock = -1;  // ampIndex(path); -1 = no amp controls
    LadderBlock* ladder = nullptr;   // the amp block's processor when its capture has a gain ladder
    std::vector<double> ladderPos;   // GAIN-knob position of each rung
    int ladderOwn = -1, desired = -1, lastCommitted = -1;
    double effKnob = kAmpKnobDefault;  // the GAIN position the ladder math uses
    Gain level;
    std::unique_ptr<Convolver> cab;  // perPath mode only
    DelayLine delay;
    bool enabled = true;
    int latency = 0;
    int compDelay = 0;
    int alignDelay = 0;
  };

  struct Ramp1 {
    double cur = 0.0, target = 0.0, step = 0.0;
    int remaining = 0;  // samples
  };
  struct BandRamp {
    Ramp1 gain, logF, logQ;
    double gNow = 0.0, fNow = 0.0, qNow = 0.0;  // current values while any dimension ramps
    double fTarget = 0.0, qTarget = 0.0;         // linear targets (a finished ramp lands exactly on them)
    bool shape = false;                          // freq or Q ramped since the band became active
    bool active() const noexcept { return gain.remaining > 0 || logF.remaining > 0 || logQ.remaining > 0; }
  };
  struct EqRamps {
    std::array<BandRamp, ParametricEq::kMaxBands> band{};
    int active = 0;  // bands with a running ramp
  };

  void processChunk(const float* in, float* out, int n) noexcept;
  void renderPath(Path& p, float* io, int n, std::uint64_t counter, bool allowRamp) noexcept;
  int driveRung(const Path& p) const noexcept;
  AmpKnobs effectiveAmpKnobs(const Path& p, const AmpKnobs& k) const noexcept;
  void applyAlignment(const AlignResult& r);
  void applyLevelMatch(const LevelMatchResult& r);
  float levelTarget(std::size_t k, double levelDb, bool mute) const noexcept;
  // Effective (headroom, law, make-up, polarity) A / B weights for blend `b`. RT-safe.
  void blendWeights(double b, BlendLaw law, float& wa, float& wb) const noexcept;
  void setBlendTargets(double b, BlendLaw law, bool ramp) noexcept;
  void processEq(ParametricEq& eq, EqRamps& rs, float* w, int n, std::uint64_t counter) noexcept;
  void applyLiveEq(EqRamps& rs, const std::vector<EqBand>& cfg, const LiveEq& oldL,
                   LiveEq& newL) noexcept;
  void resetAll();
  void applyCalibrationPlan(const CalibrationPlan& plan, int rampSamples) noexcept;
  void applyDynamics(const DynamicsSet& d) noexcept;

  Preset preset_;
  ChainResources res_;
  std::array<Path, 2> path_;
  Gain inGain_, outGain_, trimGain_;  // trimGain_: the auto trim, after outGain_
  double autoTrimNowDb_ = 0.0;
  bool trimSet_ = false;
  int trimRampSamples_ = 1;
  Gate gate_;
  std::atomic<float> gateFloorOut_{std::numeric_limits<float>::quiet_NaN()};  // processChunk(): the learned live-gate floor, NaN until learned
  bool gateOn_ = false;
  drift::PeakTap driftTap_;
  std::vector<std::uint8_t> openMask_;  // the gate's open flags for the drift tap (sized in prepare)
  std::unique_ptr<Convolver> cabShared_;
  ParametricEq postEq_;
  BusCompressor comp_;
  bool compOn_ = false;
  // Targets are the effective weights: 0.5 headroom x law x make-up (blendB_ carries the polarity).
  float blendA_ = 0.25f, blendB_ = 0.25f;
  float blendCurA_ = 0.25f, blendCurB_ = 0.25f;  // current values while ramping
  float blendStepA_ = 0.0f, blendStepB_ = 0.0f;
  int blendRamp_ = 0;                          // samples left in the blend ramp
  LiveParams live_;
  CalibrationPlan calPlan_;                       // what setCalibration() applied
  SwapSlot<CalibrationPlan> calSlot_;             // publishCalibration() -> process()
  std::uint64_t calPublished_ = 0, calApplied_ = 0;
  int rampSamples_ = 1;
  EqRamps postRamps_;
  std::array<EqRamps, 2> preRamps_, pathRamps_;
  std::uint64_t eqCounter_ = 0;  // samples processed: the 32-sample EQ redesign grid is absolute
  AlignResult align_;
  LevelMatchResult level_;
  std::array<double, 2> trimDb_{};  // in effect (folded into each path's level gain)
  int latency_ = 0;
  int maxBlock_ = 0;
  bool prepared_ = false;
  std::vector<float> work_, bufA_, bufB_;
  std::vector<std::string> warnings_;       // static, from the preset and resources
  std::vector<std::string> alignWarnings_;  // from the last alignment
  std::vector<std::string> levelWarnings_;   // from the last level match
};

}  // namespace sawblade
