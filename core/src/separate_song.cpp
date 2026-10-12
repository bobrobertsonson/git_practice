#include "sawblade/separate_song.h"

#include <array>
#include <chrono>
#include <string>
#include <vector>

#include <memory>

#include <dr_wav.h>

#include "sawblade/wav_io.h"

namespace sawblade {
namespace {

// One float32 stereo 44.1 kHz WAV per stem, appended to as the separator emits regions.
class WavStemSink final : public StemSink {
 public:
  explicit WavStemSink(std::filesystem::path dir) : dir_(std::move(dir)) {}
  ~WavStemSink() override { close(); }
  void begin(std::int64_t, bool hasGuitar) override {
    for (int k = 0; k < kStemKindCount; ++k) {
      if (k == static_cast<int>(StemKind::Guitar) && !hasGuitar) continue;
      drwav_data_format fmt{};
      fmt.container = drwav_container_riff;
      fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
      fmt.channels = 2;
      fmt.sampleRate = static_cast<drwav_uint32>(kSeparatorSampleRate);
      fmt.bitsPerSample = 32;
      const auto path = dir_ / (std::string(stemKindName(static_cast<StemKind>(k))) + ".wav");
      auto& w = wav_[static_cast<std::size_t>(k)];
      w = std::make_unique<drwav>();
      if (!drwav_init_file_write(w.get(), path.string().c_str(), &fmt, nullptr)) {
        w.reset();
        throw std::runtime_error("cannot write " + path.string());
      }
    }
  }
  void write(StemKind kind, std::int64_t, const float* l, const float* r, std::int64_t n) override {
    drwav* w = wav_[static_cast<std::size_t>(kind)].get();
    inter_.resize(static_cast<std::size_t>(n) * 2);
    for (std::int64_t i = 0; i < n; ++i) {
      inter_[static_cast<std::size_t>(i) * 2] = l[i];
      inter_[static_cast<std::size_t>(i) * 2 + 1] = r[i];
    }
    if (drwav_write_pcm_frames(w, static_cast<drwav_uint64>(n), inter_.data()) != static_cast<drwav_uint64>(n))
      throw std::runtime_error("short write in " + dir_.string() + " (disk full?)");
  }
  void close() {
    for (auto& w : wav_)
      if (w) {
        drwav_uninit(w.get());
        w.reset();
      }
  }

 private:
  std::filesystem::path dir_;
  std::array<std::unique_ptr<drwav>, kStemKindCount> wav_;
  std::vector<float> inter_;
};

}  // namespace

SeparateSongResult separateSong(const std::filesystem::path& song, const SeparateSongOptions& o,
                                const SeparationProgress& progress, CancelToken& cancel) {
  SeparateSongResult res;
  const StemCache cache(o.stemsDir.empty() ? defaultStemsDirectory() : o.stemsDir);
  res.key = StemCache::keyFor(song, o.model);
  if (const auto hit = cache.lookup(res.key, o.model)) {
    res.stemsDir = *hit;
    res.cacheHit = true;
    if (progress) progress(1.0, 0.0);
    return res;
  }
  if (o.cacheOnly) throw NotCached();
  if (cancel.cancelled()) throw SeparationCancelled();

  const ModelStore store(o.modelsDir.empty() ? defaultModelsDirectory() : o.modelsDir);
  const ModelStatus status = store.check(o.model);
  if (!status.ok()) throw ModelUnavailable(status);

  AudioFile audio = o.decoder ? o.decoder(song) : readAudioFile(song);
  if (cancel.cancelled()) throw SeparationCancelled();

  const auto t0 = std::chrono::steady_clock::now();
  Separator sep(status.path, o.model, SeparatorOptions{o.threads});
  StemCache::Staging staging = cache.beginStaging(res.key);
  {
    WavStemSink sink(staging.dir());
    sep.separate(std::move(audio), sink, progress, cancel);  // streams into the staging directory
    sink.close();
  }
  if (cancel.cancelled()) throw SeparationCancelled();
  res.stemsDir = cache.commit(staging, res.key, o.model);
  res.separateSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return res;
}

}  // namespace sawblade
