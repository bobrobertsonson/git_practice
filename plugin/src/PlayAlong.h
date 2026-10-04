#pragma once

// Play-along backing for the plugin (docs/specs/phase5_2_playalong_plugin.md): a StemPlayer
// (core/stem_player.h) plus everything that lets the message thread, a loader thread and the audio
// thread share it. JUCE-free.
//
// Threading
//   audio thread    process(): drains the command queue, runs the StemPlayer, measures the rig's output
//                   loudness, publishes a Snapshot (atomics). Never allocates, locks, does I/O or throws.
//   loader thread   one worker: loads a folder of stems (loadStemDirectory), hands the StemSet to the
//                   player through its SwapSlot (lock-free) and frees retired sets (collectGarbage). It
//                   is separate from the plugin's EngineLoader on purpose: a long song load must not
//                   hold up preset builds or prepareToPlay.
//   other threads   (message thread, host state restore, tests): every setter below. Setters update the
//                   saved settings under a mutex and push a command into the SPSC queue. The queue's
//                   producer side is serialised by a mutex that the audio thread never takes. Nothing
//                   the audio thread touches is ever locked by it.
// The StemPlayer's own setters are audio-thread-only (core/stem_player.h); only setStartOffsetSamples,
// setStemSet, collectGarbage and prepare/reset are used from other threads, as that header allows.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "sawblade/loudness.h"
#include "sawblade/stem_player.h"
#include "sawblade/stem_set.h"

namespace sawblade::plugin {

constexpr double kBackingLevelMinDb = -40.0;   // control range of the backing level
constexpr double kBackingLevelMaxDb = 6.0;
constexpr double kBackingLevelDefaultDb = 0.0;
constexpr double kRigReferenceLufs = -18.0;    // assumed rig loudness until a rig signal has been observed
constexpr double kOffsetLimitMs = 10000.0;     // control range of the offset
constexpr double kDefaultBpm = 120.0;

// Everything the play-along panel saves (plugin state `playAlong`, docs/PRESET_SCHEMA.md). UI state: not tone.
struct PlayAlongSettings {
  std::string folder;                       // folder of stems ("" = none chosen)
  double offsetMs = 0.0;                    // where the DI/playhead start lies inside the song (matcher sign, see below)
  bool loopOn = false;
  double loopAMs = -1.0, loopBMs = -1.0;    // loop points in playhead time; < 0 = unset
  bool countIn = false;
  double bpm = kDefaultBpm;
  GuitarMode guitarMode = GuitarMode::Muted;
  double levelDb = kBackingLevelDefaultDb;  // backing level
  bool keepOther = false;                   // OtherRole::Other ("keep keys") instead of OtherRole::Guitar
  bool hostSync = false;                    // plugin only: follow the host transport

  bool operator==(const PlayAlongSettings&) const = default;
  bool isDefault() const { return *this == PlayAlongSettings{}; }
};
// offsetMs follows the matcher's `--offset-ms` and `tonerender --backing-offset-ms`: where the DI starts
// inside the song. Positive: the stems lead (stem audio from offsetMs plays at playhead 0). The
// StemPlayer's own offset is the opposite sign (core/stem_player.h): player offset = -offset.

nlohmann::json playAlongToJson(const PlayAlongSettings& s);
// Tolerant: never throws; wrong-typed or missing fields keep their defaults, numbers are clamped.
PlayAlongSettings playAlongFromJson(const nlohmann::json& j);

// The one-time suggested backing level: rig loudness minus the song's backing loudness (so the backing
// sits at the rig's loudness), clamped to the control range. `rig` none (no rig signal observed yet)
// uses kRigReferenceLufs. `backing` none (silent / no backing stems) gives the default level.
double suggestedBackingLevelDb(std::optional<double> rigLufs, std::optional<double> backingLufs);

// Cheap running estimate of the rig output's loudness (K-weighted, 100 ms hops, BS.1770 absolute gate
// -70 LUFS and a -10 LU relative gate against the running gated mean, leaky with a 30 s time constant).
// The rig output is dual mono (both output channels carry it), so a hop's loudness is that of two equal
// channels. process() is real-time safe; lufs() may be read from any thread.
class RigLoudness {
 public:
  static constexpr double kHopSeconds = 0.1, kMemorySeconds = 30.0, kMinGatedSeconds = 0.5;
  void prepare(double sampleRate);
  void process(const float* x, int n) noexcept;
  std::optional<double> lufs() const noexcept;

