#pragma once

// DI take recorder (docs/specs/phase6a_record_match_plugin.md). JUCE-free.
//
// It records the plugin's INPUT (the clean DI, before the gate; a stereo input is summed to mono exactly
// like the rig does) into a preallocated lock-free ring that a writer thread drains into a 32-bit float
// mono WAV in the takes directory, with a JSON sidecar next to it (docs/PLUGIN.md "Record + Match").
//
// Threading
//   audio thread   process(): pushes the block into the ring, and Start / Gap / End events into a small
//                  preallocated event queue. Atomics only: no allocation, locks, I/O or exceptions. A full
//                  ring never blocks: the block is dropped, counted (overruns) and the writer later pads
//                  the file with the same number of zeros, so the take stays on the DI timeline.
//   writer thread  drains the events and the ring, owns the file, writes the sidecar when the take ends.
//   message thread start() / stop() / list / rename / remove. start() hands the writer the take's file name
//                  and song folder through a mutex that the audio thread never takes.
// Events carry the ring position they happened at, so the writer processes data and events in order even
// if a whole take (start, stop) fits between two writer wake-ups.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sawblade::plugin {

// What the audio thread knows about the song at the take's first sample (see PlayAlong::takeStartInfo).
struct TakeStartInfo {
  bool hasSong = false;             // a stem set is loaded
  bool running = false;             // the backing is playing (not paused, not counting in)
  std::int64_t stemSampleIndex = 0; // the stem sample played at the take's first sample (player offset applied)
  double stemSampleRate = 0.0;
};

// One finished take on disk (WAV + sidecar).
struct TakeInfo {
  std::string name;                 // file stem
  std::filesystem::path wav, json;
  double sampleRate = 0.0;
  std::int64_t lengthSamples = 0;
  int channels = 1;
  std::uint64_t overruns = 0;       // blocks the ring could not take (filled with silence in the file)
  std::uint64_t droppedSamples = 0;
  bool hasPlayAlong = false;        // sidecar `playAlong` is not null
  bool running = false;
  std::int64_t stemSampleIndex = 0;
  double stemSampleRate = 0.0;
  std::string songFolder;
  std::string createdUtc;

  double lengthSeconds() const { return sampleRate > 0.0 ? static_cast<double>(lengthSamples) / sampleRate : 0.0; }
  // The matcher's --offset-ms (where the DI starts inside the song); none when no song was playing.
  std::optional<double> offsetMs() const {
    if (!hasPlayAlong || !running || stemSampleRate <= 0.0) return std::nullopt;
    return 1000.0 * static_cast<double>(stemSampleIndex) / stemSampleRate;
  }
};

std::optional<TakeInfo> readTakeSidecar(const std::filesystem::path& json);

class TakeRecorder {
 public:
  enum class State { Idle, Armed, Recording, Finalizing };

  static constexpr double kMinRingSeconds = 2.0;

  TakeRecorder();
  ~TakeRecorder();
  TakeRecorder(const TakeRecorder&) = delete;
  TakeRecorder& operator=(const TakeRecorder&) = delete;

  // ---- non-audio threads ------------------------------------------------------------------------------
  // Allocates the ring (>= kMinRingSeconds at `sampleRate`, power of two). Not concurrent with process().
  // A take in progress is ended first.
  void prepare(double sampleRate);
  void setTakesDir(const std::filesystem::path& dir);
  std::filesystem::path takesDir() const;

  // Requests a take: it begins at the start of the next audio block. `songFolder` goes into the sidecar.
  // False (and lastError() says why) if not prepared, already armed / recording, or the previous take is
  // still being finalised.
  bool start(const std::string& songFolder);
  // Ends the take at the next audio block boundary; the writer then finalises the files.
  void stop();
  State state() const noexcept;
  std::uint64_t recordedSamples() const noexcept { return recorded_.load(std::memory_order_relaxed); }  // current / last take
  std::uint64_t overruns() const noexcept { return overruns_.load(std::memory_order_relaxed); }         // blocks dropped, this take
  std::uint64_t droppedSamples() const noexcept { return droppedSamples_.load(std::memory_order_relaxed); }
  double sampleRate() const noexcept { return rate_.load(std::memory_order_relaxed); }
  std::string lastError() const;
  // Bumped whenever the set of takes on disk changed because of this recorder (a take finished, rename, delete).
  std::uint64_t takesVersion() const noexcept { return version_.load(std::memory_order_relaxed); }
  // Name of the take in progress / the last finished one ("" if none).
  std::string currentTakeName() const;

