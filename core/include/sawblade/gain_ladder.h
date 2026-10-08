#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "sawblade/preset.h"
#include "sawblade/processor.h"
#include "sawblade/swap_slot.h"

// v0.2 Task B: gain ladders. A TONE3000 tone that is the same amp at several gain settings gives an
// ordered ladder of rungs (Capture::ladder, ascending gain). The path's GAIN knob picks the rung nearest
// its position and drives the amp block only for the remainder (docs/PRESET_SCHEMA.md "Gain ladder").
namespace sawblade {

class CaptureCache;

constexpr int kMaxLadderRungs = 64;     // schema limit
constexpr int kMaxLoadedRungs = 8;      // models kept loaded at once (the 8 nearest the active rung)
constexpr double kLadderHysteresis = 0.15;  // knob units past the midpoint before the rung switches
constexpr double kLadderFadeMs = 20.0;      // equal-power crossfade length
constexpr double kLadderWarmMs = 10.0;      // the incoming model runs (output discarded) this long before the fade starts

// Knob positions (0..10) of the rungs, in the ladder's order: p_i = 10 (g_i - g_min) / (g_max - g_min).
std::vector<double> ladderPositions(const std::vector<LadderRung>& ladder);
// Index of the rung whose modelId equals `id`, or -1.
int rungIndexOfModel(const std::vector<LadderRung>& ladder, const std::string& id);
// The rung that is the block's own capture (capture.source->modelId), or -1.
int ownRungIndex(const Capture& c);
// Nearest rung with hysteresis: the rung changes only once the knob is >= kLadderHysteresis past the midpoint
// between two neighbouring rungs. `positions` ascending; `current` is clamped into range.
int selectRung(const std::vector<double>& positions, int current, double knob);
// Residual drive (dB) of the GAIN knob `knob` on a rung at `position`: (knob - position) * 2.4, clamped to +-12.
double ladderResidualDb(double knob, double position);
// The Capture of rung `r` of `own`'s tone (the cache location is derived from source.id / modelId).
Capture rungCapture(const Capture& own, const LadderRung& r);
// Where the rung's model is in the capture cache; nullopt if it is not cached (or the ids are not plain tokens).
std::optional<std::filesystem::path> locateRungFile(const Capture& own, const LadderRung& r);

// Builds and prepares the processor of rung `rung` of a nam block (same input / output gain and loudness
// settings as the block). Load-time / background thread only. Null (and *why set) if the rung is not cached or
// fails to load or prepare.
std::unique_ptr<Processor> buildRungProcessor(const NamBlockParams& block, int rung, const ProcessSpec& spec,
                                              CaptureCache* cache, std::string* why = nullptr);

// A nam block whose capture has a gain ladder: the active rung's model runs; another rung that is loaded can
// be switched to: the incoming model first runs on the signal for kLadderWarmMs (output discarded, so its state is
// warm), then an equal-power crossfade of kLadderFadeMs (both models run). Replaces the plain NamBlock
// in the Chain for such blocks; everything else (live gains, prepare, reset) reaches every loaded model.
//
// Threading: setTargetRung() and process() are audio-thread / RT-safe. publishRungs() / flushRungs() may be
// called from one background thread at a time (a mutex serialises them); they hand prepared processors to the
// audio thread through a SwapSlot (lock-free), and models the audio thread has replaced are destroyed on the
// background thread (collectGarbage), never on the audio thread. A rung that is not loaded yet leaves the block
// on its current rung ("pending"). A rung whose latency differs from the block's is rejected and never runs.
class LadderBlock : public Processor {
 public:
  struct Entry {
    int rung = -1;
    std::unique_ptr<Processor> proc;  // null: evict the rung's model (ignored for the active / fading rungs)
  };

