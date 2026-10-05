#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "sawblade/model_store.h"

// On-disk cache of separated songs (phase 5.1b). One directory per key under the stems root; the files
// are named so loadStemDirectory (stem_set.h) loads them unchanged: drums.wav, bass.wav, vocals.wav,
// other.wav and, for the 6-stem model, guitar.wav. They are 32-bit float stereo 44.1 kHz WAV (dr_libs
// has no FLAC encoder and none is pinned). Key = sha256(audio file bytes) + "-" + model id.
//
// An entry is written into a staging directory and renamed into place only when complete, so a
// cancelled or failed job leaves nothing behind and a reader never sees a partial entry.
// Load-time only: allocates, does I/O, throws std::runtime_error.
namespace sawblade {

// The files a complete entry holds for `m`.
std::vector<std::string> stemCacheFileNames(SeparationModel m);

class StemCache {
 public:
  explicit StemCache(std::filesystem::path root = defaultStemsDirectory()) : root_(std::move(root)) {}
  const std::filesystem::path& root() const noexcept { return root_; }

  // "<sha256 of the file's bytes>-<model id>". Throws if the file cannot be read.
  static std::string keyFor(const std::filesystem::path& audioFile, SeparationModel m);

  std::filesystem::path entryDir(const std::string& key) const { return root_ / key; }
  // The entry directory when it exists and holds every expected file, else nullopt.
  std::optional<std::filesystem::path> lookup(const std::string& key, SeparationModel m) const;

  // A uniquely named staging directory (created). Destroying it without commit() deletes it.
  class Staging {
   public:
    Staging() = default;
    Staging(Staging&& o) noexcept : dir_(std::move(o.dir_)) { o.dir_.clear(); }
    Staging& operator=(Staging&& o) noexcept;
    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;
    ~Staging();
    const std::filesystem::path& dir() const noexcept { return dir_; }

   private:
    friend class StemCache;
    explicit Staging(std::filesystem::path d) : dir_(std::move(d)) {}
    std::filesystem::path dir_;
  };
  Staging beginStaging(const std::string& key) const;
  // Atomically publishes the staging directory as the entry for `key` and returns its path. A complete
  // entry that appeared meanwhile (another process) is kept and the staging directory dropped.
  std::filesystem::path commit(Staging& staging, const std::string& key, SeparationModel m) const;

 private:
  std::filesystem::path root_;
};

}  // namespace sawblade
