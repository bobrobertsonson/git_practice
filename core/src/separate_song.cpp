#include "sawblade/separate_song.h"

#include <chrono>

#include "sawblade/wav_io.h"

namespace sawblade {

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
  if (cancel.cancelled()) throw SeparationCancelled();

  const ModelStore store(o.modelsDir.empty() ? defaultModelsDirectory() : o.modelsDir);
  const ModelStatus status = store.check(o.model);
  if (!status.ok()) throw ModelUnavailable(status);

  AudioFile audio = o.decoder ? o.decoder(song) : readAudioFile(song);
  if (cancel.cancelled()) throw SeparationCancelled();

  const auto t0 = std::chrono::steady_clock::now();
  Separator sep(status.path, o.model, SeparatorOptions{o.threads});
  SeparationResult r = sep.separate(std::move(audio), progress, cancel);

  StemCache::Staging staging = cache.beginStaging(res.key);
  for (int k = 0; k < kStemKindCount; ++k) {
    auto& st = r.stems[static_cast<std::size_t>(k)];
    if (!st) continue;
    writeWavFloat32Stereo(staging.dir() / (std::string(stemKindName(static_cast<StemKind>(k))) + ".wav"),
                          kSeparatorSampleRate, (*st)[0], (*st)[1]);
    st.reset();  // free as we go
    if (cancel.cancelled()) throw SeparationCancelled();
  }
  res.stemsDir = cache.commit(staging, res.key, o.model);
  res.separateSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return res;
}

}  // namespace sawblade
