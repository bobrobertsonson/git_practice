#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "sawblade/gate.h"

// v0.8 I3: input-level drift check (docs/specs/v0_8-I3-drift_check.md). A device calibration only holds at the interface gain it was
// made at; if that knob moves, every planned NAM drive is off by the same amount. This unit measures the player's input level and
// says so. It never changes a gain.
//
// Two halves, both JUCE-free:
//  - PeakTap (audio thread): the statistic's source. Per 50 ms window of the DI (before INPUT and before any calibration gain) it takes
//    the peak, and keeps the window only if it was "played": the peak is at least kPlayedAboveFloorDb above the DI's own noise floor,
//    tracked by a dedicated floor follower (Gate::followFloor, minimum statistics, seedable from the I2 learned floor). This does not
//    depend on the preset or its gate, so presets with the gate off, or any gate threshold, give the same statistic. The peak is quantised
//    to 0.25 dB and pushed into a lock-free ring. process() allocates nothing, locks nothing and does no I/O.
//  - DriftTracker (message thread, 10 Hz): reads new windows, keeps the rolling p95 of the last 15 s of played windows, learns the
//    baseline in the first >= 60 s of played audio, and raises / clears the drift notice. Silence adds no windows, so it neither
//    counts toward the 30 s nor resets it.
namespace sawblade::drift {

constexpr double kWindowMs = 50.0;
constexpr double kBinDb = 0.25;
constexpr double kMinDb = -80.0, kMaxDb = 0.0;
constexpr int kBins = static_cast<int>((kMaxDb - kMinDb) / kBinDb);  // 320; bin b covers [kMinDb + b*kBinDb, +kBinDb)
// A window is played when its peak is at least this far above the DI's noise floor. 12 dB: the live gate opens at floor + 10 dB (the
// floor-relative offset of the live dynamics policy), and the peak of a noise-only window sits a few dB above the follower's minimum
// statistic, so 12 dB keeps noise out while any real note (tens of dB above the floor) passes.
constexpr double kPlayedAboveFloorDb = 12.0;
constexpr int kRing = 512;                                           // windows (25.6 s) between two reads before any are lost

// The played test, in one place (the drift tap and the stereo-DI chooser, stereo_input.h, both use it): a dedicated floor follower
// that gates nothing, and the verdict on a window's peak.
void configureFloorFollower(Gate& g) noexcept;
inline bool windowPlayed(double peakLinear, const Gate& floorFollower) noexcept {
  const double db = 20.0 * std::log10(peakLinear > 1e-9 ? peakLinear : 1e-9);
  return db >= floorFollower.floorEstimateDb() + kPlayedAboveFloorDb;
}

double binCenterDb(int bin) noexcept;
int binForDb(double db) noexcept;  // clamped to [0, kBins - 1]

class PeakTap {
 public:
  PeakTap();
  // Off the audio thread, before audio runs. Clears the ring and restarts the floor follower (from the seed).
  void prepare(double sampleRate);
  // Where the DI's floor follower starts (default -70 dBFS; the I2 learned floor, converted to the DI, when there is one). Before prepare().
  void setFloorSeedDb(double db) noexcept { floor_.setFloorSeedDb(db); }
  double floorEstimateDb() const noexcept { return floor_.floorEstimateDb(); }  // audio thread / after the audio stops
  // Any thread. Off (the default): process() does nothing and costs one relaxed load.
  void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

  // Audio thread. `di` is the unprocessed input. Window boundaries are counted in samples and the floor follower is fed up to each
  // boundary, so the result does not depend on the block size.
  void process(const float* di, int n) noexcept;

  // Consumer side (one reader). `cursor` is the count of windows already read from this tap; reads the new ones into out (at most
  // maxOut, oldest first), advances the cursor and returns how many. Windows overwritten before they were read are skipped.
  int read(std::uint32_t& cursor, std::uint16_t* out, int maxOut) const noexcept;
  std::uint32_t written() const noexcept { return written_.load(std::memory_order_acquire); }
  std::uint64_t id() const noexcept { return id_; }  // unique per tap: a consumer restarts its cursor when it sees a new one

