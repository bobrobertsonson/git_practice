// Phase 5.1b support code that needs no ONNX Runtime: audio decoding (wav / flac / mp3), the model store
// (paths, sha256 check, fetch command) and the stem cache.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "sawblade/model_store.h"
#include "sawblade/sha256.h"
#include "sawblade/stem_cache.h"
#include "sawblade/stem_set.h"
#include "sawblade/wav_io.h"
#include "stem_test_util.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

// Sets an environment variable for a scope.
struct EnvGuard {
  std::string name;
  std::optional<std::string> old;
  EnvGuard(const std::string& n, const std::optional<std::string>& v) : name(n) {
    if (const char* o = std::getenv(n.c_str())) old = o;
    set(v);
  }
  void set(const std::optional<std::string>& v) {
    if (v) ::setenv(name.c_str(), v->c_str(), 1);
    else ::unsetenv(name.c_str());
  }
  ~EnvGuard() { set(old); }
};

void writeText(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

bool haveFfmpeg() { return std::system("ffmpeg -version >/dev/null 2>&1") == 0; }

}  // namespace

TEST_CASE("Decoding: wav, flac and mp3 fixtures generated at test time", "[separation][decode]") {
  StemTempDir t;
  const double fs0 = 44100.0;
  const std::size_t n = 44100;  // 1 s
  const auto l = sine(440.0, fs0, n, 0.4), r = sine(660.0, fs0, n, 0.3);
  writeWavFloat32Stereo(t / "a.wav", fs0, l, r);

  SECTION("wav") {
    const AudioFile f = readAudioFile(t / "a.wav");
    REQUIRE(f.channels == 2);
    REQUIRE(f.sampleRate == fs0);
    REQUIRE(f.interleaved.size() == 2 * n);
    REQUIRE(f.interleaved[2 * 100] == l[100]);
  }
  SECTION("flac") {
    std::vector<std::int16_t> a(n), b(n);
    for (std::size_t i = 0; i < n; ++i) {
      a[i] = static_cast<std::int16_t>(std::lround(l[i] * 32767.0));
      b[i] = static_cast<std::int16_t>(std::lround(r[i] * 32767.0));
    }
    writeFlac16(t / "a.flac", 44100, {a, b});
    const AudioFile f = readAudioFile(t / "a.flac");
    REQUIRE(f.channels == 2);
    REQUIRE(f.sampleRate == fs0);
    REQUIRE(f.interleaved.size() == 2 * n);
    for (std::size_t i = 0; i < n; i += 997) REQUIRE(std::fabs(f.interleaved[2 * i] - l[i]) < 1e-4);
  }
  SECTION("mp3 (encoded with ffmpeg; skipped when ffmpeg is absent)") {
    if (!haveFfmpeg()) SKIP("ffmpeg not found: mp3 fixtures need it");
    for (const auto& [ch, rate] : std::vector<std::pair<int, int>>{{2, 44100}, {1, 44100}, {2, 48000}}) {
      const std::string out = (t / ("c" + std::to_string(ch) + "_" + std::to_string(rate) + ".mp3")).string();
      const std::string cmd = "ffmpeg -y -loglevel error -i '" + (t / "a.wav").string() + "' -ac " + std::to_string(ch) + " -ar " +
                              std::to_string(rate) + " -codec:a libmp3lame -b:a 256k '" + out + "'";
      REQUIRE(std::system(cmd.c_str()) == 0);
      const AudioFile f = readAudioFile(out);
      REQUIRE(f.channels == ch);
      REQUIRE(f.sampleRate == static_cast<double>(rate));
      const std::size_t frames = f.interleaved.size() / static_cast<std::size_t>(ch);
      const double expect = static_cast<double>(n) * rate / fs0;
      REQUIRE(std::fabs(static_cast<double>(frames) - expect) < 3000.0);  // encoder delay + padding
      // Same tone: the left channel's RMS is that of a 0.4 sine (the first channel for mono input is the mono mix).
      std::vector<float> c0(frames);
      for (std::size_t i = 0; i < frames; ++i) c0[i] = f.interleaved[i * static_cast<std::size_t>(ch)];
      const double r0 = rms(c0.data() + 4000, frames - 8000);
      if (ch == 2) REQUIRE(std::fabs(r0 - 0.4 / std::sqrt(2.0)) < 0.01);  // the 0.4 sine
      else REQUIRE((r0 > 0.15 && r0 < 0.4));                              // ffmpeg's downmix gain is its own business
    }
  }
  SECTION("unsupported and broken files name the path") {
    writeText(t / "x.m4a", "not audio");
    REQUIRE_THROWS_WITH(readAudioFile(t / "x.m4a"), ContainsSubstring("x.m4a"));
    REQUIRE_THROWS_WITH(readAudioFile(t / "x.m4a"), ContainsSubstring("unsupported"));
    writeText(t / "bad.mp3", "not an mp3 at all, just text");
    REQUIRE_THROWS_WITH(readAudioFile(t / "bad.mp3"), ContainsSubstring("bad.mp3"));
  }
}

