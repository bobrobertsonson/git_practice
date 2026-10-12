// Phase 5.1b: the Separator, separateSong and the CLI tools, against a synthetic ONNX model with the real
// graph's I/O (tests/onnx_synth.h). No weights, no Python. See also test_separation_support.cpp.
//
// Reference for a linear model: stem s = g[s] * (x - m) + m with g = timeGain + freqGain and m the mean of
// the mono mix (the separator normalises per song with an affine map). With freqGain = 0 the network is a
// pure time-domain gain and the whole-song result is exact; with a frequency path the STFT / iSTFT pair
// is only the identity away from each segment's edges (as in demucs), so those tests allow -40 dB.
#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "onnx_synth.h"
#include "sawblade/resample.h"
#include "sawblade/separate_song.h"
#include "sawblade/separator.h"
#include "sawblade/sha256.h"
#include "sawblade/stem_set.h"
#include "sawblade/wav_io.h"
#include "stem_test_util.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

constexpr int kSegLen = 343980;
constexpr int kStride = 257985;

// htdemucs source order: drums, bass, other, vocals, guitar, piano.
const std::vector<float> kTime6 = {0.5f, -0.25f, 0.8f, 0.1f, 0.35f, 0.2f};
const std::vector<float> kZero6(6, 0.0f);
const std::vector<float> kFreq6 = {0.25f, 0.5f, -0.4f, 0.3f, 0.15f, 0.1f};
const std::vector<float> kTime4 = {0.5f, -0.25f, 0.8f, 0.1f};
const std::vector<float> kZero4(4, 0.0f);
const std::vector<float> kFreq4 = {0.25f, 0.5f, -0.4f, 0.3f};

fs::path writeModel(const fs::path& dir, SeparationModel m, const std::vector<float>& timeGain, const std::vector<float>& freqGain,
                    bool sidecar = true) {
  fs::create_directories(dir);
  const fs::path p = ModelStore(dir).modelPath(m);
  const std::string bytes = onnx_synth::buildCoreModel(timeGain, freqGain);
  onnx_synth::write(p, bytes);
  if (sidecar) std::ofstream(p.string() + ".sha256") << sha256Hex(bytes.data(), bytes.size()) << "\n";
  return p;
}

// A stereo song with a DC offset so the per-song mean matters; L and R differ.
// `noNyquist`: the noise is low-passed with [1 2 1] / 4, which has a zero at Nyquist exactly. HTDemucs drops the
// Nyquist bin of its spectrogram, so only such a signal can come back through the STFT path unchanged.
AudioFile makeSong(std::size_t frames, double rate = 44100.0, unsigned seed = 7, bool noNyquist = false) {
  auto a = noise(frames, seed, 0.3f), b = noise(frames, seed + 1, 0.2f);
  if (noNyquist)
    for (auto* v : {&a, &b}) {
      const std::vector<float> src = *v;
      for (std::size_t i = 0; i < frames; ++i)
        (*v)[i] = 0.25f * src[i > 0 ? i - 1 : 0] + 0.5f * src[i] + 0.25f * src[i + 1 < frames ? i + 1 : frames - 1];
    }
  const auto s = sine(220.0, rate, frames, 0.2);
  AudioFile f;
  f.sampleRate = rate;
  f.channels = 2;
  f.interleaved.resize(frames * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    f.interleaved[2 * i] = a[i] + s[i] + 0.05f;
    f.interleaved[2 * i + 1] = b[i] - 0.5f * s[i] + 0.05f;
  }
  return f;
}

StemAudio planar(const AudioFile& f) {
  const std::size_t n = f.interleaved.size() / static_cast<std::size_t>(f.channels);
  StemAudio a{std::vector<float>(n), std::vector<float>(n)};
  for (std::size_t i = 0; i < n; ++i)
    for (std::size_t c = 0; c < 2; ++c) a[c][i] = f.interleaved[i * static_cast<std::size_t>(f.channels) + (f.channels == 1 ? 0 : c)];
  return a;
}

