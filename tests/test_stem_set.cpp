// Phase 5.1: StemSet loader (WAV / FLAC decode, resampling, directory scan).
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dr_wav.h"
#include "sawblade/resample.h"
#include "sawblade/stem_set.h"
#include "sawblade/wav_io.h"
#include "stem_test_util.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

bool hasWarning(const StemSet& s, const std::string& needle) {
  return std::any_of(s.warnings.begin(), s.warnings.end(), [&](const std::string& w) { return w.find(needle) != std::string::npos; });
}

void writeStereo(const fs::path& p, double fs, const std::vector<float>& l, const std::vector<float>& r) {
  writeWavFloat32Stereo(p, fs, l, r);
}

}  // namespace

TEST_CASE("writeWavFloat32Stereo round trip and errors", "[stems][wav]") {
  StemTempDir t;
  const auto l = noise(500, 1), r = noise(500, 2);
  writeStereo(t / "s.wav", 44100.0, l, r);
  const AudioFile f = readAudioFile(t / "s.wav");
  REQUIRE(f.channels == 2);
  REQUIRE(f.sampleRate == 44100.0);
  REQUIRE(f.interleaved.size() == 1000);
  for (std::size_t i = 0; i < 500; ++i) {
    REQUIRE(f.interleaved[2 * i] == l[i]);
    REQUIRE(f.interleaved[2 * i + 1] == r[i]);
  }
  REQUIRE_THROWS_AS(writeWavFloat32Stereo(t / "bad.wav", 44100.0, l, noise(499, 3)), std::runtime_error);
  REQUIRE_THROWS_WITH(readAudioFile(t / "x.mp3"), ContainsSubstring("x.mp3"));
  REQUIRE_THROWS_WITH(readAudioFile(t / "missing.wav"), ContainsSubstring("missing.wav"));
  REQUIRE_THROWS_WITH(readAudioFile(t / "missing.flac"), ContainsSubstring("missing.flac"));
}

TEST_CASE("FLAC decode returns the written samples exactly (mono and stereo)", "[stems][flac]") {
  StemTempDir t;
  auto pattern = [](std::size_t n, int seed) {
    std::vector<std::int16_t> v(n);
    unsigned s = static_cast<unsigned>(seed);
    for (auto& x : v) {
      s = s * 1664525u + 1013904223u;
      x = static_cast<std::int16_t>(static_cast<std::int32_t>(s >> 16) - 32768);
    }
    v[0] = -32768;
    if (n > 1) v[1] = 32767;
    return v;
  };
  // 10000 frames = two full 4096 blocks and one short one.
  const auto m = pattern(10000, 5), l = pattern(10000, 6), r = pattern(10000, 7);
  writeFlac16(t / "mono.flac", 44100, {m});
  writeFlac16(t / "stereo.FLAC", 48000, {l, r});

  const AudioFile fm = readAudioFile(t / "mono.flac");
  REQUIRE(fm.channels == 1);
  REQUIRE(fm.sampleRate == 44100.0);
  REQUIRE(fm.interleaved.size() == m.size());
  for (std::size_t i = 0; i < m.size(); ++i) REQUIRE(fm.interleaved[i] == static_cast<float>(m[i]) / 32768.0f);

  const AudioFile fs2 = readAudioFile(t / "stereo.FLAC");
  REQUIRE(fs2.channels == 2);
  REQUIRE(fs2.sampleRate == 48000.0);
  REQUIRE(fs2.interleaved.size() == 2 * l.size());
  for (std::size_t i = 0; i < l.size(); ++i) {
    REQUIRE(fs2.interleaved[2 * i] == static_cast<float>(l[i]) / 32768.0f);
    REQUIRE(fs2.interleaved[2 * i + 1] == static_cast<float>(r[i]) / 32768.0f);
  }
  // Corrupt file -> error naming the path.
  { std::ofstream(t / "bad.flac") << "not a flac"; }
  REQUIRE_THROWS_WITH(readAudioFile(t / "bad.flac"), ContainsSubstring("bad.flac"));
}

