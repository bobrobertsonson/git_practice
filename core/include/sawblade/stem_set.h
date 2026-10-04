#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Backing-track stems (drums, bass, vocals, other, guitar) as immutable in-memory stereo audio at
// one sample rate, plus the load-time loader. Everything here is load-time only: it allocates, does
// I/O and throws. The real-time player is stem_player.h.
namespace sawblade {

enum class StemKind : int { Drums = 0, Bass, Vocals, Other, Guitar };
constexpr int kStemKindCount = 5;

// "drums", "bass", "vocals", "other", "guitar".
const char* stemKindName(StemKind kind) noexcept;

struct StemInfo {
  std::string file;           // path as given to the loader
  double sourceRate = 0.0;    // before resampling
  int sourceChannels = 0;
  std::int64_t sourceFrames = 0;
};

// Immutable after construction.
struct StemSet {
  double sampleRate = 0.0;
  std::int64_t length = 0;  // frames: the longest stem; shorter ones are zero-padded
  std::array<bool, kStemKindCount> present{};
  // [kind][0 = L, 1 = R]; each `length` long when present, empty otherwise.
  std::array<std::array<std::vector<float>, 2>, kStemKindCount> audio;
  std::array<std::vector<StemInfo>, kStemKindCount> sources;  // files that went into each stem
  std::vector<std::string> warnings;
  // BS.1770-4 integrated loudness (LUFS) of the unity-gain sum of every present stem except guitar,
  // measured at load time. None if there is no non-guitar stem or it is silent. Metadata only: it
  // is for suggesting a backing level in a UI; StemPlayer never applies gain from it.
  std::optional<double> backingLoudnessLufs;
};

// Loads the given files, each mapped to a kind; several files may map to the same kind and are
// summed (after resampling). Mono files are duplicated to L and R; files with more than two
// channels keep the first two (with a warning). Every file is resampled to `sampleRate` with the
// offline Kaiser resample() (bit-identical pass-through at equal rates). `.wav` and `.flac`.
// Throws std::runtime_error (path in the message) for an unreadable/undecodable file, an empty
// list, a non-positive rate, or a file with zero frames.
StemSet loadStemFiles(const std::vector<std::pair<StemKind, std::filesystem::path>>& files, double sampleRate);

// Scans `dir` (non-recursive) for *.wav / *.flac (case-insensitive extension), in sorted file-name
// order. Base names `drums`, `bass`, `vocals`, `other`, `guitar` or `guitars` (case-insensitive)
// map to their kind; any other audio file (e.g. Demucs 6-stem `piano.wav`) is summed into `other`
// with a warning naming it. Non-audio files are ignored. Throws std::runtime_error when the
// directory is missing or holds no audio file, plus the loadStemFiles errors.
StemSet loadStemDirectory(const std::filesystem::path& dir, double sampleRate);

using StemAudio = std::array<std::vector<float>, 2>;  // L, R

// Builds a set from audio that is already at `sampleRate` (tests, bindings). L and R of a stem
// must have equal, non-zero length; stems are zero-padded to the longest. Throws
// std::runtime_error for a non-positive rate, an invalid stem, or no stems at all.
StemSet makeStemSet(double sampleRate, const std::array<std::optional<StemAudio>, kStemKindCount>& audio);

}  // namespace sawblade
