#pragma once

#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

#include "sawblade/model_store.h"
#include "sawblade/stem_set.h"
#include "sawblade/wav_io.h"

// On-device stem separation (phase 5.1b): the htdemucs core network run by ONNX Runtime (CPU EP) with our
// own STFT / iSTFT, segmentation and overlap-add around it (docs/specs/phase5_1a_separator_engine_REPORT.md,
// spikes/separator/separator_onnx.cpp). Only built with SAWBLADE_WITH_SEPARATOR. OFF THE AUDIO THREAD:
// allocates, does I/O, takes seconds to minutes, throws. This header does not include ONNX Runtime.
namespace sawblade {

// Thrown by Separator::separate / separateSong when the job was cancelled.
class SeparationCancelled : public std::runtime_error {
 public:
  SeparationCancelled() : std::runtime_error("separation cancelled") {}
};

// Cooperative cancel, shared between the job and whoever may cancel it (any thread). cancel() is
// idempotent and returns at once; the engine notices between segments, and cancel() also interrupts a
// running network evaluation (ONNX Runtime RunOptions::SetTerminate), so the job returns well within one
// segment. A token cancels one job: make a new one per job.
class CancelToken {
 public:
  void cancel() {
    flag_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lk(m_);
    if (hook_) hook_();
  }
  bool cancelled() const noexcept { return flag_.load(std::memory_order_acquire); }
  // Engine side: the callable runs on the cancelling thread and must be thread-safe. Always cleared
  // before the objects it touches die.
  void setHook(std::function<void()> h) {
    std::lock_guard<std::mutex> lk(m_);
    hook_ = std::move(h);
  }
  void clearHook() { setHook(nullptr); }

 private:
  std::atomic<bool> flag_{false};
  std::mutex m_;
  std::function<void()> hook_;
};

// fraction in 0..1; etaSeconds is the remaining time estimated from the segments done so far (< 0: not
// known yet). Called on the thread running the job: once before the first segment (fraction 0) and
// after every segment.
using SeparationProgress = std::function<void(double fraction, double etaSeconds)>;

struct SeparatorOptions {
  int threads = 0;  // ONNX Runtime intra-op threads; 0 = defaultSeparatorThreads()
};
// max(1, hardware_concurrency - 1)
int defaultSeparatorThreads();

constexpr double kSeparatorSampleRate = 44100.0;

// Receives the separated stems incrementally, so a whole song never has to be held in memory. For every
// StemKind the engine present for the model (drums, bass, vocals, other, and guitar for the 6-stem model)
// write() is called with consecutive, increasing, non-overlapping ranges that together cover [0, length).
// Ranges of different kinds arrive interleaved in time. The 6-stem model's piano is already summed into
// `other`. Called on the separating thread; may throw (the job then fails).
class StemSink {
 public:
  virtual ~StemSink() = default;
  virtual void begin(std::int64_t length, bool hasGuitar) = 0;
  virtual void write(StemKind kind, std::int64_t offset, const float* left, const float* right, std::int64_t frames) = 0;
};

// Stems at 44.1 kHz, each `length` frames. The 6-stem model's piano is already summed into `other`.
struct SeparationResult {
  std::int64_t length = 0;
  std::array<std::optional<StemAudio>, kStemKindCount> stems;
  int segments = 0;
};

// A sink that collects whole stems in memory (tests, short clips).
class CollectingSink final : public StemSink {
 public:
  void begin(std::int64_t length, bool hasGuitar) override {
    result.length = length;
    for (int k = 0; k < kStemKindCount; ++k) {
      auto& o = result.stems[static_cast<std::size_t>(k)];
      o.reset();
      if (k == static_cast<int>(StemKind::Guitar) && !hasGuitar) continue;
      o = StemAudio{std::vector<float>(static_cast<std::size_t>(length)), std::vector<float>(static_cast<std::size_t>(length))};
    }
  }
  void write(StemKind kind, std::int64_t offset, const float* l, const float* r, std::int64_t n) override {
    auto& a = *result.stems[static_cast<std::size_t>(kind)];
    std::copy(l, l + n, a[0].begin() + offset);
    std::copy(r, r + n, a[1].begin() + offset);
  }
  SeparationResult result;
};

class Separator {
 public:
  // Loads the ONNX core graph (sha256 verification is ModelStore's job). Throws std::runtime_error when the
  // file is not a htdemucs core graph with the model's source count.
  Separator(const std::filesystem::path& onnxFile, SeparationModel model, const SeparatorOptions& opts = {});
  ~Separator();
  Separator(const Separator&) = delete;
  Separator& operator=(const Separator&) = delete;

  SeparationModel model() const noexcept;
  int threads() const noexcept;

  // Streams the stems into `sink` as they become final (a sliding window of about one segment per stem is
  // all that is held, so peak memory does not grow with the song's length beyond the input). Otherwise as
  // below.
  void separate(AudioFile audio, StemSink& sink, const SeparationProgress& progress, CancelToken& cancel);

  // Separates `audio` (mono or stereo; any rate, resampled to 44.1 kHz with the offline Kaiser resampler;
  // mono is duplicated; channels beyond two are dropped). `audio` is consumed to keep peak memory low:
  // the working set is the normalised stereo input plus one accumulator per output channel, and every
  // segment is overlap-added straight into the accumulators. Throws SeparationCancelled when `cancel`
  // fires, std::runtime_error on an empty input or an engine failure.
  SeparationResult separate(AudioFile audio, const SeparationProgress& progress, CancelToken& cancel);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sawblade