 private:
  std::uint64_t id_;
  std::atomic<bool> enabled_{false};
  int winLen_ = 2400;
  Gate floor_;                   // dedicated: follows the DI's noise floor, gates nothing
  int pos_ = 0;                  // audio thread only
  float peak_ = 0.0f;
  std::array<std::atomic<std::uint16_t>, kRing> ring_{};
  std::atomic<std::uint32_t> written_{0};
};

// The drift check's thresholds (also in docs/PLUGIN.md and the I3 spec). Why 5 dB and a 15 s window: a literal >= 6 dB threshold is a coin
// flip for a true 6 dB change (the p95 of a few hundred windows is only good to about +-0.5 dB), and a 30 s window made the notice take
// 40 to 60 s. 5 dB catches a true 6 dB step reliably, and playing dynamics within +-4 dB stay below it.
constexpr double kDriftThresholdDb = 5.0;   // |rolling p95 - baseline| at or above this ...
constexpr double kDriftSustainS = 30.0;     // ... for this much played time raises the notice
constexpr double kDriftRollS = 15.0;        // the rolling p95 window, in played time
constexpr double kDriftLearnS = 60.0;       // played time needed to learn a baseline
constexpr double kDriftMinRollS = 10.0;     // no comparison before the rolling window holds this much
constexpr double kDriftClearBelowDb = 4.0;  // a raised notice clears when the deviation falls below this
constexpr double kDriftIgnoreClearDb = 3.0; // an ignored level is forgotten when the drift returns within this of the baseline
// Re-raise after Ignore: >= kDriftThresholdDb from the ignored level, sustained again.
struct DriftConfig {
  double thresholdDb = kDriftThresholdDb;
  double sustainS = kDriftSustainS;
  double rollS = kDriftRollS;
  double learnS = kDriftLearnS;
  double minRollS = kDriftMinRollS;
  double clearBelowDb = kDriftClearBelowDb;
  double ignoreClearDb = kDriftIgnoreClearDb;
};

struct DriftNotice {
  bool active = false;
  int db = 0;          // whole dB, always >= 0
  bool hotter = true;  // true: the input is hotter than at calibration
};
// "Your playing level is running ~N dB hotter|quieter than when this interface was set up — did the interface gain change?"
std::string driftNoticeText(const DriftNotice& n);

class DriftTracker {
 public:
  explicit DriftTracker(const DriftConfig& cfg = {});

  // The baseline the settings hold (nullopt = none yet: learn one). A value different from the one in force restarts everything
  // (rolling window, sustain, ignore); the same value is a no-op. Call every tick with the device record's value.
  void setBaseline(std::optional<double> p95Dbfs);
  // Forget all measurements and the baseline (calibration off, no record, device re-picked). Keeps nothing.
  void reset();

  // Reads the new windows of `tap` (restarting at 0 when it is a different tap than last time) and advances. Returns the number of
  // windows consumed.
  int consume(const PeakTap& tap);
  // One played window straight in (tests; consume() uses it).
  void addWindow(int bin);

  // Set once when learning completes; the caller persists it and the tracker already holds it, so the settings value coming back
  // through setBaseline() is a no-op.
  std::optional<double> takeNewBaseline();

  // [Ignore]: silence the notice until the baseline changes or the drift moves another thresholdDb from this level.
  void ignore();

  DriftNotice notice() const;
  bool learning() const { return !baseline_.has_value(); }
  double learnedS() const { return learnCount_ * kWindowMs * 0.001; }
  std::optional<double> baselineDb() const { return baseline_; }
  std::optional<double> rollingP95Db() const;  // nullopt until minRollS of played audio
  double driftDb() const { return drift_; }    // rolling p95 - baseline, 0 until both exist
  double sustainedS() const { return sustain_ * kWindowMs * 0.001; }
  double rollingS() const { return static_cast<double>(rollCount_) * kWindowMs * 0.001; }
  bool ignored() const { return ignored_.has_value(); }

 private:
  void evaluate();
  double p95Of(const std::array<int, kBins>& h, int total) const;

  DriftConfig cfg_;
  int rollMax_, learnNeed_, sustainNeed_, rollMin_;
  std::array<std::uint16_t, 2048> rollBins_{};  // ring of the last rollMax_ windows
  int rollHead_ = 0, rollCount_ = 0;
  std::array<int, kBins> rollHist_{};
  std::array<int, kBins> learnHist_{};
  int learnCount_ = 0;
  std::optional<double> baseline_;
  std::optional<double> newBaseline_;
  double drift_ = 0.0;
  int sustain_ = 0;
  bool active_ = false;
  std::optional<double> ignored_;  // the drift (dB re baseline) that was ignored
  std::uint64_t tapId_ = 0;
  std::uint32_t cursor_ = 0;
};

}  // namespace sawblade::drift
