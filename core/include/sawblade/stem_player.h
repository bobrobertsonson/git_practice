#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "sawblade/delay.h"
#include "sawblade/processor.h"
#include "sawblade/stem_set.h"
#include "sawblade/swap_slot.h"

// Play-along backing: plays a StemSet (stem_set.h) as a stereo mix, with transport, A-B loop,
// count-in, per-stem mix and rig-latency compensation. See docs/specs/phase5_1_stemplayer.md.
namespace sawblade {

enum class GuitarMode { Muted, Ghost, Full };
enum class TransportMode { FreeRun, HostFollow };

constexpr double kTransportFadeMs = 5.0;   // play / pause gain ramp
constexpr double kSeekFadeMs = 5.0;        // equal-power crossfade of a seek
constexpr double kLoopFadeMs = 10.0;       // equal-power crossfade of a loop wrap
constexpr double kMixRampMs = 20.0;        // stem gain / master level ramps
constexpr double kGhostGuideDb = -12.0;    // guitar stem level in GuitarMode::Ghost
constexpr double kClickMs = 30.0;          // count-in click length
constexpr double kClickDecayMs = 5.0;
constexpr double kClickAccentHz = 1500.0;
constexpr double kClickNormalHz = 1000.0;
constexpr double kDefaultClickLevelDb = -6.0;
constexpr std::int64_t kDefaultHostJumpThreshold = 64;  // samples

// Threading:
//  * producer thread (message / loader): prepare(), reset(), setStemSet(), collectGarbage();
//  * audio thread: process() and every transport / mix / host / query member below. They are
//    RT-safe (no allocation, locks, I/O, exceptions) and must be called on the audio thread, or
//    otherwise serialised with process(). Setters take effect at the start of the next process()
//    call; that block boundary is the sample they act on. When several commands arrive in one
//    block, the last play()/pause() wins and seek() is applied before it.
// A thread-safe command queue for the plugin is a later phase.
//
// Ramps (transport, stem gains, master) follow the same linear law as Gain::rampToLinear (the first
// ramped sample already moves one step; the last equals the target) but are double-precision and
// counter-based (value = start + (target - start) * k / N), so the result is exact to ~1e-12 and
// does not depend on block size.
// Start offset (setStartOffsetSamples): the player's playhead p lives in "playhead time". Playhead time p
// plays stem sample (p - offset). Negative offset: the stems lead (stem audio from |offset| plays at
// p = 0). Positive offset: silence for the first `offset` samples, then the stems from sample 0. Seek,
// loop points, position() and playheadLength() are all in playhead time; the host-follow position is
// too (a host sample of p plays stem sample p - offset, so a positive offset is "the song starts
// `offset` samples into the host timeline"). The playhead runs over [0, playheadLength()] with
// playheadLength() = max(0, stemSetLength() + offset). The offset is set on the producer thread and
// takes effect at the first block start where the transport is fully stopped (or no set is adopted
// yet), like a replacement stem set; the position stays where it is (clamped in free-run mode), a
// loop that no longer fits is cleared. (tonerender --backing-offset-ms uses the matcher's opposite
// sign: see cli/main.cpp.)
// Loop: a wrap happens when the playhead steps from b - 1 to b. A seek to exactly b (or beyond)
// with a loop active therefore does not wrap; it plays on, like a playhead already at or beyond b
// when the loop is set.
class StemPlayer {
 public:
  StemPlayer();
  ~StemPlayer();
  StemPlayer(const StemPlayer&) = delete;
  StemPlayer& operator=(const StemPlayer&) = delete;

  // ---- producer thread ---------------------------------------------------------------------
  // Allocates everything: the two rig-latency delay lines (up to maxRigLatencySamples), click
  // buffers and crossfade tables. Resets the transport. A previously adopted set whose rate
  // differs from spec.sampleRate is dropped. Throws std::invalid_argument on bad arguments.
  void prepare(const ProcessSpec& spec, int maxRigLatencySamples);
  void reset();  // transport, ramps, delays and clicks back to the just-prepared state; settings kept