TEST_CASE("Loader: mono is duplicated, equal rate is bit-identical", "[stems][loader]") {
  StemTempDir t;
  const auto x = noise(3000, 11);
  writeWavFloat32(t / "m.wav", 48000.0, x);
  const StemSet s = loadStemFiles({{StemKind::Bass, t / "m.wav"}}, 48000.0);
  REQUIRE(s.sampleRate == 48000.0);
  REQUIRE(s.length == 3000);
  REQUIRE(s.present[static_cast<int>(StemKind::Bass)]);
  REQUIRE_FALSE(s.present[static_cast<int>(StemKind::Drums)]);
  REQUIRE(s.audio[static_cast<int>(StemKind::Bass)][0] == x);
  REQUIRE(s.audio[static_cast<int>(StemKind::Bass)][1] == x);
  REQUIRE(s.audio[static_cast<int>(StemKind::Drums)][0].empty());
  REQUIRE(s.sources[static_cast<int>(StemKind::Bass)].size() == 1);
  REQUIRE(s.sources[static_cast<int>(StemKind::Bass)][0].sourceChannels == 1);
  REQUIRE(s.sources[static_cast<int>(StemKind::Bass)][0].sourceRate == 48000.0);
  REQUIRE(s.sources[static_cast<int>(StemKind::Bass)][0].sourceFrames == 3000);

  const auto l = noise(2000, 12), r = noise(2000, 13);
  writeStereo(t / "st.wav", 48000.0, l, r);
  const StemSet s2 = loadStemFiles({{StemKind::Vocals, t / "st.wav"}}, 48000.0);
  REQUIRE(s2.audio[static_cast<int>(StemKind::Vocals)][0] == l);
  REQUIRE(s2.audio[static_cast<int>(StemKind::Vocals)][1] == r);
}

TEST_CASE("Loader: 44.1 kHz stem resampled to 48 kHz matches resample() bit-exactly", "[stems][loader][resample]") {
  StemTempDir t;
  const auto l = noise(4410, 21), r = noise(4410, 22);
  writeStereo(t / "d.wav", 44100.0, l, r);
  const StemSet s = loadStemFiles({{StemKind::Drums, t / "d.wav"}}, 48000.0);
  const std::size_t expected = resampledLength(4410, 44100.0, 48000.0);
  REQUIRE(static_cast<std::size_t>(s.length) == expected);
  REQUIRE(s.audio[0][0].size() == expected);
  REQUIRE(s.audio[0][0] == resample(l, 44100.0, 48000.0));
  REQUIRE(s.audio[0][1] == resample(r, 44100.0, 48000.0));
  REQUIRE(s.sources[0][0].sourceRate == 44100.0);
}

TEST_CASE("Loader: padding, summing, >2 channels and errors", "[stems][loader]") {
  StemTempDir t;
  const auto a = noise(1000, 31), b = noise(1500, 32), c = noise(800, 33);
  writeWavFloat32(t / "a.wav", 48000.0, a);
  writeWavFloat32(t / "b.wav", 48000.0, b);
  writeWavFloat32(t / "c.wav", 48000.0, c);
  const StemSet s = loadStemFiles({{StemKind::Drums, t / "a.wav"}, {StemKind::Drums, t / "b.wav"}, {StemKind::Other, t / "c.wav"}}, 48000.0);
  REQUIRE(s.length == 1500);
  REQUIRE(s.audio[0][0].size() == 1500);
  REQUIRE(s.audio[3][0].size() == 1500);  // padded
  for (std::size_t i = 0; i < 1500; ++i) {
    const float expect = (i < 1000 ? a[i] : 0.0f) + b[i];
    REQUIRE(s.audio[0][0][i] == expect);
  }
  for (std::size_t i = 800; i < 1500; ++i) REQUIRE(s.audio[3][1][i] == 0.0f);
  REQUIRE(s.sources[0].size() == 2);

  // 3 channels: the first two are kept, with a warning.
  {
    const auto c0 = noise(600, 51), c1 = noise(600, 52), c2 = noise(600, 53);
    std::vector<float> inter(1800);
    for (std::size_t i = 0; i < 600; ++i) {
      inter[3 * i] = c0[i];
      inter[3 * i + 1] = c1[i];
      inter[3 * i + 2] = c2[i];
    }
    drwav_data_format fmt;
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = 3;
    fmt.sampleRate = 48000;
    fmt.bitsPerSample = 32;
    drwav w;
    const std::string p = (t / "three.wav").string();
    REQUIRE(drwav_init_file_write(&w, p.c_str(), &fmt, nullptr));
    drwav_write_pcm_frames(&w, 600, inter.data());
    drwav_uninit(&w);
    const StemSet s3 = loadStemFiles({{StemKind::Other, t / "three.wav"}}, 48000.0);
    REQUIRE(s3.audio[3][0] == c0);
    REQUIRE(s3.audio[3][1] == c1);
    REQUIRE(hasWarning(s3, "three.wav"));
    REQUIRE(s3.sources[3][0].sourceChannels == 3);
  }
  REQUIRE_THROWS_WITH(loadStemFiles({}, 48000.0), ContainsSubstring("no stem"));
  REQUIRE_THROWS_WITH(loadStemFiles({{StemKind::Bass, t / "a.wav"}}, 0.0), ContainsSubstring("rate"));
  REQUIRE_THROWS_WITH(loadStemFiles({{StemKind::Bass, t / "nope.wav"}}, 48000.0), ContainsSubstring("nope.wav"));
  writeWavFloat32(t / "empty.wav", 48000.0, {});
  REQUIRE_THROWS_WITH(loadStemFiles({{StemKind::Bass, t / "empty.wav"}}, 48000.0), ContainsSubstring("empty.wav"));
}