TEST_CASE("ModelStore: directory resolution", "[separation][modelstore]") {
  SECTION("SAWBLADE_MODELS_DIR wins; the stems dir is its sibling `stems`") {
    EnvGuard m("SAWBLADE_MODELS_DIR", "/data/x/models");
    EnvGuard s("SAWBLADE_STEMS_DIR", std::nullopt);
    REQUIRE(defaultModelsDirectory() == fs::path("/data/x/models"));
    REQUIRE(defaultStemsDirectory() == fs::path("/data/x/stems"));
    EnvGuard s2("SAWBLADE_STEMS_DIR", "/elsewhere/s");
    REQUIRE(defaultStemsDirectory() == fs::path("/elsewhere/s"));
  }
#if !defined(__APPLE__)
  SECTION("XDG data home, then ~/.local/share") {
    EnvGuard m("SAWBLADE_MODELS_DIR", std::nullopt);
    EnvGuard s("SAWBLADE_STEMS_DIR", std::nullopt);
    EnvGuard x("XDG_DATA_HOME", "/xdg");
    REQUIRE(defaultModelsDirectory() == fs::path("/xdg/sawblade/models"));
    REQUIRE(defaultStemsDirectory() == fs::path("/xdg/sawblade/stems"));
    x.set(std::nullopt);
    EnvGuard h("HOME", "/home/someone");
    REQUIRE(defaultModelsDirectory() == fs::path("/home/someone/.local/share/sawblade/models"));
    REQUIRE(defaultStemsDirectory() == fs::path("/home/someone/.local/share/sawblade/stems"));
  }
#endif
  SECTION("ids, file names, fetch commands") {
    REQUIRE(std::string(separationModelId(SeparationModel::Htdemucs6s)) == "htdemucs_6s");
    REQUIRE(std::string(separationModelId(SeparationModel::Htdemucs4s)) == "htdemucs");
    REQUIRE(separationModelFetchCommand(SeparationModel::Htdemucs6s) == "match/.venv/bin/sawblade-models fetch --model htdemucs_6s");
    REQUIRE(ModelStore("/m").modelPath(SeparationModel::Htdemucs4s) == fs::path("/m/htdemucs-core-opset17.onnx"));
    REQUIRE(std::string(separationModelPinnedSha256(SeparationModel::Htdemucs4s)) ==
            "79189af3c584b1a2145ae5e4182a50c0204f88b76e2829bd27e4d4a88ede427d");
    REQUIRE(std::string(separationModelPinnedSha256(SeparationModel::Htdemucs6s)) ==
            "d23996ba2e9396d393e2bd53c29f1411bd33b8cf3451854ad32d746ad3d06132");
  }
}

TEST_CASE("ModelStore: missing, mismatch, sidecar", "[separation][modelstore]") {
  StemTempDir t;
  const ModelStore store(t.dir);
  const auto m = SeparationModel::Htdemucs6s;

  const ModelStatus missing = store.check(m);
  REQUIRE(missing.state == ModelStatus::State::Missing);
  REQUIRE_FALSE(missing.ok());
  REQUIRE_THAT(missing.message, ContainsSubstring("match/.venv/bin/sawblade-models fetch --model htdemucs_6s"));
  REQUIRE_THAT(missing.message, ContainsSubstring(store.modelPath(m).string()));

  writeText(store.modelPath(m), "pretend onnx bytes");
  const ModelStatus bad = store.check(m);  // no sidecar, not the pinned hash
  REQUIRE(bad.state == ModelStatus::State::Mismatch);
  REQUIRE(bad.actualSha256 == sha256Hex("pretend onnx bytes", 18));
  REQUIRE_THAT(bad.message, ContainsSubstring("sawblade-models fetch --model htdemucs_6s"));

  writeText(store.modelPath(m).string() + ".sha256", std::string(64, '0') + "\n");  // wrong sidecar
  REQUIRE(store.check(m).state == ModelStatus::State::Mismatch);

  writeText(store.modelPath(m).string() + ".sha256", bad.actualSha256 + "\n");
  const ModelStatus ok = store.check(m);
  REQUIRE(ok.ok());
  REQUIRE(ok.matchedSidecar);
  REQUIRE_FALSE(ok.matchedPinned);

  // The in-process hash cache is keyed by (path, size, mtime): a changed file is re-hashed.
  writeText(store.modelPath(m), "pretend onnx bytes, now longer");
  REQUIRE(store.check(m).state == ModelStatus::State::Mismatch);

  // The other model is independent.
  REQUIRE(store.check(SeparationModel::Htdemucs4s).state == ModelStatus::State::Missing);
}

