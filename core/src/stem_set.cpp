#include "sawblade/stem_set.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>

#include "sawblade/loudness.h"
#include "sawblade/resample.h"
#include "sawblade/wav_io.h"

namespace sawblade {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("stems: " + what); }

void validateRate(double sampleRate) {
  if (!(sampleRate > 0.0) || !std::isfinite(sampleRate)) fail("sample rate must be positive");
}

void padTo(StemSet& s) {
  for (int k = 0; k < kStemKindCount; ++k) {
    if (!s.present[static_cast<std::size_t>(k)]) continue;
    for (auto& ch : s.audio[static_cast<std::size_t>(k)]) ch.resize(static_cast<std::size_t>(s.length), 0.0f);
  }
}

// Adds `src` into `dst` (growing dst with zeros if needed).
void sumInto(std::vector<float>& dst, const std::vector<float>& src) {
  if (dst.size() < src.size()) dst.resize(src.size(), 0.0f);
  for (std::size_t i = 0; i < src.size(); ++i) dst[i] += src[i];
}

void measureBacking(StemSet& s) {
  std::vector<float> l(static_cast<std::size_t>(s.length), 0.0f), r(l);
  bool any = false;
  for (int k = 0; k < kStemKindCount; ++k) {
    const auto ks = static_cast<std::size_t>(k);
    if (!s.present[ks] || k == static_cast<int>(StemKind::Guitar)) continue;
    any = true;
    for (std::size_t i = 0; i < l.size(); ++i) {
      l[i] += s.audio[ks][0][i];
      r[i] += s.audio[ks][1][i];
    }
  }
  s.backingLoudnessLufs.reset();
  if (any) s.backingLoudnessLufs = integratedLoudnessLufs(l.data(), r.data(), s.length, s.sampleRate);
}

}  // namespace

const char* stemKindName(StemKind kind) noexcept {
  switch (kind) {
    case StemKind::Drums: return "drums";
    case StemKind::Bass: return "bass";
    case StemKind::Vocals: return "vocals";
    case StemKind::Other: return "other";
    case StemKind::Guitar: return "guitar";
  }
  return "other";
}

StemSet loadStemFiles(const std::vector<std::pair<StemKind, std::filesystem::path>>& files, double sampleRate) {
  validateRate(sampleRate);
  if (files.empty()) fail("no stem files given");
  StemSet set;
  set.sampleRate = sampleRate;

  for (const auto& [kind, path] : files) {
    AudioFile f;
    try {
      f = readAudioFile(path);
    } catch (const std::exception& e) {
      throw std::runtime_error(e.what());  // already carries the path
    }
    const auto ch = static_cast<std::size_t>(f.channels);
    const std::size_t frames = f.interleaved.size() / ch;
    if (frames == 0) fail("stem has zero frames (" + path.string() + ")");
    if (f.channels > 2)
      set.warnings.push_back(path.string() + ": " + std::to_string(f.channels) + " channels; using the first two");

    StemAudio chans;
    for (std::size_t c = 0; c < 2; ++c) {
      const std::size_t src = ch == 1 ? 0 : c;
      std::vector<float> x(frames);
      for (std::size_t i = 0; i < frames; ++i) x[i] = f.interleaved[i * ch + src];
      chans[c] = resample(x, f.sampleRate, sampleRate);
    }
    const auto k = static_cast<std::size_t>(kind);
    if (!set.present[k]) {
      set.present[k] = true;
      set.audio[k] = std::move(chans);
    } else {
      sumInto(set.audio[k][0], chans[0]);
      sumInto(set.audio[k][1], chans[1]);
    }
    set.sources[k].push_back({path.string(), f.sampleRate, f.channels, static_cast<std::int64_t>(frames)});
  }

  for (int k = 0; k < kStemKindCount; ++k)
    if (set.present[static_cast<std::size_t>(k)])
      set.length = std::max<std::int64_t>(set.length, static_cast<std::int64_t>(set.audio[static_cast<std::size_t>(k)][0].size()));
  if (set.length == 0) fail("stems have zero length after resampling");
  padTo(set);
  measureBacking(set);
  return set;
}