 private:
  struct Bq {
    LoudnessBiquad c;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double step(double x) noexcept {
      const double y = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
      x2 = x1;
      x1 = x;
      y2 = y1;
      y1 = y;
      return y;
    }
  };
  Bq shelf_, hp_;
  int hopN_ = 4800, hopPos_ = 0;
  double acc_ = 0.0, decay_ = 1.0;
  double absE_ = 0.0, absW_ = 0.0, relE_ = 0.0, relW_ = 0.0;
  std::atomic<double> est_{std::numeric_limits<double>::quiet_NaN()};
};

// A command from a non-audio thread to the audio thread.
struct PlayAlongCmd {
  enum class Type : std::uint8_t { Play, Pause, Seek, SetLoop, ClearLoop, CountIn, GuitarMode, Level, HostSync };
  Type type = Type::Pause;
  std::int64_t a = 0, b = 0;
  double d = 0.0;
};

// Preallocated lock-free single-producer / single-consumer ring (the caller serialises producers).
class PlayAlongQueue {
 public:
  static constexpr std::size_t kCapacity = 256;  // power of two
  bool push(const PlayAlongCmd& c) noexcept {    // false when full (nothing is stored)
    const auto t = tail_.load(std::memory_order_relaxed);
    if (t - head_.load(std::memory_order_acquire) >= kCapacity) return false;
    buf_[t & (kCapacity - 1)] = c;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  bool pop(PlayAlongCmd& c) noexcept {
    const auto h = head_.load(std::memory_order_relaxed);
    if (h == tail_.load(std::memory_order_acquire)) return false;
    c = buf_[h & (kCapacity - 1)];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

 private:
  std::array<PlayAlongCmd, kCapacity> buf_{};
  std::atomic<std::size_t> head_{0}, tail_{0};
};

class PlayAlong {
 public:
  struct Snapshot {  // published by the audio thread at the end of every block
    bool hasSet = false, playing = false, countingIn = false, atEnd = false, loopActive = false;
    bool following = false;       // plugin mode with host sync on: the host owns the transport
    std::int64_t position = 0;    // playhead time, samples
    std::int64_t length = 0;      // playhead length, samples
    std::int64_t loopStart = 0, loopEnd = 0;
    double sampleRate = 0.0;
  };
  struct LoadStatus {
    enum class State { None, Loading, Ready, Failed };
    State state = State::None;
    std::string message;                      // why it failed
    std::string songName;                     // folder name of the loaded song
    std::vector<std::string> warnings;        // from the loader
    std::optional<double> backingLufs;
    std::optional<double> suggestedLevelDb;   // what the last user load suggested (and applied)
    double lengthSeconds = 0.0;
    bool otherMappedToGuitar = false, hasGuitarStem = false;
  };
  struct HostTransport {
    bool playing = false;
    std::int64_t sample = 0;  // host position of the block's first sample
  };

  PlayAlong();
  ~PlayAlong();
  PlayAlong(const PlayAlong&) = delete;
  PlayAlong& operator=(const PlayAlong&) = delete;

  // ---- non-audio threads ---------------------------------------------------------------------
  // Standalone: free-run, backing always enabled. Plugin: follows the host transport once hostSync is on.
  void setStandalone(bool standalone) noexcept { standalone_.store(standalone, std::memory_order_relaxed); }
  bool standalone() const noexcept { return standalone_.load(std::memory_order_relaxed); }
  // Allocates (prepareToPlay). maxRigLatencySamples bounds setRigLatencySamples().
  void prepare(double sampleRate, int maxBlock, int maxRigLatencySamples);
  // Latency of the rig, host samples: the backing is delayed by it. Atomic; the audio thread applies it.
  void setRigLatencySamples(int samples) noexcept { rigLatency_.store(samples, std::memory_order_relaxed); }

  // Starts a background load of `folder` (never throws; failures show in loadStatus()). `userInitiated`
  // loads also set the backing level once from the loudness rule (suggestedBackingLevelDb); state
  // restores and reloads never touch the level.
  void loadFolder(const std::string& folder, bool userInitiated);
  // Applies saved settings (state restore): never changes the level automatically, loads the folder in
  // the background if there is one.
  void restore(const PlayAlongSettings& s);

  void play();
  void pause();
  void seekSamples(std::int64_t playheadSample);
  void setLoopMs(double aMs, double bMs, bool on);
  void setCountIn(bool on, double bpm);
  void setGuitarMode(GuitarMode m);
  void setLevelDb(double db);
  void setOffsetMs(double ms);
  void setHostSync(bool on);
  void setKeepOther(bool keep);  // reloads the folder with the other role

  PlayAlongSettings settings() const;
  LoadStatus loadStatus() const;
  Snapshot snapshot() const noexcept;
  std::optional<double> rigLoudnessLufs() const noexcept { return loud_.lufs(); }
  bool waitForLoader(std::chrono::milliseconds timeout = std::chrono::milliseconds(60000));
  std::uint64_t commandsDropped() const noexcept { return dropped_.load(); }

  // ---- audio thread --------------------------------------------------------------------------
  // `rig` is the rig's mono output for these n samples (read only: measured for the loudness estimate).
  // bl / br are overwritten with the backing for the same n samples. host.sample is the host position
  // of the first sample (ignored in Standalone mode).
  void process(const float* rig, float* bl, float* br, int n, const HostTransport& host) noexcept;

 private:
  struct Request {
    std::string folder;
    double rate = 0.0;
    OtherRole role = OtherRole::Guitar;
    bool user = false;
    std::uint64_t id = 0;
  };

  void push(const PlayAlongCmd& c);
  void applyAll();                         // pushes the whole settings state
  void requestLoad(bool user);
  void loaderMain();
  void runLoad(const Request& r);
  void applyCommand(const PlayAlongCmd& c) noexcept;
  void applyOffset(double ms);
  void pushLoop(const PlayAlongSettings& s, double rate);

  StemPlayer player_;
  RigLoudness loud_;
  std::atomic<bool> standalone_{false};
  std::atomic<double> rate_{0.0};
  std::atomic<double> loadedRate_{0.0};
  std::atomic<int> rigLatency_{0};
  std::atomic<std::uint64_t> dropped_{0};

  mutable std::mutex m_;  // settings_, status_
  PlayAlongSettings settings_;
  LoadStatus status_;

  std::mutex producerM_;  // serialises queue producers (never taken by the audio thread)
  PlayAlongQueue queue_;
  std::mutex prepareM_;   // prepare() vs the loader's setStemSet (never taken by the audio thread)

  // Loader worker.
  std::mutex loaderM_;
  std::condition_variable loaderCv_, idleCv_;
  std::optional<Request> pending_;
  std::uint64_t requestId_ = 0;
  bool busy_ = false, stop_ = false, loadWanted_ = false, wantedUser_ = false;

  // Audio-thread state.
  int appliedLatency_ = 0;
  bool wantLoop_ = false, hostSync_ = false, followMode_ = false;
  std::int64_t loopA_ = 0, loopB_ = 0;
  std::atomic<bool> sHasSet_{false}, sPlaying_{false}, sCounting_{false}, sAtEnd_{false}, sLoop_{false}, sFollowing_{false};
  std::atomic<std::int64_t> sPos_{0}, sLen_{0}, sLoopA_{0}, sLoopB_{0};

  std::thread thread_;  // last
};

}  // namespace sawblade::plugin