// Expected stem for gain g from the 44.1 kHz planar input. Demucs adds the song mean to every source when it
// undoes the normalisation, so `other` + piano carries it twice (meanCopies).
StemAudio expected(const StemAudio& x, double g, double meanCopies = 1.0) {
  double sum = 0;
  const std::size_t n = x[0].size();
  for (std::size_t i = 0; i < n; ++i) sum += 0.5 * (static_cast<double>(x[0][i]) + x[1][i]);
  const double m = sum / static_cast<double>(n);
  StemAudio e{std::vector<float>(n), std::vector<float>(n)};
  for (std::size_t c = 0; c < 2; ++c)
    for (std::size_t i = 0; i < n; ++i) e[c][i] = static_cast<float>(g * (static_cast<double>(x[c][i]) - m) + meanCopies * m);
  return e;
}

// Null of `got` against `ref` in dB relative to the reference's RMS, over [from, n - from) and outside
// +-2500 samples of every segment edge (k * stride and k * stride + segment length) when `skipSegmentEdges`.
double nullDb(const StemAudio& got, const StemAudio& ref, std::size_t from = 0, bool skipSegmentEdges = false) {
  REQUIRE(got[0].size() == ref[0].size());
  auto skipped = [&](std::size_t i) {
    if (!skipSegmentEdges) return false;
    for (std::size_t k = 0; k * kStride < ref[0].size() + kStride; ++k) {
      for (const std::size_t e : {k * kStride, k * kStride + kSegLen})
        if (i + 2500 >= e && i <= e + 2500) return true;
    }
    return false;
  };
  double num = 0, den = 0;
  for (std::size_t c = 0; c < 2; ++c)
    for (std::size_t i = from; i + from < ref[c].size(); ++i) {
      if (skipped(i)) continue;
      const double d = static_cast<double>(got[c][i]) - ref[c][i];
      num += d * d;
      den += static_cast<double>(ref[c][i]) * ref[c][i];
    }
  return 10.0 * std::log10(std::max(num, 1e-30) / den);
}

const StemAudio& stem(const SeparationResult& r, StemKind k) {
  const auto& o = r.stems[static_cast<std::size_t>(k)];
  REQUIRE(o.has_value());
  return *o;
}

struct Case {
  SeparationModel model;
  const std::vector<float>& time;
  const std::vector<float>& freq;
};

SeparationResult run(const fs::path& model, SeparationModel m, AudioFile song, int threads = 2) {
  Separator sep(model, m, SeparatorOptions{threads});
  CancelToken cancel;
  return sep.separate(std::move(song), nullptr, cancel);
}

// Expected gain per StemKind for the model's source order.
double gainFor(StemKind k, const std::vector<float>& t, const std::vector<float>& f) {
  auto g = [&](std::size_t s) { return static_cast<double>(t[s]) + f[s]; };
  switch (k) {
    case StemKind::Drums: return g(0);
    case StemKind::Bass: return g(1);
    case StemKind::Other: return t.size() == 6 ? g(2) + g(5) : g(2);  // piano summed into other
    case StemKind::Vocals: return g(3);
    case StemKind::Guitar: return g(4);
  }
  return 0;
}

double meanCopiesFor(StemKind k, const std::vector<float>& t) { return k == StemKind::Other && t.size() == 6 ? 2.0 : 1.0; }

std::string q(const fs::path& p) { return "'" + p.string() + "'"; }

