#pragma once

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

#include "sawblade/model_store.h"
#include "sawblade/separator.h"
#include "sawblade/stem_cache.h"

// One call from "an audio file" to "a directory of stems" that loadStemDirectory loads: stem-cache lookup,
// model check, decode, separate, publish into the cache (phase 5.1b). Only built with SAWBLADE_WITH_SEPARATOR.
// Off the audio thread; throws.
namespace sawblade {

// The model file is missing or fails its sha256 check. status.message holds the exact fetch command.
class ModelUnavailable : public std::runtime_error {
 public:
  explicit ModelUnavailable(ModelStatus s) : std::runtime_error(s.message), status(std::move(s)) {}
  ModelStatus status;
};

// Thrown instead of separating when SeparateSongOptions::cacheOnly is set and the song is not cached.
class NotCached : public std::runtime_error {
 public:
  NotCached() : std::runtime_error("song not separated yet") {}
};

struct SeparateSongOptions {
  bool cacheOnly = false;                   // never separate: a cache miss throws NotCached
  SeparationModel model = SeparationModel::Htdemucs6s;
  int threads = 0;                          // 0 = defaultSeparatorThreads()
  std::filesystem::path modelsDir;          // empty = defaultModelsDirectory()
  std::filesystem::path stemsDir;           // empty = defaultStemsDirectory()
  // Replaces readAudioFile (wav / flac / mp3) for decoding, e.g. the plugin's platform decoder for m4a.
  std::function<AudioFile(const std::filesystem::path&)> decoder;
};

struct SeparateSongResult {
  std::filesystem::path stemsDir;  // the cache entry: drums.wav, bass.wav, vocals.wav, other.wav[, guitar.wav]
  std::string key;
  bool cacheHit = false;
  double separateSeconds = 0.0;    // 0 on a cache hit
};

// Cache hit: returns at once (progress called once with 1.0), the model is not needed. Otherwise: throws
// ModelUnavailable when the model is missing, SeparationCancelled when `cancel` fires (nothing is left in
// the cache), std::runtime_error for decode / engine failures. Mono input is duplicated to stereo.
SeparateSongResult separateSong(const std::filesystem::path& song, const SeparateSongOptions& options,
                                const SeparationProgress& progress, CancelToken& cancel);

}  // namespace sawblade
