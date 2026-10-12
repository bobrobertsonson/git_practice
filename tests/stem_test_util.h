#pragma once

// Shared helpers of the stem tests: temp dir, a minimal FLAC writer (dr_flac has no encoder),
// synthetic stem sets. Test files only ever go to a temp directory, never into the repo.
#include <unistd.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "sawblade/stem_set.h"
#include "sawblade/wav_io.h"

namespace sawblade::test {

struct StemTempDir {
  std::filesystem::path dir;
  StemTempDir() {
    static int counter = 0;
    dir = std::filesystem::temp_directory_path() /
          ("sawblade_stem_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    std::filesystem::create_directories(dir);
  }
  ~StemTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  StemTempDir(const StemTempDir&) = delete;
  StemTempDir& operator=(const StemTempDir&) = delete;
  std::filesystem::path operator/(const std::string& f) const { return dir / f; }
};

// ---- minimal FLAC encoder: STREAMINFO + fixed-blocksize frames of VERBATIM subframes, 16-bit ----
namespace flac_detail {
inline std::uint8_t crc8(const std::vector<std::uint8_t>& d) {
  std::uint8_t c = 0;
  for (std::uint8_t b : d) {
    c ^= b;
    for (int i = 0; i < 8; ++i) c = static_cast<std::uint8_t>((c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1));
  }
  return c;
}
inline std::uint16_t crc16(const std::vector<std::uint8_t>& d) {
  std::uint16_t c = 0;
  for (std::uint8_t b : d) {
    c = static_cast<std::uint16_t>(c ^ (b << 8));
    for (int i = 0; i < 8; ++i) c = static_cast<std::uint16_t>((c & 0x8000) ? ((c << 1) ^ 0x8005) : (c << 1));
  }
  return c;
}
inline void utf8(std::vector<std::uint8_t>& o, std::uint32_t v) {  // frame number, < 2^21 here
  if (v < 0x80) {
    o.push_back(static_cast<std::uint8_t>(v));
  } else if (v < 0x800) {
    o.push_back(static_cast<std::uint8_t>(0xC0 | (v >> 6)));
    o.push_back(static_cast<std::uint8_t>(0x80 | (v & 0x3F)));
  } else if (v < 0x10000) {
    o.push_back(static_cast<std::uint8_t>(0xE0 | (v >> 12)));
    o.push_back(static_cast<std::uint8_t>(0x80 | ((v >> 6) & 0x3F)));
    o.push_back(static_cast<std::uint8_t>(0x80 | (v & 0x3F)));
  } else {
    o.push_back(static_cast<std::uint8_t>(0xF0 | (v >> 18)));
    o.push_back(static_cast<std::uint8_t>(0x80 | ((v >> 12) & 0x3F)));
    o.push_back(static_cast<std::uint8_t>(0x80 | ((v >> 6) & 0x3F)));
    o.push_back(static_cast<std::uint8_t>(0x80 | (v & 0x3F)));
  }
}
}  // namespace flac_detail

// `chans[c][i]` are 16-bit samples; all channels equally long (>= 1 frame), 1 or 2 channels.
inline void writeFlac16(const std::filesystem::path& path, std::uint32_t rate,
                        const std::vector<std::vector<std::int16_t>>& chans, std::uint32_t blockSize = 4096) {
  using namespace flac_detail;
  const auto channels = static_cast<std::uint32_t>(chans.size());
  const std::uint64_t total = chans[0].size();
  std::vector<std::uint8_t> out = {'f', 'L', 'a', 'C'};
  out.push_back(0x80);  // last metadata block, type 0 (STREAMINFO)
  out.insert(out.end(), {0, 0, 34});
  auto put16 = [&](std::uint32_t v) { out.push_back(static_cast<std::uint8_t>(v >> 8)); out.push_back(static_cast<std::uint8_t>(v)); };
  put16(blockSize);  // min block size
  put16(blockSize);  // max block size
  out.insert(out.end(), {0, 0, 0, 0, 0, 0});  // min / max frame size unknown
  const std::uint64_t packed = (static_cast<std::uint64_t>(rate) << 44) | (static_cast<std::uint64_t>(channels - 1) << 41) |
                               (static_cast<std::uint64_t>(15) << 36) | total;
  for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(packed >> (8 * i)));
  out.insert(out.end(), 16, 0);  // MD5 unknown

  std::uint32_t frameNo = 0;
  for (std::uint64_t pos = 0; pos < total; pos += blockSize, ++frameNo) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(blockSize, total - pos));
    std::vector<std::uint8_t> f = {0xFF, 0xF8};  // sync, fixed block size
    f.push_back(0x70);                           // block size: 16-bit value follows; sample rate from STREAMINFO
    f.push_back(static_cast<std::uint8_t>(((channels - 1) << 4) | (4 << 1)));  // independent channels, 16 bit
    utf8(f, frameNo);
    f.push_back(static_cast<std::uint8_t>((n - 1) >> 8));
    f.push_back(static_cast<std::uint8_t>((n - 1) & 0xFF));
    f.push_back(crc8(f));
    for (std::uint32_t c = 0; c < channels; ++c) {
      f.push_back(0x02);  // subframe header: VERBATIM, no wasted bits
      for (std::uint32_t i = 0; i < n; ++i) {
        const auto s = static_cast<std::uint16_t>(chans[c][pos + i]);
        f.push_back(static_cast<std::uint8_t>(s >> 8));
        f.push_back(static_cast<std::uint8_t>(s & 0xFF));
      }
    }
    const std::uint16_t crc = crc16(f);
    f.push_back(static_cast<std::uint8_t>(crc >> 8));
    f.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    out.insert(out.end(), f.begin(), f.end());
  }
  std::ofstream os(path, std::ios::binary);
  os.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

// A StemSet of already-at-rate audio. Each stem given as one channel (duplicated) or L/R.
inline StemSet mkSet(double fs, std::initializer_list<std::pair<StemKind, std::vector<float>>> mono) {
  std::array<std::optional<StemAudio>, kStemKindCount> a;
  for (const auto& [k, v] : mono) a[static_cast<std::size_t>(k)] = StemAudio{v, v};
  return makeStemSet(fs, a);
}

}  // namespace sawblade::test
