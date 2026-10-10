#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "dr_wav.h"
#include "sawblade/wav_io.h"
#include "test_util.h"

using namespace sawblade;

namespace {

struct TempDir {
  std::filesystem::path dir;
  TempDir() {
    dir = std::filesystem::temp_directory_path() / ("sawblade_wav_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
  std::filesystem::path operator/(const std::string& f) const { return dir / f; }
};

}  // namespace

TEST_CASE("WAV float32 round trip is exact", "[wav]") {
  TempDir t;
  std::vector<float> x = sawblade::test::noise(5000, 11, 0.9f);
  x[0] = 1.0f;
  x[1] = -1.0f;
  x[2] = 1e-7f;
  writeWavFloat32(t / "rt.wav", 44100.0, x);
  const AudioFile f = readWav(t / "rt.wav");
  REQUIRE(f.sampleRate == 44100.0);
  REQUIRE(f.channels == 1);
  REQUIRE(f.interleaved == x);
}

TEST_CASE("WAV reads 16-bit and 24-bit PCM", "[wav]") {
  TempDir t;
  // Values exactly representable at 16 bits so the expected result is exact within 1 LSB.
  std::vector<float> src;
  for (int i = 0; i < 2000; ++i) src.push_back(static_cast<float>(std::sin(i * 0.05) * 0.8));

  SECTION("16-bit") {
    // Write true 16-bit PCM by hand-quantizing and using dr_wav's native 16-bit writer.
    drwav_data_format fmt{drwav_container_riff, DR_WAVE_FORMAT_PCM, 2, 48000, 16};
    std::vector<std::int16_t> pcm;
    for (float v : src) { pcm.push_back(static_cast<std::int16_t>(std::lround(v * 32767.0))); pcm.push_back(static_cast<std::int16_t>(-pcm.back())); }
    drwav w;
    REQUIRE(drwav_init_file_write(&w, (t / "a16.wav").string().c_str(), &fmt, nullptr));
    REQUIRE(drwav_write_pcm_frames(&w, src.size(), pcm.data()) == src.size());
    drwav_uninit(&w);

    const AudioFile f = readWav(t / "a16.wav");
    REQUIRE(f.sampleRate == 48000.0);
    REQUIRE(f.channels == 2);
    REQUIRE(f.interleaved.size() == src.size() * 2);
    for (std::size_t i = 0; i < src.size(); ++i) {
      REQUIRE(f.interleaved[2 * i] == Catch::Approx(src[i]).margin(2.0 / 32768.0));
      REQUIRE(f.interleaved[2 * i + 1] == Catch::Approx(-src[i]).margin(2.0 / 32768.0));
    }
  }

  SECTION("24-bit") {
    drwav_data_format fmt{drwav_container_riff, DR_WAVE_FORMAT_PCM, 1, 96000, 24};
    std::vector<std::uint8_t> bytes;
    std::vector<std::int32_t> q;
    for (float v : src) {
      const auto s = static_cast<std::int32_t>(std::lround(v * 8388607.0));
      q.push_back(s);
      bytes.push_back(static_cast<std::uint8_t>(s & 0xff));
      bytes.push_back(static_cast<std::uint8_t>((s >> 8) & 0xff));
      bytes.push_back(static_cast<std::uint8_t>((s >> 16) & 0xff));
    }
    drwav w;
    REQUIRE(drwav_init_file_write(&w, (t / "a24.wav").string().c_str(), &fmt, nullptr));
    REQUIRE(drwav_write_pcm_frames(&w, src.size(), bytes.data()) == src.size());
    drwav_uninit(&w);

    const AudioFile f = readWav(t / "a24.wav");
    REQUIRE(f.sampleRate == 96000.0);
    REQUIRE(f.channels == 1);
    REQUIRE(f.interleaved.size() == src.size());
    for (std::size_t i = 0; i < src.size(); ++i)
      REQUIRE(f.interleaved[i] == Catch::Approx(src[i]).margin(1.5 / 8388608.0));
  }
}

TEST_CASE("WAV errors carry the path", "[wav]") {
  TempDir t;
  const auto missing = t / "does_not_exist.wav";
  try {
    readWav(missing);
    FAIL("expected throw");
  } catch (const std::runtime_error& e) {
    REQUIRE(std::string(e.what()).find("does_not_exist.wav") != std::string::npos);
  }

  { std::ofstream(t / "garbage.wav") << "this is not a wav file at all"; }
  REQUIRE_THROWS_AS(readWav(t / "garbage.wav"), std::runtime_error);

  try {
    writeWavFloat32(t / "no_such_dir" / "x.wav", 48000.0, {0.0f});
    FAIL("expected throw");
  } catch (const std::runtime_error& e) {
    REQUIRE(std::string(e.what()).find("no_such_dir") != std::string::npos);
  }
  REQUIRE_THROWS_AS(writeWavFloat32(t / "bad_rate.wav", 0.0, {0.0f}), std::runtime_error);
}