StemSet loadStemDirectory(const std::filesystem::path& dir, double sampleRate, OtherRole otherRole) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) fail("not a directory (" + dir.string() + ")");

  std::vector<fs::path> audioFiles;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    if (isHiddenFileName(it->path().filename().string())) continue;  // AppleDouble ._x.wav, .DS_Store
    const std::string ext = lower(it->path().extension().string());
    if (ext == ".wav" || ext == ".flac") audioFiles.push_back(it->path());
  }
  if (ec) fail("cannot read directory (" + dir.string() + "): " + ec.message());
  std::sort(audioFiles.begin(), audioFiles.end(),
            [](const fs::path& a, const fs::path& b) { return a.filename().string() < b.filename().string(); });

  // Pass 1: name -> kind (unknown names go to `other`).
  struct Entry {
    StemKind kind;
    fs::path path;
    bool unknown;
    bool quiet;  // recognised, but maps to `other` without a duplicate or unknown-name warning (piano)
  };
  std::vector<Entry> entries;
  bool realGuitar = false, anyOther = false;
  for (const auto& p : audioFiles) {
    const std::string base = lower(p.stem().string());
    StemKind kind = StemKind::Other;
    bool unknown = false, quiet = false;
    if (base == "drums") kind = StemKind::Drums;
    else if (base == "bass") kind = StemKind::Bass;
    else if (base == "vocals") kind = StemKind::Vocals;
    else if (base == "other") kind = StemKind::Other;
    else if (base == "guitar" || base == "guitars") kind = StemKind::Guitar;
    else if (base == "piano") quiet = true;  // 6-stem separation piano: summed into `other`, not a surprise
    else unknown = true;
    realGuitar = realGuitar || kind == StemKind::Guitar;
    anyOther = anyOther || kind == StemKind::Other;
    entries.push_back({kind, p, unknown, quiet});
  }
  if (entries.empty()) fail("no .wav or .flac stems found in " + dir.string());

  // Pass 2: role mapping, then warnings (which name the kind a file ends up in).
  const bool toGuitar = otherRole == OtherRole::Guitar && !realGuitar && anyOther;
  std::vector<std::pair<StemKind, fs::path>> files;
  std::vector<std::string> warnings;
  std::array<std::string, kStemKindCount> firstNamed;  // first file that mapped to each stem
  for (auto& e : entries) {
    if (toGuitar && e.kind == StemKind::Other) e.kind = StemKind::Guitar;
    if (e.unknown) {
      warnings.push_back(e.path.filename().string() + ": unrecognised stem name; summed into '" + stemKindName(e.kind) + "'");
    } else if (!e.quiet) {
      std::string& first = firstNamed[static_cast<std::size_t>(e.kind)];
      if (!first.empty())
        warnings.push_back(e.path.filename().string() + ": duplicate '" + stemKindName(e.kind) + "' stem (also " + first + "); summed");
      else first = e.path.filename().string();
    }
    files.emplace_back(e.kind, e.path);
  }

  StemSet set = loadStemFiles(files, sampleRate);
  set.warnings.insert(set.warnings.begin(), warnings.begin(), warnings.end());
  set.otherMappedToGuitar = toGuitar;
  return set;
}

bool isHiddenFileName(const std::string& fileName) { return !fileName.empty() && fileName.front() == '.'; }

bool isAudioFileName(const std::string& path) {
  const std::string ext = lower(std::filesystem::path(path).extension().string());
  return ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".m4a" || ext == ".aac" || ext == ".aif" || ext == ".aiff" ||
         ext == ".ogg";
}

bool isStemName(const std::string& baseName) {
  const std::string b = lower(baseName);
  return b == "drums" || b == "bass" || b == "vocals" || b == "other" || b == "guitar" || b == "guitars" || b == "piano";
}

StemFolderCheck classifyStemFolder(const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  StemFolderCheck r;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return r;  // not a folder: loadStemDirectory reports it
  std::set<std::string> kinds;
  std::string stranger;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2) || isHiddenFileName(it->path().filename().string()) ||
        !isAudioFileName(it->path().filename().string()))
      continue;
    const std::string base = lower(it->path().stem().string());
    if (!isStemName(base)) {
      if (stranger.empty() || it->path().filename().string() < stranger) stranger = it->path().filename().string();
      continue;
    }
    kinds.insert(base == "guitars" ? "guitar" : base);
  }
  r.recognised = static_cast<int>(kinds.size());
  if (!stranger.empty()) {
    r.ok = false;
    r.reason = "'" + stranger + "' is not a stem name";
  } else if (r.recognised < 2) {
    r.ok = false;
    r.reason = r.recognised == 0 ? "no stem files" : "only one stem file";
  } else {
    r.ok = true;
  }
  return r;
}

StemSet makeStemSet(double sampleRate, const std::array<std::optional<StemAudio>, kStemKindCount>& audio) {
  validateRate(sampleRate);
  StemSet set;
  set.sampleRate = sampleRate;
  for (int k = 0; k < kStemKindCount; ++k) {
    const auto& a = audio[static_cast<std::size_t>(k)];
    if (!a) continue;
    const std::string name = stemKindName(static_cast<StemKind>(k));
    if ((*a)[0].size() != (*a)[1].size()) fail("stem '" + name + "': L and R differ in length");
    if ((*a)[0].empty()) fail("stem '" + name + "' has zero frames");
    set.present[static_cast<std::size_t>(k)] = true;
    set.audio[static_cast<std::size_t>(k)] = *a;
    set.length = std::max<std::int64_t>(set.length, static_cast<std::int64_t>((*a)[0].size()));
  }
  if (set.length == 0) fail("no stems given");
  padTo(set);
  measureBacking(set);
  return set;
}

}  // namespace sawblade