  // `active` is the model of rung `activeRung`; its latency is the block's.
  LadderBlock(int rungCount, int activeRung, std::unique_ptr<Processor> active);
  ~LadderBlock() override;

  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int n) noexcept override;
  int latencySamples() const noexcept override { return latency_; }
  bool setLiveGainsDb(double inDb, double outDb, int rampSamples) noexcept override;
  bool setLiveGainsDb(double inDb, double outDb, double makeupDb, int rampSamples) noexcept override;
  // The level info the Chain plans with is the block's starting rung (immutable, any thread). setCalibration reaches every
  // loaded rung (and any rung loaded later); each rung computes its own planned gain from its own capture metadata, so a
  // rung swap changes the drive by the metadata difference (v0.8 I1, decision 4). A downstream NAM block is planned from the
  // starting rung's output level and does not follow a rung swap (documented limit; an amp ladder is normally the last block).
  calibration::BlockLevelInfo levelInfo() const noexcept override { return levelInfo_; }
  void setCalibration(const calibration::BlockCalibration& c, int rampSamples) noexcept override;

  // RT-safe. Which rung should sound. If it is not loaded, the block stays where it is and pending() is true.
  void setTargetRung(int rung) noexcept;
  int targetRung() const noexcept { return target_.load(std::memory_order_relaxed); }
  int activeRung() const noexcept { return active_.load(std::memory_order_relaxed); }
  // The rung the audio is on, or moving to once a fade has started.
  int committedRung() const noexcept { return committed_.load(std::memory_order_relaxed); }
  bool pending() const noexcept { return target_.load(std::memory_order_relaxed) != committed_.load(std::memory_order_relaxed); }
  int rungCount() const noexcept { return rungCount_; }
  std::uint64_t loadedMask() const noexcept { return loaded_.load(std::memory_order_acquire); }    // bit i: rung i's model is loaded
  std::uint64_t rejectedMask() const noexcept { return rejected_.load(std::memory_order_acquire); }  // bit i: latency mismatch

  // Background thread. Entries are staged and handed over when the audio thread has taken the previous batch;
  // returns whether everything staged has been handed over. Rungs < 64 only.
  bool publishRungs(std::vector<Entry> entries);
  bool flushRungs();  // retry handing over what is still staged
  // Rungs whose models are loaded, staged, or in a batch the audio thread has not taken yet (the producer's view). A model the
  // audio thread dropped (rejected for its latency, or evicted) is not in it.
  std::uint64_t knownMask() const;

 private:
  struct Batch {
    std::uint64_t seq = 0;
    std::vector<Entry> entries;
  };
  void drain() noexcept;
  Processor* slot(int rung) const noexcept { return rung >= 0 && rung < rungCount_ ? slots_[static_cast<std::size_t>(rung)].get() : nullptr; }

  int rungCount_;
  int latency_ = 0;
  std::vector<std::unique_ptr<Processor>> slots_;  // one per rung; audio thread owns the contents
  std::atomic<int> active_, committed_, target_;
  std::atomic<std::uint64_t> loaded_{0}, rejected_{0};
  bool warming_ = false, fading_ = false;
  int fadeTo_ = -1, fadePos_ = 0, fadeLen_ = 1, warmLen_ = 1;
  std::vector<float> fadeOld_, fadeNew_;  // equal-power weights, fadeLen_ + 1 entries
  std::vector<float> scratch_;
  int maxBlock_ = 0;
  calibration::BlockLevelInfo levelInfo_{};
  bool haveCal_ = false;
  calibration::BlockCalibration cal_{};
  int calRamp_ = 1;
  bool haveGains_ = false;
  double gainIn_ = 0.0, gainOut_ = 0.0, gainMakeup_ = 0.0;
  int gainRamp_ = 1;
  std::uint64_t seenSeq_ = 0;
  std::atomic<std::uint64_t> consumedSeq_{0};

  SwapSlot<Batch> slot_;
  std::mutex producerMutex_;
  std::vector<Entry> staged_;
  std::uint64_t publishedSeq_ = 0;
  std::uint64_t inflightMask_ = 0;  // rungs (with models) in the batch the audio thread has not taken yet
};

}  // namespace sawblade