TEST_CASE("Directory scan maps names, aliases, unknown files and sorts", "[stems][loader][dir]") {
  StemTempDir t;
  const auto d = noise(1000, 41), bs = noise(1000, 42), v = noise(1000, 43), o = noise(1000, 44), g = noise(1000, 45);
  const auto piano = noise(1200, 46), zed = noise(900, 47);
  writeWavFloat32(t / "Drums.WAV", 48000.0, d);
  writeWavFloat32(t / "bass.wav", 48000.0, bs);
  writeWavFloat32(t / "vocals.wav", 48000.0, v);
  writeWavFloat32(t / "other.wav", 48000.0, o);
  writeWavFloat32(t / "Guitars.wav", 48000.0, g);       // alias
  writeWavFloat32(t / "piano.wav", 48000.0, piano);     // unknown: summed into other
  writeWavFloat32(t / "zed.wav", 48000.0, zed);         // unknown, sorts after piano
  { std::ofstream(t / "notes.txt") << "ignored"; }
  fs::create_directories(t / "sub");
  writeWavFloat32(t / "sub" / "drums.wav", 48000.0, noise(5000, 48));  // not recursive

  const StemSet s = loadStemDirectory(t.dir, 48000.0);
  REQUIRE(s.length == 1200);
  for (int k = 0; k < kStemKindCount; ++k) REQUIRE(s.present[static_cast<std::size_t>(k)]);
  REQUIRE(s.audio[static_cast<int>(StemKind::Drums)][0].size() == 1200);
  REQUIRE(s.audio[static_cast<int>(StemKind::Drums)][0][0] == d[0]);
  REQUIRE(s.audio[static_cast<int>(StemKind::Guitar)][0][5] == g[5]);
  REQUIRE(s.sources[static_cast<int>(StemKind::Other)].size() == 3);
  // Summed in sorted name order: other.wav, piano.wav, zed.wav.
  const auto& so = s.sources[static_cast<int>(StemKind::Other)];
  REQUIRE(fs::path(so[0].file).filename() == "other.wav");
  REQUIRE(fs::path(so[1].file).filename() == "piano.wav");
  REQUIRE(fs::path(so[2].file).filename() == "zed.wav");
  REQUIRE(s.audio[static_cast<int>(StemKind::Other)][0][10] == (o[10] + piano[10]) + zed[10]);
  REQUIRE(hasWarning(s, "piano.wav"));
  REQUIRE(hasWarning(s, "zed.wav"));
  REQUIRE_FALSE(hasWarning(s, "drums"));
  REQUIRE(s.warnings.size() == 2);

  // Determinism: a second load is identical.
  const StemSet s2 = loadStemDirectory(t.dir, 48000.0);
  REQUIRE(s2.audio == s.audio);
  REQUIRE(s2.warnings == s.warnings);
}