TEST_CASE("StemCache: key, staging, atomic publish", "[separation][cache]") {
  StemTempDir t;
  const fs::path audio = t / "song.bin";
  writeText(audio, "some audio bytes");
  const StemCache cache(t / "stems");
  const auto m6 = SeparationModel::Htdemucs6s, m4 = SeparationModel::Htdemucs4s;

  const std::string k6 = StemCache::keyFor(audio, m6), k4 = StemCache::keyFor(audio, m4);
  REQUIRE(k6 == sha256File(audio) + "-htdemucs_6s");
  REQUIRE(k4 == sha256File(audio) + "-htdemucs");
  REQUIRE(k6 != k4);
  REQUIRE_THROWS_AS(StemCache::keyFor(t / "missing.mp3", m6), std::runtime_error);
  REQUIRE(stemCacheFileNames(m6).size() == 5);
  REQUIRE(stemCacheFileNames(m4).size() == 4);

  REQUIRE_FALSE(cache.lookup(k6, m6));

  fs::path staged;
  {  // an abandoned staging directory disappears
    StemCache::Staging s = cache.beginStaging(k6);
    staged = s.dir();
    REQUIRE(fs::is_directory(staged));
    writeText(s.dir() / "drums.wav", "x");
  }
  REQUIRE_FALSE(fs::exists(staged));
  REQUIRE_FALSE(cache.lookup(k6, m6));

  {  // complete entry is published atomically
    StemCache::Staging s = cache.beginStaging(k6);
    for (const auto& f : stemCacheFileNames(m6)) writeText(s.dir() / f, f);
    REQUIRE_FALSE(cache.lookup(k6, m6));  // not visible before commit
    const fs::path dst = cache.commit(s, k6, m6);
    REQUIRE(dst == cache.entryDir(k6));
    REQUIRE(s.dir().empty());
  }
  REQUIRE(cache.lookup(k6, m6));
  REQUIRE_FALSE(cache.lookup(k4, m4));  // different model, different key
  // Only the entry is left in the cache root (no staging leftovers).
  std::size_t entries = 0;
  for (const auto& e : fs::directory_iterator(t / "stems")) {
    (void)e;
    ++entries;
  }
  REQUIRE(entries == 1);

  {  // a second, complete commit of the same key keeps the first
    StemCache::Staging s = cache.beginStaging(k6);
    for (const auto& f : stemCacheFileNames(m6)) writeText(s.dir() / f, "second");
    cache.commit(s, k6, m6);
  }
  std::ifstream first(cache.entryDir(k6) / "drums.wav");
  std::string content((std::istreambuf_iterator<char>(first)), std::istreambuf_iterator<char>());
  REQUIRE(content == "drums.wav");

  // An incomplete leftover entry is not a hit and is replaced.
  fs::remove(cache.entryDir(k6) / "guitar.wav");
  REQUIRE_FALSE(cache.lookup(k6, m6));
  {
    StemCache::Staging s = cache.beginStaging(k6);
    for (const auto& f : stemCacheFileNames(m6)) writeText(s.dir() / f, "third");
    cache.commit(s, k6, m6);
  }
  REQUIRE(cache.lookup(k6, m6));
}

TEST_CASE("StemCache entries load with loadStemDirectory (4-stem other maps to guitar, 6-stem keeps guitar)", "[separation][cache]") {
  StemTempDir t;
  const std::vector<float> a = noise(4410, 1), b = noise(4410, 2);
  for (const char* sub : {"four", "six"}) {
    fs::create_directories(t / sub);
    for (const auto& f : stemCacheFileNames(std::string(sub) == "six" ? SeparationModel::Htdemucs6s : SeparationModel::Htdemucs4s))
      writeWavFloat32Stereo(t / sub / f, 44100.0, a, b);
  }
  const StemSet four = loadStemDirectory(t / "four", 44100.0);
  REQUIRE(four.otherMappedToGuitar);
  REQUIRE(four.present[static_cast<std::size_t>(StemKind::Guitar)]);
  REQUIRE_FALSE(four.present[static_cast<std::size_t>(StemKind::Other)]);
  const StemSet fourKeep = loadStemDirectory(t / "four", 44100.0, OtherRole::Other);
  REQUIRE(fourKeep.present[static_cast<std::size_t>(StemKind::Other)]);
  const StemSet six = loadStemDirectory(t / "six", 44100.0);
  REQUIRE_FALSE(six.otherMappedToGuitar);
  REQUIRE(six.present[static_cast<std::size_t>(StemKind::Guitar)]);
  REQUIRE(six.present[static_cast<std::size_t>(StemKind::Other)]);
}
