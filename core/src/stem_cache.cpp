#include "sawblade/stem_cache.h"

#include <atomic>
#include <chrono>
#include <random>
#include <stdexcept>
#include <system_error>

#include "sawblade/sha256.h"

namespace sawblade {
namespace fs = std::filesystem;

std::vector<std::string> stemCacheFileNames(SeparationModel m) {
  std::vector<std::string> v{"drums.wav", "bass.wav", "vocals.wav", "other.wav"};
  if (m == SeparationModel::Htdemucs6s) v.push_back("guitar.wav");
  return v;
}

std::string StemCache::keyFor(const fs::path& audioFile, SeparationModel m) {
  return sha256File(audioFile) + "-" + separationModelId(m);
}

std::optional<fs::path> StemCache::lookup(const std::string& key, SeparationModel m) const {
  const fs::path d = entryDir(key);
  std::error_code ec;
  if (!fs::is_directory(d, ec)) return std::nullopt;
  for (const auto& f : stemCacheFileNames(m))
    if (!fs::is_regular_file(d / f, ec)) return std::nullopt;
  return d;
}

StemCache::Staging& StemCache::Staging::operator=(Staging&& o) noexcept {
  if (this != &o) {
    if (!dir_.empty()) {
      std::error_code ec;
      fs::remove_all(dir_, ec);
    }
    dir_ = std::move(o.dir_);
    o.dir_.clear();
  }
  return *this;
}

StemCache::Staging::~Staging() {
  if (dir_.empty()) return;
  std::error_code ec;
  fs::remove_all(dir_, ec);
}

StemCache::Staging StemCache::beginStaging(const std::string& key) const {
  std::error_code ec;
  fs::create_directories(root_, ec);
  if (ec) throw std::runtime_error("stem cache: cannot create " + root_.string() + ": " + ec.message());
  // Housekeeping: staging directories a crashed process left behind (older than a day).
  for (fs::directory_iterator it(root_, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (n.rfind(".tmp-", 0) != 0) continue;
    std::error_code e2;
    const auto t = fs::last_write_time(it->path(), e2);
    if (!e2 && fs::file_time_type::clock::now() - t > std::chrono::hours(24)) fs::remove_all(it->path(), e2);
  }
  static std::atomic<unsigned> counter{0};
  std::random_device rd;
  for (int attempt = 0; attempt < 16; ++attempt) {
    const fs::path d = root_ / (".tmp-" + key.substr(0, 16) + "-" + std::to_string(rd()) + "-" + std::to_string(counter++));
    std::error_code e3;
    if (fs::create_directory(d, e3)) return Staging(d);
  }
  throw std::runtime_error("stem cache: cannot create a staging directory in " + root_.string());
}

fs::path StemCache::commit(Staging& staging, const std::string& key, SeparationModel m) const {
  const fs::path dst = entryDir(key);
  std::error_code ec;
  fs::rename(staging.dir_, dst, ec);
  if (ec) {
    if (lookup(key, m)) return dst;  // another process finished first; ours is dropped by ~Staging
    std::error_code e2;
    fs::remove_all(dst, e2);  // an incomplete leftover
    fs::rename(staging.dir_, dst, ec);
    if (ec) throw std::runtime_error("stem cache: cannot publish " + dst.string() + ": " + ec.message());
  }
  staging.dir_.clear();
  return dst;
}

}  // namespace sawblade