TEST_CASE("Directory scan: errors", "[stems][loader][dir]") {
  StemTempDir t;
  REQUIRE_THROWS_WITH(loadStemDirectory(t / "missing", 48000.0), ContainsSubstring("missing"));
  REQUIRE_THROWS_WITH(loadStemDirectory(t.dir, 48000.0), ContainsSubstring("no .wav or .flac"));
  { std::ofstream(t / "readme.txt") << "x"; }
  REQUIRE_THROWS_AS(loadStemDirectory(t.dir, 48000.0), std::runtime_error);
  { std::ofstream(t / "drums.wav") << "garbage"; }
  REQUIRE_THROWS_WITH(loadStemDirectory(t.dir, 48000.0), ContainsSubstring("drums.wav"));
}

TEST_CASE("Directory scan decodes FLAC stems", "[stems][loader][dir][flac]") {
  StemTempDir t;
  std::vector<std::int16_t> v(5000);
  for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<std::int16_t>((i * 37) % 20000) - 10000;
  writeFlac16(t / "guitar.flac", 48000, {v, v});
  const StemSet s = loadStemDirectory(t.dir, 48000.0);
  REQUIRE(s.present[static_cast<int>(StemKind::Guitar)]);
  REQUIRE(s.length == 5000);
  REQUIRE(s.audio[static_cast<int>(StemKind::Guitar)][1][100] == static_cast<float>(v[100]) / 32768.0f);
}

TEST_CASE("makeStemSet validates and pads", "[stems][factory]") {
  std::array<std::optional<StemAudio>, kStemKindCount> a;
  REQUIRE_THROWS_AS(makeStemSet(48000.0, a), std::runtime_error);  // no stems
  a[0] = StemAudio{std::vector<float>(10, 1.0f), std::vector<float>(10, 2.0f)};
  a[4] = StemAudio{std::vector<float>(25, 3.0f), std::vector<float>(25, 4.0f)};
  const StemSet s = makeStemSet(44100.0, a);
  REQUIRE(s.sampleRate == 44100.0);
  REQUIRE(s.length == 25);
  REQUIRE(s.audio[0][0].size() == 25);
  REQUIRE(s.audio[0][1][9] == 2.0f);
  REQUIRE(s.audio[0][1][10] == 0.0f);
  REQUIRE(s.audio[4][0][24] == 3.0f);
  REQUIRE_FALSE(s.present[1]);
  REQUIRE_THROWS_AS(makeStemSet(0.0, a), std::runtime_error);
  a[1] = StemAudio{std::vector<float>(3), std::vector<float>(4)};
  REQUIRE_THROWS_AS(makeStemSet(48000.0, a), std::runtime_error);
  a[1] = StemAudio{std::vector<float>{}, std::vector<float>{}};
  REQUIRE_THROWS_AS(makeStemSet(48000.0, a), std::runtime_error);
  REQUIRE(std::string(stemKindName(StemKind::Guitar)) == "guitar");
}

TEST_CASE("Directory scan warns when two files map to the same named stem (still summed)", "[stems][loader][dir]") {
  StemTempDir t;
  std::vector<std::int16_t> v(2000);
  for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<std::int16_t>((i * 13) % 8000) - 4000;
  const auto w = noise(2000, 61), g1 = noise(2000, 62), g2 = noise(2000, 63);
  writeFlac16(t / "drums.flac", 48000, {v});
  writeWavFloat32(t / "drums.wav", 48000.0, w);
  writeWavFloat32(t / "guitar.wav", 48000.0, g1);
  writeWavFloat32(t / "guitars.wav", 48000.0, g2);
  const StemSet s = loadStemDirectory(t.dir, 48000.0);
  REQUIRE(s.warnings.size() == 2);
  REQUIRE(hasWarning(s, "drums.wav: duplicate 'drums' stem (also drums.flac); summed"));
  REQUIRE(hasWarning(s, "guitars.wav: duplicate 'guitar' stem (also guitar.wav); summed"));
  REQUIRE(s.sources[static_cast<int>(StemKind::Drums)].size() == 2);
  for (std::size_t i = 0; i < 2000; i += 97) {
    REQUIRE(s.audio[static_cast<int>(StemKind::Drums)][0][i] == static_cast<float>(v[i]) / 32768.0f + w[i]);
    REQUIRE(s.audio[static_cast<int>(StemKind::Guitar)][1][i] == g1[i] + g2[i]);
  }
}