  std::vector<TakeInfo> listTakes() const;  // finished takes, newest first
  bool renameTake(const std::string& name, const std::string& newName, std::string* error = nullptr);
  bool removeTake(const std::string& name);

  bool waitIdle(std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));  // armed -> ... -> idle, writer drained
  std::size_t ringCapacity() const noexcept { return capacity_; }

  // Test hook: while true the writer does not drain (to provoke overruns).
  void setWriterStalledForTest(bool stalled) noexcept { stalled_.store(stalled, std::memory_order_relaxed); }

  // ---- audio thread -----------------------------------------------------------------------------------
  // True when a take was requested and has not begun: the caller then computes the start info for this block.
  bool startPending() const noexcept { return arm_.load(std::memory_order_acquire) == kArmed && audioState_ == AudioState::Idle; }
  // Records x[0..n) (the take's samples) if a take is running. `info` is only read when a take begins.
  void process(const float* x, int n, const TakeStartInfo* info) noexcept;

 private:
  // arm_ transitions (the only writer of each edge):
  //   Idle -> Armed         start()    (message)
  //   Armed -> Idle         stop()     (message; the take never began)
  //   Armed -> Recording    process()  (audio; only when the event queue has room for Start, Gap and End)
  //   Recording -> Stopping stop()     (message)
  //   Stopping -> Idle      writer     (after the take's End event is handled and the files are closed)
  // Every edge is a compare-exchange except the writer's store, which nothing can race with: Stopping has no
  // other outgoing edge and start() needs Idle.
  enum Arm : int { kIdle = 0, kArmed = 1, kRecording = 2, kStopping = 3 };
  enum class AudioState : std::uint8_t { Idle, Recording };
  struct Event {
    enum class Type : std::uint8_t { Start, Gap, End };
    Type type = Type::End;
    std::uint64_t pos = 0;            // ring position (samples since prepare) the event happened at
    std::uint64_t samples = 0;        // Gap: silence to insert
    std::uint32_t blocks = 0;         // Gap: blocks dropped
    TakeStartInfo info;               // Start
  };
  struct Pending {                    // handed from start() to the writer (mutex)
    std::string name;
    std::string songFolder;
    bool valid = false;
  };
  struct OpenTake;

  bool pushEvent(const Event& e) noexcept;
  std::size_t eventSpace() const noexcept { return kEventCap - (evTail_.load(std::memory_order_relaxed) - evHead_.load(std::memory_order_acquire)); }
  void writerMain();
  void writerPass();
  void drainRing(std::uint64_t upTo);
  void handleEvent(const Event& e);
  void beginTake(const Event& e);
  void endTake();
  void writeZeros(std::uint64_t n);
  void writeSamples(const float* x, std::size_t n);
  void failTake(const std::string& msg);
  void setError(const std::string& msg);

  // Ring (prepare() allocates; the audio thread only indexes it).
  std::vector<float> ring_;
  std::size_t capacity_ = 0, mask_ = 0;
  std::atomic<std::uint64_t> tail_{0};   // producer (audio)
  std::atomic<std::uint64_t> head_{0};   // consumer (writer)
  // Events: SPSC, audio produces.
  static constexpr std::size_t kEventCap = 64;
  std::array<Event, kEventCap> events_{};
  std::atomic<std::size_t> evHead_{0}, evTail_{0};

  // Audio-thread state.
  AudioState audioState_ = AudioState::Idle;
  std::uint64_t gapSamples_ = 0;
  std::uint32_t gapBlocks_ = 0;
  std::atomic<int> arm_{kIdle};
  std::atomic<std::uint64_t> recorded_{0}, overruns_{0}, droppedSamples_{0};
  std::atomic<double> rate_{0.0};
  std::atomic<bool> stalled_{false};
  std::atomic<std::uint64_t> version_{0};

  mutable std::mutex m_;                 // dir_, pending_, error_, current_ (never taken by the audio thread)
  std::filesystem::path dir_;
  Pending pending_;
  std::string error_, current_;
  std::condition_variable cv_;
  bool stop_ = false;

  // Writer-thread state.
  std::unique_ptr<OpenTake> open_;
  std::thread thread_;  // last
};

}  // namespace sawblade::plugin