int runCmd(const std::string& cmd) {
  const int st = std::system(cmd.c_str());
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("Separator: segmenting and overlap-add reproduce the unsegmented reference (time-domain model)", "[separator][null]") {
  StemTempDir t;
  for (const bool six : {true, false}) {
    const auto m = six ? SeparationModel::Htdemucs6s : SeparationModel::Htdemucs4s;
    const auto& tg = six ? kTime6 : kTime4;
    const auto& zg = six ? kZero6 : kZero4;
    const fs::path model = writeModel(t / (six ? "m6" : "m4"), m, tg, zg);
    // 1.5 s (shorter than a segment), exactly one segment, and 3 whole segments plus a ragged tail.
    for (const std::size_t frames : {std::size_t{66150}, std::size_t{kSegLen}, std::size_t{3 * kStride + 12345}}) {
      INFO((six ? "6-stem" : "4-stem") << ", frames " << frames);
      const AudioFile song = makeSong(frames);
      const StemAudio x = planar(song);
      const SeparationResult r = run(model, m, song);
      REQUIRE(r.length == static_cast<std::int64_t>(frames));
      REQUIRE(r.segments == static_cast<int>((frames + kStride - 1) / kStride));
      REQUIRE(r.stems[static_cast<std::size_t>(StemKind::Guitar)].has_value() == six);
      for (const StemKind k : {StemKind::Drums, StemKind::Bass, StemKind::Vocals, StemKind::Other, StemKind::Guitar}) {
        if (k == StemKind::Guitar && !six) continue;
        INFO(stemKindName(k));
        const StemAudio& got = stem(r, k);
        REQUIRE(got[0].size() == frames);
        REQUIRE(nullDb(got, expected(x, gainFor(k, tg, zg), meanCopiesFor(k, tg))) < -90.0);
      }
    }
  }
}

TEST_CASE("Separator: STFT / iSTFT path (frequency-domain model)", "[separator][null]") {
  StemTempDir t;
  for (const bool six : {true, false}) {
    const auto m = six ? SeparationModel::Htdemucs6s : SeparationModel::Htdemucs4s;
    const auto& zg = six ? kZero6 : kZero4;
    const auto& fg = six ? kFreq6 : kFreq4;
    const fs::path model = writeModel(t / (six ? "m6" : "m4"), m, zg, fg);
    const AudioFile song = makeSong(2 * kStride + 50000, 44100.0, 7, /*noNyquist=*/true);
    const StemAudio x = planar(song);
    const SeparationResult r = run(model, m, song);
    for (const StemKind k : {StemKind::Drums, StemKind::Bass, StemKind::Vocals, StemKind::Other, StemKind::Guitar}) {
      if (k == StemKind::Guitar && !six) continue;
      INFO((six ? "6-stem " : "4-stem ") << stemKindName(k));
      const StemAudio ref = expected(x, gainFor(k, zg, fg), meanCopiesFor(k, zg));
      // Over the whole song, segment edges included, the edge effect of the iSTFT (as in demucs) limits the null.
      REQUIRE(nullDb(stem(r, k), ref, 4096) < -40.0);
      // Away from the segment edges the STFT / iSTFT pair is the identity to float precision.
      const double db = nullDb(stem(r, k), ref, 4096, true);
      REQUIRE(db < -100.0);
    }
  }
}

TEST_CASE("Separator: stems stream to the sink as contiguous ranges, bit-identical to the collected result", "[separator][stream]") {
  StemTempDir t;
  const fs::path model = writeModel(t / "m", SeparationModel::Htdemucs6s, kTime6, kFreq6);
  const AudioFile song = makeSong(3 * kStride + 777);
  const SeparationResult whole = run(model, SeparationModel::Htdemucs6s, song);

  struct Rec final : StemSink {
    std::int64_t len = 0;
    bool guitar = false;
    std::array<std::int64_t, kStemKindCount> next{};
    std::array<StemAudio, kStemKindCount> data;
    std::size_t calls = 0;
    std::int64_t maxChunk = 0;
    void begin(std::int64_t l, bool g) override { len = l; guitar = g; }
    void write(StemKind k, std::int64_t off, const float* l, const float* r, std::int64_t n) override {
      const auto i = static_cast<std::size_t>(k);
      REQUIRE(off == next[i]);  // consecutive and increasing
      next[i] += n;
      ++calls;
      maxChunk = std::max(maxChunk, n);
      data[i][0].insert(data[i][0].end(), l, l + n);
      data[i][1].insert(data[i][1].end(), r, r + n);
    }
  } rec;
  Separator sep(model, SeparationModel::Htdemucs6s, SeparatorOptions{2});
  CancelToken c;
  sep.separate(song, rec, nullptr, c);
  REQUIRE(rec.guitar);
  REQUIRE(rec.len == static_cast<std::int64_t>(song.interleaved.size() / 2));
  REQUIRE(rec.maxChunk <= kStride);  // a window, not the whole song
  REQUIRE(rec.calls >= 4 * 5);
  for (const StemKind k : {StemKind::Drums, StemKind::Bass, StemKind::Vocals, StemKind::Other, StemKind::Guitar}) {
    REQUIRE(rec.next[static_cast<std::size_t>(k)] == rec.len);
    REQUIRE(rec.data[static_cast<std::size_t>(k)] == stem(whole, k));
  }
}

TEST_CASE("Separator: mono and 48 kHz input are duplicated to stereo and resampled to 44.1 kHz", "[separator][input]") {
  StemTempDir t;
  const fs::path model = writeModel(t / "m", SeparationModel::Htdemucs6s, kTime6, kZero6);
  AudioFile mono;
  mono.sampleRate = 48000.0;
  mono.channels = 1;
  mono.interleaved = noise(96000, 5, 0.3f);
  for (auto& v : mono.interleaved) v += 0.04f;
  const auto x44 = resample(mono.interleaved, 48000.0, 44100.0);
  const SeparationResult r = run(model, SeparationModel::Htdemucs6s, mono);
  REQUIRE(r.length == static_cast<std::int64_t>(x44.size()));
  const StemAudio& drums = stem(r, StemKind::Drums);
  REQUIRE(drums[0] == drums[1]);  // duplicated mono stays identical in both channels
  REQUIRE(nullDb(drums, expected(StemAudio{x44, x44}, kTime6[0])) < -90.0);
}

TEST_CASE("Separator: results are deterministic and independent of the thread count", "[separator][determinism]") {
  StemTempDir t;
  const fs::path model = writeModel(t / "m", SeparationModel::Htdemucs6s, kTime6, kFreq6);
  const AudioFile song = makeSong(kStride + 30000);
  const SeparationResult a = run(model, SeparationModel::Htdemucs6s, song, 1);
  const SeparationResult b = run(model, SeparationModel::Htdemucs6s, song, 1);
  const SeparationResult c = run(model, SeparationModel::Htdemucs6s, song, 3);
  for (const StemKind k : {StemKind::Drums, StemKind::Bass, StemKind::Vocals, StemKind::Other, StemKind::Guitar}) {
    REQUIRE(stem(a, k) == stem(b, k));
    REQUIRE(stem(a, k) == stem(c, k));
  }
}

TEST_CASE("Separator: progress is reported at least once per segment, with an ETA", "[separator][progress]") {
  StemTempDir t;
  const fs::path model = writeModel(t / "m", SeparationModel::Htdemucs6s, kTime6, kZero6);
  Separator sep(model, SeparationModel::Htdemucs6s, SeparatorOptions{2});
  CancelToken cancel;
  std::vector<std::pair<double, double>> calls;
  const std::thread::id caller = std::this_thread::get_id();
  bool sameThread = true;
  const std::size_t frames = 2 * kStride + 1000;  // 3 segments
  (void)sep.separate(makeSong(frames), [&](double f, double eta) {
    calls.emplace_back(f, eta);
    sameThread = sameThread && std::this_thread::get_id() == caller;
  }, cancel);
  REQUIRE(sameThread);
  REQUIRE(calls.size() == 4);  // 0 before the first segment, then one per segment
  REQUIRE(calls.front().first == 0.0);
  for (std::size_t i = 1; i < calls.size(); ++i) {
    REQUIRE(calls[i].first > calls[i - 1].first);
    REQUIRE(calls[i].second >= 0.0);
  }
  REQUIRE(calls.back().first == 1.0);
  REQUIRE(calls.back().second == 0.0);
}

TEST_CASE("Separator: cancel", "[separator][cancel]") {
  StemTempDir t;
  const fs::path model = writeModel(t / "m", SeparationModel::Htdemucs6s, kTime6, kZero6);
  Separator sep(model, SeparationModel::Htdemucs6s, SeparatorOptions{2});

  SECTION("before the first segment: nothing is processed") {
    CancelToken cancel;
    cancel.cancel();
    int calls = 0;
    REQUIRE_THROWS_AS(sep.separate(makeSong(kStride), [&](double, double) { ++calls; }, cancel), SeparationCancelled);
    REQUIRE(calls == 0);
  }
  SECTION("from the progress callback: stops before the next segment") {
    CancelToken cancel;
    int calls = 0;
    REQUIRE_THROWS_AS(sep.separate(makeSong(3 * kStride), [&](double f, double) {
                        ++calls;
                        if (f > 0.0) cancel.cancel();  // after the first segment
                      }, cancel),
                      SeparationCancelled);
    REQUIRE(calls == 2);
  }
  SECTION("from another thread, mid-job: returns promptly, the separator stays usable") {
    CancelToken cancel;
    std::atomic<bool> firstDone{false};
    std::promise<std::chrono::steady_clock::time_point> returned;
    auto fut = returned.get_future();
    std::thread worker([&] {
      try {
        (void)sep.separate(makeSong(8 * kStride), [&](double f, double) { if (f > 0.0) firstDone = true; }, cancel);
        returned.set_value({});  // not cancelled: test failure below
      } catch (const SeparationCancelled&) {
        returned.set_value(std::chrono::steady_clock::now());
      }
    });
    while (!firstDone) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto t0 = std::chrono::steady_clock::now();
    cancel.cancel();
    const auto done = fut.get();
    worker.join();
    REQUIRE(done != std::chrono::steady_clock::time_point{});
    REQUIRE(std::chrono::duration<double>(done - t0).count() < 5.0);
    CancelToken fresh;
    REQUIRE_NOTHROW(sep.separate(makeSong(20000), nullptr, fresh));
  }
}

TEST_CASE("Separator: construction errors", "[separator]") {
  StemTempDir t;
  SECTION("not an onnx file") {
    std::ofstream(t / "junk.onnx") << "junk";
    REQUIRE_THROWS_AS(Separator(t / "junk.onnx", SeparationModel::Htdemucs6s), std::runtime_error);
    REQUIRE_THROWS_AS(Separator(t / "missing.onnx", SeparationModel::Htdemucs6s), std::runtime_error);
  }
  SECTION("source count does not match the model id") {
    const fs::path m4 = writeModel(t / "m", SeparationModel::Htdemucs4s, kTime4, kZero4);
    REQUIRE_THROWS_WITH(Separator(m4, SeparationModel::Htdemucs6s), ContainsSubstring("expected output shape"));
  }
  SECTION("empty audio") {
    const fs::path m6 = writeModel(t / "m", SeparationModel::Htdemucs6s, kTime6, kZero6);
    Separator sep(m6, SeparationModel::Htdemucs6s, SeparatorOptions{1});
    CancelToken c;
    AudioFile empty;
    empty.sampleRate = 44100.0;
    empty.channels = 2;
    REQUIRE_THROWS_AS(sep.separate(empty, nullptr, c), std::runtime_error);
  }
  SECTION("thread option") {
    const fs::path m6 = writeModel(t / "m", SeparationModel::Htdemucs6s, kTime6, kZero6);
    REQUIRE(Separator(m6, SeparationModel::Htdemucs6s, SeparatorOptions{3}).threads() == 3);
    REQUIRE(Separator(m6, SeparationModel::Htdemucs6s).threads() == defaultSeparatorThreads());
    REQUIRE(defaultSeparatorThreads() >= 1);
  }
}

TEST_CASE("separateSong: cache miss, hit without the model, per-model keys, cancel leaves nothing", "[separator][cache]") {
  StemTempDir t;
  const fs::path models = t / "models", stems = t / "stems";
  const fs::path m6 = writeModel(models, SeparationModel::Htdemucs6s, kTime6, kZero6);
  writeModel(models, SeparationModel::Htdemucs4s, kTime4, kZero4);
  const fs::path song = t / "song.wav";
  const AudioFile a = makeSong(kStride + 20000);
  writeWavFloat32Stereo(song, 44100.0, planar(a)[0], planar(a)[1]);

  SeparateSongOptions o;
  o.modelsDir = models;
  o.stemsDir = stems;
  o.threads = 2;

  SECTION("miss then hit") {
    CancelToken c1;
    int calls1 = 0;
    const SeparateSongResult r1 = separateSong(song, o, [&](double, double) { ++calls1; }, c1);
    REQUIRE_FALSE(r1.cacheHit);
    REQUIRE(calls1 >= 2);
    REQUIRE(r1.stemsDir == stems / (sha256File(song) + "-htdemucs_6s"));
    for (const auto& f : stemCacheFileNames(SeparationModel::Htdemucs6s)) REQUIRE(fs::is_regular_file(r1.stemsDir / f));
    // Only the entry is in the cache root.
    std::size_t entries = 0;
    for (const auto& e : fs::directory_iterator(stems)) { (void)e; ++entries; }
    REQUIRE(entries == 1);

    // loadStemDirectory loads it unchanged; the stem content is the separated audio.
    const StemSet set = loadStemDirectory(r1.stemsDir, 44100.0);
    REQUIRE(set.present[static_cast<std::size_t>(StemKind::Guitar)]);
    REQUIRE(set.length == static_cast<std::int64_t>(a.interleaved.size() / 2));
    REQUIRE(nullDb(set.audio[static_cast<std::size_t>(StemKind::Drums)], expected(planar(a), kTime6[0])) < -90.0);

    // Second load: a hit, no separation, the model is not even needed.
    fs::remove(m6);
    CancelToken c2;
    std::vector<double> fractions;
    const SeparateSongResult r2 = separateSong(song, o, [&](double f, double) { fractions.push_back(f); }, c2);
    REQUIRE(r2.cacheHit);
    REQUIRE(r2.stemsDir == r1.stemsDir);
    REQUIRE(r2.separateSeconds == 0.0);
    REQUIRE(fractions == std::vector<double>{1.0});
  }
  SECTION("4-stem model has its own key and writes four files") {
    o.model = SeparationModel::Htdemucs4s;
    CancelToken c;
    const SeparateSongResult r = separateSong(song, o, nullptr, c);
    REQUIRE(r.key == sha256File(song) + "-htdemucs");
    REQUIRE(fs::is_regular_file(r.stemsDir / "other.wav"));
    REQUIRE_FALSE(fs::exists(r.stemsDir / "guitar.wav"));
    const StemSet set = loadStemDirectory(r.stemsDir, 44100.0);
    REQUIRE(set.otherMappedToGuitar);
  }
  SECTION("cancel mid-job leaves no cache entry and no staging directory") {
    CancelToken c;
    REQUIRE_THROWS_AS(separateSong(song, o, [&](double f, double) { if (f > 0.0) c.cancel(); }, c), SeparationCancelled);
    std::size_t entries = 0;
    if (fs::exists(stems))
      for (const auto& e : fs::directory_iterator(stems)) { (void)e; ++entries; }
    REQUIRE(entries == 0);
  }
  SECTION("missing or wrong model: ModelUnavailable with the exact fetch command") {
    o.modelsDir = t / "nomodels";
    CancelToken c;
    try {
      separateSong(song, o, nullptr, c);
      FAIL("expected ModelUnavailable");
    } catch (const ModelUnavailable& e) {
      REQUIRE(e.status.state == ModelStatus::State::Missing);
      REQUIRE_THAT(e.what(), ContainsSubstring("match/.venv/bin/sawblade-models fetch --model htdemucs_6s"));
    }
    writeModel(t / "badmodels", SeparationModel::Htdemucs6s, kTime6, kZero6, /*sidecar=*/false);
    o.modelsDir = t / "badmodels";
    REQUIRE_THROWS_AS(separateSong(song, o, nullptr, c), ModelUnavailable);
    REQUIRE_FALSE(fs::exists(stems));  // nothing written
  }
  SECTION("custom decoder replaces the built-in one (plugin m4a path)") {
    const fs::path fake = t / "song.m4a";
    std::ofstream(fake) << "not decodable by core";
    int used = 0;
    o.decoder = [&](const fs::path&) { ++used; return a; };
    CancelToken c;
    const SeparateSongResult r = separateSong(fake, o, nullptr, c);
    REQUIRE(used == 1);
    REQUIRE(fs::is_regular_file(r.stemsDir / "guitar.wav"));
    // Without it, core says m4a is unsupported.
    o.decoder = nullptr;
    o.stemsDir = t / "stems2";
    REQUIRE_THROWS_WITH(separateSong(fake, o, nullptr, c), ContainsSubstring("unsupported"));
  }
}

TEST_CASE("CLI: sawblade-stems and tonerender --separate", "[separator][cli]") {
  StemTempDir t;
  const fs::path models = t / "models", stems = t / "stems", out = t / "out";
  const AudioFile a = makeSong(kStride / 2);
  writeWavFloat32Stereo(t / "song.wav", 44100.0, planar(a)[0], planar(a)[1]);
  const std::string env = "SAWBLADE_MODELS_DIR=" + q(models) + " SAWBLADE_STEMS_DIR=" + q(stems);
  const std::string stemsTool = "env " + env + " " + q(SAWBLADE_SEPARATE_EXE);
  const std::string render = "env " + env + " " + q(SAWBLADE_TONERENDER_EXE);

  SECTION("missing model: non-zero exit and the exact fetch command on stderr") {
    const int rc = runCmd(stemsTool + " " + q(t / "song.wav") + " " + q(out) + " >/dev/null 2>" + q(t / "err.txt"));
    REQUIRE(rc != 0);
    REQUIRE_THAT(slurp(t / "err.txt"), ContainsSubstring("match/.venv/bin/sawblade-models fetch --model htdemucs_6s"));
    const int rc2 = runCmd(render + " --separate " + q(t / "song.wav") + " --stems-out " + q(out) + " >/dev/null 2>" + q(t / "err2.txt"));
    REQUIRE(rc2 != 0);
    REQUIRE_THAT(slurp(t / "err2.txt"), ContainsSubstring("sawblade-models fetch --model htdemucs_6s"));
  }
  SECTION("separates, prints progress, copies the stems, and the second run is a cache hit") {
    writeModel(models, SeparationModel::Htdemucs6s, kTime6, kZero6);
    REQUIRE(runCmd(stemsTool + " " + q(t / "song.wav") + " " + q(out) + " --threads 2 >" + q(t / "o1.txt") + " 2>" + q(t / "e1.txt")) == 0);
    for (const auto& f : stemCacheFileNames(SeparationModel::Htdemucs6s)) REQUIRE(fs::is_regular_file(out / f));
    REQUIRE_THAT(slurp(t / "e1.txt") + slurp(t / "o1.txt"), ContainsSubstring("%"));
    REQUIRE_THAT(slurp(t / "o1.txt"), ContainsSubstring("cache miss"));
    REQUIRE(runCmd(stemsTool + " " + q(t / "song.wav") + " " + q(t / "out2") + " >" + q(t / "o2.txt") + " 2>" + q(t / "e2.txt")) == 0);
    REQUIRE_THAT(slurp(t / "o2.txt"), ContainsSubstring("cache hit"));
    REQUIRE(slurp(out / "drums.wav") == slurp(t / "out2" / "drums.wav"));

    // tonerender --separate with the 4-stem model needs its own model file.
    writeModel(models, SeparationModel::Htdemucs4s, kTime4, kZero4);
    REQUIRE(runCmd(render + " --separate " + q(t / "song.wav") + " --stems-out " + q(t / "out3") + " --model htdemucs --threads 2 >" +
                   q(t / "o3.txt") + " 2>" + q(t / "e3.txt")) == 0);
    REQUIRE(fs::is_regular_file(t / "out3" / "other.wav"));
    REQUIRE_FALSE(fs::exists(t / "out3" / "guitar.wav"));
  }
  SECTION("bad flags") {
    REQUIRE(runCmd(stemsTool + " >/dev/null 2>&1") != 0);
    REQUIRE(runCmd(stemsTool + " " + q(t / "song.wav") + " " + q(out) + " --model nope >/dev/null 2>&1") != 0);
    REQUIRE(runCmd(stemsTool + " " + q(t / "song.wav") + " " + q(out) + " --threads 0 >/dev/null 2>&1") != 0);
    REQUIRE(runCmd(render + " --stems-out " + q(out) + " >/dev/null 2>&1") != 0);
  }
}

// Real model, gated: SAWBLADE_SEPARATOR_REAL_TEST=1. Separates a 10 s clip with the model from the model store
// and nulls every stem against the spike's separator_onnx output for the same clip (<= -40 dB per stem).
//   SAWBLADE_SEPARATOR_REAL_CLIP  44.1 kHz stereo WAV, e.g. a generated mixture:
//       ffmpeg -f lavfi -i "anoisesrc=d=10:c=pink:r=44100:a=0.15" -f lavfi -i "sine=f=196:d=10:r=44100"
//              -f lavfi -i "sine=f=1250:d=10:r=44100" -filter_complex "[0][1][2]amix=inputs=3:normalize=0,aformat=channel_layouts=stereo" clip.wav
//   SAWBLADE_SEPARATOR_REAL_REF   directory with the spike's stems of that clip:
//       <spike build>/spikes/separator/separator_onnx --model <models dir>/htdemucs_6s-core-opset17.onnx --in clip.wav --out-dir ref --threads 4
//       (spike build: cmake -DSAWBLADE_BUILD_SEPARATOR_SPIKE=ON -DSAWBLADE_BUILD_SEPARATOR_ONNX=ON -DSAWBLADE_BUILD_TESTS=OFF, target separator_onnx)
//   SAWBLADE_SEPARATOR_REAL_MODEL optional: htdemucs (4-stem; the spike reference must come from that model)
TEST_CASE("Separator: real htdemucs model nulls against the spike driver (env-gated)", "[separator][real]") {
  const char* gate = std::getenv("SAWBLADE_SEPARATOR_REAL_TEST");
  const char* clip = std::getenv("SAWBLADE_SEPARATOR_REAL_CLIP");
  const char* ref = std::getenv("SAWBLADE_SEPARATOR_REAL_REF");
  if (!gate || std::string(gate) != "1") SKIP("set SAWBLADE_SEPARATOR_REAL_TEST=1 (and _CLIP, _REF) to run");
  if (!clip || !ref) SKIP("SAWBLADE_SEPARATOR_REAL_CLIP and SAWBLADE_SEPARATOR_REAL_REF are required");
  const char* mid = std::getenv("SAWBLADE_SEPARATOR_REAL_MODEL");
  const SeparationModel m = (mid && std::string(mid) == "htdemucs") ? SeparationModel::Htdemucs4s : SeparationModel::Htdemucs6s;
  const ModelStatus st = ModelStore().check(m);
  if (!st.ok()) SKIP(st.message);

  const AudioFile song = readAudioFile(clip);
  REQUIRE(song.sampleRate == 44100.0);
  REQUIRE(song.channels == 2);
  const SeparationResult r = run(st.path, m, song, 4);
  auto load = [&](const char* name) {
    const AudioFile f = readAudioFile(fs::path(ref) / (std::string(name) + ".wav"));
    return planar(f);
  };
  struct Pair { StemKind kind; const char* file; const char* extra; };
  std::vector<Pair> pairs = {{StemKind::Drums, "drums", nullptr}, {StemKind::Bass, "bass", nullptr},
                             {StemKind::Vocals, "vocals", nullptr}, {StemKind::Other, "other", m == SeparationModel::Htdemucs6s ? "piano" : nullptr}};
  if (m == SeparationModel::Htdemucs6s) pairs.push_back({StemKind::Guitar, "guitar", nullptr});
  for (const auto& p : pairs) {
    StemAudio want = load(p.file);
    if (p.extra) {
      const StemAudio e = load(p.extra);
      for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < want[c].size(); ++i) want[c][i] += e[c][i];
    }
    const double db = nullDb(stem(r, p.kind), want);
    INFO(p.file << ": null " << db << " dB");
    REQUIRE(db <= -40.0);
  }
}