  // Publishes a set (lock-free, via SwapSlot). Throws std::invalid_argument if `set` is null or its
  // rate differs from the prepared rate. The audio thread adopts it only at the start of a block in
  // which the transport is fully stopped (paused, pause fade complete, not counting in); adoption
  // resets the playhead to 0 and clears the loop. Exception: when no set has been adopted yet, the
  // first set is adopted at the start of any block, keeps the playhead where the transport is
  // (outside [0, length] it just reads silence) and clears the loop and pending crossfade state. Retired sets are freed by collectGarbage().
  void setStemSet(std::unique_ptr<StemSet> set);
  void collectGarbage() noexcept { slot_.collectGarbage(); }
  // Producer thread (any thread, relaxed atomic): the start offset above, in samples at the set's rate.
  void setStartOffsetSamples(std::int64_t offset) noexcept { offsetReq_.store(offset, std::memory_order_relaxed); }
  std::int64_t startOffsetSamples() const noexcept { return offsetReq_.load(std::memory_order_relaxed); }

  // ---- audio thread ------------------------------------------------------------------------
  // Overwrites both outputs with the backing mix + count-in clicks, delayed by the rig latency.
  // Any n >= 0 is accepted (n == 0 only applies pending commands).
  void process(float* outL, float* outR, int n) noexcept;

  // Transport (free-run mode; ignored in host-follow mode).
  void play() noexcept;
  void pause() noexcept;
  void seek(std::int64_t pos) noexcept;  // clamped to [0, length]; see the spec for the crossfade
  bool setLoop(std::int64_t a, std::int64_t b) noexcept;  // false (nothing changes) if invalid
  void clearLoop() noexcept;
  void setCountIn(int bars, double bpm, int beatsPerBar = 4) noexcept;  // clamped; bars 0 = off
  void setClickLevelDb(double db) noexcept;  // -60..0, default -6; rewrites the click buffers in place

  // Queries.
  std::int64_t position() const noexcept { return pos_; }  // playhead time (see above), at the set's rate
  bool isPlaying() const noexcept { return wantPlay_; }     // requested state (true during count-in)
  bool isCountingIn() const noexcept { return countingIn_; }
  bool atEnd() const noexcept { return set_ != nullptr && pos_ >= len_; }
  bool hasStemSet() const noexcept { return set_ != nullptr; }
  const StemSet* adoptedSet() const noexcept { return set_; }  // identity only (audio thread); null if none
  std::int64_t stemSetLength() const noexcept { return rawLen_; }  // of the adopted set; 0 if none
  std::int64_t playheadLength() const noexcept { return len_; }    // max(0, length + applied offset); 0 if none
  std::int64_t appliedStartOffsetSamples() const noexcept { return off_; }
  bool loopActive() const noexcept { return loopActive_; }
  std::int64_t loopStart() const noexcept { return loopA_; }
  std::int64_t loopEnd() const noexcept { return loopB_; }
  const std::vector<float>& accentClick() const noexcept { return accent_; }
  const std::vector<float>& normalClick() const noexcept { return normal_; }

  // Mix.
  void setStemGainDb(StemKind k, double db) noexcept;  // -60..+12
  void setStemMute(StemKind k, bool mute) noexcept;
  void setStemSolo(StemKind k, bool solo) noexcept;
  void setMasterLevelDb(double db) noexcept;  // -60..+12
  void setGuitarMode(GuitarMode m) noexcept;  // default Muted

  // Latency compensation: the whole output is delayed by this many samples so it lines up with a
  // Chain reporting Chain::latencySamples(). Clamped to [0, maxRigLatencySamples]. Changing it
  // while playing is not click-free. latencySamples() stays 0 (the delay is deliberate).
  void setRigLatencySamples(int samples) noexcept;
  int rigLatencySamples() const noexcept { return latencyReq_; }
  int latencySamples() const noexcept { return 0; }

  // Host-follow mode: call setHostPosition() before every process() (hostSample = host position
  // of the block's first sample, in samples at the set's rate; may be negative for pre-roll).
  void setTransportMode(TransportMode m) noexcept { mode_ = m; }
  TransportMode transportMode() const noexcept { return mode_; }
  void setHostPosition(std::int64_t hostSample, bool hostPlaying) noexcept {
    hostSample_ = hostSample;
    hostPlaying_ = hostPlaying;
  }
  void setHostJumpThresholdSamples(std::int64_t t) noexcept { hostThreshold_ = t < 0 ? 0 : t; }

