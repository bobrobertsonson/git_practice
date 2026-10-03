#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "sawblade/ir.h"
#include "sawblade/nam_block.h"
#include "sawblade/preset.h"

namespace sawblade {

// Load-once store of the slow-to-build capture data, shared by any number of renders (and threads).
//
//   * NAM: the parsed model (NamModel). Each render still builds its own NamBlock (own state), so
//     renders never share mutable DSP state; what is saved is the file read + JSON parse.
//   * IR: the loaded, resampled and (optionally) normalized IrData per (render rate, normalize).
//     Each render builds its own Convolver from it.
//
// Keying: entries are per file path (absolute, lexically normalized) and carry the file's SHA-256.
// Every lookup re-validates the entry cheaply with the file's size + mtime; if either changed the
// file is re-hashed and, when the hash differs, all data derived from the old content is dropped
// (so an edited file is not served stale, and an unchanged file is never re-read). The check is
// stat-gated: an edit that keeps both size and mtime (e.g. same-size rewrite with the mtime
// restored) is NOT noticed and is served stale; clear() forces a reload. A preset's
// own `sha256`, when present, is checked against the content hash on every lookup, exactly as
// verifyCapture does (same error text).
//
// Counters: a lookup that finds the data already loaded is a hit; one that has to load it (also a
// failed load) is a miss. Cached results are bit-identical to uncached ones.
//
// Thread safety: all members are safe to call concurrently. Concurrent first lookups of the same
// file wait for one load; different files load in parallel. Returned objects are immutable.
// Errors are CaptureError (a std::runtime_error carrying the JSON path of the `file` member).
class CaptureCache {
 public:
  struct Stats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::size_t files = 0;  // distinct files currently tracked
  };

  CaptureCache();
  ~CaptureCache();
  CaptureCache(const CaptureCache&) = delete;
  CaptureCache& operator=(const CaptureCache&) = delete;

  // `jsonPath` is the path of the capture's `file` member ("paths.a.blocks[0].model.file").
  std::shared_ptr<const NamModel> namModel(const Capture& c, const std::string& jsonPath);
  std::shared_ptr<const IrData> ir(const Capture& c, const std::string& jsonPath, double targetRate, bool normalize);

  Stats stats() const;
  void resetStats();  // zero the counters; keeps the data
  void clear();       // drop all data and zero the counters

 private:
  struct FileEntry;
  std::shared_ptr<FileEntry> entryFor(const std::string& key);

  mutable std::mutex mapMutex_;
  std::map<std::string, std::shared_ptr<FileEntry>> files_;
  std::atomic<std::uint64_t> hits_{0}, misses_{0};
};

}  // namespace sawblade
