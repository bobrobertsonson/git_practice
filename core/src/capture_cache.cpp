#include "sawblade/capture_cache.h"

#include <filesystem>
#include <optional>

#include "sawblade/sha256.h"

namespace sawblade {
namespace fs = std::filesystem;

struct CaptureCache::FileEntry {
  std::mutex m;  // guards everything below; held while loading, so one load per file
  bool known = false;
  std::uintmax_t size = 0;
  fs::file_time_type mtime{};
  std::string sha;
  std::shared_ptr<const NamModel> nam;
  std::map<std::pair<double, bool>, std::shared_ptr<const IrData>> irs;
};

CaptureCache::CaptureCache() = default;
CaptureCache::~CaptureCache() = default;

std::shared_ptr<CaptureCache::FileEntry> CaptureCache::entryFor(const std::string& key) {
  std::lock_guard<std::mutex> lk(mapMutex_);
  auto& e = files_[key];
  if (!e) e = std::make_shared<FileEntry>();
  return e;
}

namespace {

// Validates `e` against the file on disk (stat-gated re-hash) and the preset's sha256.
// Caller holds e.m.
template <class Entry>
void validate(Entry& e, const Capture& c, const std::string& jsonPath) {
  std::error_code ec;
  const fs::path p = locateCapture(c);
  if (!fs::exists(p, ec)) throw CaptureError(jsonPath, captureNotFoundMessage(c, jsonPath));
  const std::uintmax_t size = fs::file_size(p, ec);
  const fs::file_time_type mtime = ec ? fs::file_time_type{} : fs::last_write_time(p, ec);
  if (!e.known || ec || size != e.size || mtime != e.mtime) {
    std::string sha;
    try {
      sha = sha256File(p);
    } catch (const std::exception& ex) {
      throw CaptureError(jsonPath, ex.what());
    }
    if (!e.known || sha != e.sha) {
      e.nam.reset();
      e.irs.clear();
    }
    e.known = !ec;
    e.size = size;
    e.mtime = mtime;
    e.sha = sha;
  }
  if (!c.sha256.empty() && e.sha != c.sha256)
    throw CaptureError(jsonPath, jsonPath + ": sha256 mismatch for " + p.string() + " (expected " + c.sha256 +
                                     ", got " + e.sha + ")");
}

std::string keyOf(const Capture& c) { return fs::absolute(locateCapture(c)).lexically_normal().string(); }

}  // namespace

std::shared_ptr<const NamModel> CaptureCache::namModel(const Capture& c, const std::string& jsonPath) {
  const auto e = entryFor(keyOf(c));
  std::lock_guard<std::mutex> lk(e->m);
  validate(*e, c, jsonPath);
  if (e->nam) {
    ++hits_;
    return e->nam;
  }
  ++misses_;
  try {
    e->nam = NamModel::load(locateCapture(c));
  } catch (const std::exception& ex) {
    throw CaptureError(jsonPath, ex.what());
  }
  return e->nam;
}

std::shared_ptr<const IrData> CaptureCache::ir(const Capture& c, const std::string& jsonPath, double targetRate,
                                               bool normalize) {
  const auto e = entryFor(keyOf(c));
  std::lock_guard<std::mutex> lk(e->m);
  validate(*e, c, jsonPath);
  const auto k = std::make_pair(targetRate, normalize);
  if (auto it = e->irs.find(k); it != e->irs.end()) {
    ++hits_;
    return it->second;
  }
  ++misses_;
  try {
    auto data = std::make_shared<const IrData>(loadIr(locateCapture(c), targetRate, normalize));
    e->irs.emplace(k, data);
    return data;
  } catch (const std::exception& ex) {
    throw CaptureError(jsonPath, ex.what());
  }
}

CaptureCache::Stats CaptureCache::stats() const {
  Stats s;
  s.hits = hits_.load();
  s.misses = misses_.load();
  std::lock_guard<std::mutex> lk(mapMutex_);
  s.files = files_.size();
  return s;
}

void CaptureCache::resetStats() {
  hits_ = 0;
  misses_ = 0;
}

void CaptureCache::clear() {
  {
    std::lock_guard<std::mutex> lk(mapMutex_);
    files_.clear();
  }
  resetStats();
}

}  // namespace sawblade