 private:
  // Linear ramp from an integer counter (deterministic for any chunking).
  struct Ramp {
    double start = 0.0, target = 0.0, cur = 0.0;
    int total = 0, k = 0;
    void snap(double v) noexcept { start = target = cur = v; total = k = 0; }
    void begin(double t, int n) noexcept {
      if (n <= 0) {
        snap(t);
        return;
      }
      start = cur;
      target = t;
      total = n;
      k = 0;
    }
    bool active() const noexcept { return k < total; }
    double next() noexcept {
      if (k < total) {
        ++k;
        cur = k == total ? target : start + (target - start) * static_cast<double>(k) / static_cast<double>(total);
      }
      return cur;
    }
    void skip(int n) noexcept {
      if (k >= total) return;
      k = n >= total - k ? total : k + n;
      cur = k == total ? target : start + (target - start) * static_cast<double>(k) / static_cast<double>(total);
    }
  };
  enum class PlayCmd { None, Play, Pause };

  void applyCommands() noexcept;
  void applyHostFollow() noexcept;
  void adoptIfStopped() noexcept;
  void applyOffset() noexcept;
  void updateMixTargets() noexcept;
  void startPlaying() noexcept;
  void startPause() noexcept;
  void startCountIn() noexcept;
  void stepSample(float& outL, float& outR) noexcept;
  void doSeek(std::int64_t target) noexcept;
  void doWrap() noexcept;
  void startCrossfade(std::int64_t newPos, int n, const float* in, const float* out) noexcept;
  void stopTransport() noexcept;
  void startClick(bool accent) noexcept;
  std::int64_t beatStart(std::int64_t k) const noexcept;
  void rebuildClicks() noexcept;
  void snapMix() noexcept;

  // Set / slot.
  SwapSlot<StemSet> slot_;
  const StemSet* set_ = nullptr;
  std::int64_t rawLen_ = 0;  // length of the adopted set
  std::int64_t len_ = 0;     // playhead length: max(0, rawLen_ + off_)
  std::int64_t off_ = 0;     // applied start offset
  std::atomic<std::int64_t> offsetReq_{0};
  std::array<const float*, kStemKindCount * 2> ch_{};  // [kind * 2 + channel]
  std::array<int, kStemKindCount> active_{};
  int nActive_ = 0;

  // Prepared configuration.
  double fs_ = 0.0;
  int maxBlock_ = 0, maxLatency_ = 0;
  int nTrans_ = 0, nSeek_ = 0, nLoop_ = 0, nMix_ = 0;
  std::vector<float> seekIn_, seekOut_, loopIn_, loopOut_;
  std::vector<double> accentUnit_, normalUnit_;
  std::vector<float> accent_, normal_;
  double clickLevelDb_ = kDefaultClickLevelDb;
  DelayLine delayL_, delayR_;
  int latencyReq_ = 0;

  // Mix.
  std::array<double, kStemKindCount> gainDb_{};
  std::array<bool, kStemKindCount> mute_{}, solo_{};
  GuitarMode guitarMode_ = GuitarMode::Muted;
  double masterDb_ = 0.0;
  std::array<Ramp, kStemKindCount> stemRamp_;
  Ramp masterRamp_;
  std::array<double, kStemKindCount> g_{};  // per-sample gains
  double master_ = 1.0;

  // Transport.
  PlayCmd playCmd_ = PlayCmd::None;
  bool seekCmd_ = false;
  std::int64_t seekCmdPos_ = 0;
  bool wantPlay_ = false;
  bool advancing_ = false;  // playing or fading out after pause()
  bool tgOn_ = false;       // transport gain target is 1
  Ramp tg_;
  std::int64_t pos_ = 0, oldPos_ = 0;
  bool xfading_ = false;
  int xfK_ = 0, xfN_ = 0;
  const float* xfIn_ = nullptr;
  const float* xfOut_ = nullptr;
  bool pendingSeek_ = false, pendingSeekFollow_ = false;
  std::int64_t pendingSeekPos_ = 0, pendingSeekAge_ = 0;
  bool wrapPending_ = false;
  bool loopActive_ = false;
  std::int64_t loopA_ = 0, loopB_ = 0;

  // Count-in.
  int ciBars_ = 0, ciBpb_ = 4;
  double ciBpm_ = 120.0;
  bool countingIn_ = false;
  std::int64_t ciPos_ = 0, ciTotal_ = 0, ciBeat_ = 0, ciBeats_ = 0, ciNextAt_ = 0;
  double ciBpmActive_ = 120.0;
  int ciBpbActive_ = 4;
  const std::vector<float>* clickBuf_ = nullptr;
  int clickIdx_ = 0, clickLeft_ = 0;

  // Host-follow.
  TransportMode mode_ = TransportMode::FreeRun;
  std::int64_t hostSample_ = 0;
  bool hostPlaying_ = false;
  std::int64_t hostThreshold_ = kDefaultHostJumpThreshold;
};

}  // namespace sawblade
