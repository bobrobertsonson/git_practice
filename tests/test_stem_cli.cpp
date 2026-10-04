// Phase 5.1: tonerender --backing (runs the real binary).
#include <sys/wait.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sawblade/wav_io.h"
#include "stem_test_util.h"
#include "test_util.h"

using namespace sawblade;
using namespace sawblade::test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = SAWBLADE_FIXTURES_DIR;
const fs::path kPreset = kFixtures / "presets" / "golden_shared.json";
const fs::path kDi = kFixtures / "di_riff.wav";

std::string q(const fs::path& p) { return "'" + p.string() + "'"; }

int runCli(const std::string& args, const fs::path& errFile) {
  const std::string cmd = q(SAWBLADE_TONERENDER_EXE) + " " + args + " >/dev/null 2>" + q(errFile);
  const int st = std::system(cmd.c_str());
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void splitStereo(const AudioFile& f, std::vector<float>& l, std::vector<float>& r) {
  REQUIRE(f.channels == 2);
  const std::size_t n = f.interleaved.size() / 2;
  l.resize(n);
  r.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    l[i] = f.interleaved[2 * i];
    r[i] = f.interleaved[2 * i + 1];
  }
}

}  // namespace

TEST_CASE("CLI --backing: stereo mix of the guitar render and the backing", "[cli][backing]") {
  StemTempDir t;
  const fs::path stems = t.dir / "stems";
  fs::create_directories(stems);
  const std::size_t drumsLen = 250000, guitarLen = 100000;  // DI is 192000 frames: truncate / pad
  const auto dl = noise(drumsLen, 1, 0.2f), dr = noise(drumsLen, 2, 0.2f);
  const auto gstem = noise(guitarLen, 3, 0.2f);
  writeWavFloat32Stereo(stems / "drums.wav", 48000.0, dl, dr);
  writeWavFloat32(stems / "guitars.wav", 48000.0, gstem);

  const std::string base = "--preset " + q(kPreset) + " --in " + q(kDi);
  // Reference guitar render (mono, no backing).
  REQUIRE(runCli(base + " --out " + q(t / "mono.wav"), t / "err.txt") == 0);
  const AudioFile mono = readWav(t / "mono.wav");
  REQUIRE(mono.channels == 1);
  const std::size_t N = mono.interleaved.size();
  REQUIRE(N == 192000);

  auto render = [&](const std::string& extra, const std::string& name) {
    const int rc = runCli(base + " --out " + q(t / (name + ".wav")) + " --report " + q(t / (name + ".json")) + " --backing " + q(stems) + " " + extra,
                          t / "err.txt");
    INFO(slurp(t / "err.txt"));
    REQUIRE(rc == 0);
  };

  SECTION("default: guitar stem muted, backing level 0 dB") {
    render("", "a");
    std::vector<float> l, r;
    splitStereo(readWav(t / "a.wav"), l, r);
    REQUIRE(l.size() == N);
    // L - R equals the backing L - R (the guitar render is identical in both channels).
    // From the end of the 5 ms play fade-in on, the drums play at unity.
    for (std::size_t i = 300; i < N; ++i) {
      const double expect = i < drumsLen ? static_cast<double>(dl[i]) - dr[i] : 0.0;
      REQUIRE(std::fabs((static_cast<double>(l[i]) - r[i]) - expect) < 1e-5);
      // The song's guitar stem is absent: L = guitar render + drums L.
      REQUIRE(std::fabs((static_cast<double>(l[i]) - mono.interleaved[i]) - dl[i]) < 1e-5);
    }
    // The 5 ms play fade-in at the start (240 samples).
    REQUIRE(std::fabs((static_cast<double>(l[0]) - mono.interleaved[0]) - dl[0] / 240.0) < 1e-5);

    const json rep = json::parse(slurp(t / "a.json"));
    REQUIRE(rep["frames"] == N);
    REQUIRE(rep.contains("backing"));
    const json& b = rep["backing"];
    REQUIRE(b["dir"] == stems.string());
    REQUIRE(b["levelDb"] == 0.0);
    REQUIRE(b["guitarStem"] == "mute");
    REQUIRE(b["outputChannels"] == 2);
    REQUIRE(b["warnings"].is_array());
    REQUIRE(b["stems"].size() == 2);
    REQUIRE(b["stems"][0]["kind"] == "drums");
    REQUIRE(b["stems"][0]["files"][0]["file"] == "drums.wav");
    REQUIRE(b["stems"][0]["files"][0]["sourceChannels"] == 2);
    REQUIRE(b["stems"][0]["files"][0]["sourceFrames"] == drumsLen);
    REQUIRE(b["stems"][0]["files"][0]["sourceRate"] == 48000.0);
    REQUIRE(b["stems"][1]["kind"] == "guitar");
    REQUIRE(b["stems"][1]["files"][0]["sourceChannels"] == 1);
  }

  SECTION("--guitar-stem full adds the song's guitar; ghost is -12 dB") {
    render("--guitar-stem full", "f");
    std::vector<float> l, r;
    splitStereo(readWav(t / "f.wav"), l, r);
    for (std::size_t i = 300; i < guitarLen; ++i)
      REQUIRE(std::fabs((static_cast<double>(l[i]) - mono.interleaved[i]) - (static_cast<double>(dl[i]) + gstem[i])) < 1e-5);
    for (std::size_t i = guitarLen; i < N; ++i)
      REQUIRE(std::fabs((static_cast<double>(l[i]) - mono.interleaved[i]) - (i < drumsLen ? dl[i] : 0.0f)) < 1e-5);
    REQUIRE(json::parse(slurp(t / "f.json"))["backing"]["guitarStem"] == "full");

    render("--guitar-stem ghost", "g");
    splitStereo(readWav(t / "g.wav"), l, r);
    const double ghost = std::pow(10.0, -12.0 / 20.0);
    for (std::size_t i = 300; i < guitarLen; ++i)
      REQUIRE(std::fabs((static_cast<double>(l[i]) - mono.interleaved[i]) - (dl[i] + ghost * gstem[i])) < 1e-5);
  }

  SECTION("--backing-level scales the backing only") {
    render("--backing-level -6", "l");
    std::vector<float> l, r;
    splitStereo(readWav(t / "l.wav"), l, r);
    const double g = std::pow(10.0, -6.0 / 20.0);
    for (std::size_t i = 300; i < 192000; ++i)
      REQUIRE(std::fabs((static_cast<double>(l[i]) - mono.interleaved[i]) - g * dl[i]) < 1e-5);
    REQUIRE(json::parse(slurp(t / "l.json"))["backing"]["levelDb"] == -6.0);
  }

  SECTION("--normalize-peak applies to the guitar render only") {
    REQUIRE(runCli(base + " --normalize-peak -3 --out " + q(t / "nm.wav"), t / "err.txt") == 0);
    const AudioFile nm = readWav(t / "nm.wav");
    render("--normalize-peak -3", "n");
    std::vector<float> l, r;
    splitStereo(readWav(t / "n.wav"), l, r);
    for (std::size_t i = 300; i < 192000; ++i)
      REQUIRE(std::fabs((static_cast<double>(l[i]) - nm.interleaved[i]) - dl[i]) < 1e-5);
  }

  SECTION("block size does not change the result") {
    render("--block 100", "b100");
    render("--block 4096", "b4096");
    REQUIRE(readWav(t / "b100.wav").interleaved == readWav(t / "b4096.wav").interleaved);
  }
}

TEST_CASE("CLI --backing: unrecognised stems warn, errors map to exit codes", "[cli][backing]") {
  StemTempDir t;
  const std::string base = "--preset " + q(kPreset) + " --in " + q(kDi) + " --out " + q(t / "o.wav");
  const fs::path err = t / "err.txt";

  // Backing-only options without --backing: usage error.
  REQUIRE(runCli(base + " --guitar-stem full", err) == 2);
  REQUIRE(runCli(base + " --backing-level -3", err) == 2);
  // Bad values: usage error.
  const fs::path stems = t.dir / "stems";
  fs::create_directories(stems);
  writeWavFloat32(stems / "bass.wav", 48000.0, noise(1000, 4));
  writeWavFloat32(stems / "piano.wav", 48000.0, noise(1000, 5));
  REQUIRE(runCli(base + " --backing " + q(stems) + " --guitar-stem loud", err) == 2);
  REQUIRE(runCli(base + " --backing " + q(stems) + " --backing-level 13", err) == 2);
  REQUIRE(runCli(base + " --backing " + q(stems) + " --backing-level -61", err) == 2);
  REQUIRE(runCli(base + " --backing", err) == 2);

  // Bad directory / no stems / undecodable stem: exit 4.
  REQUIRE(runCli(base + " --backing " + q(t / "missing"), err) == 4);
  REQUIRE(slurp(err).find("missing") != std::string::npos);
  const fs::path empty = t.dir / "empty";
  fs::create_directories(empty);
  REQUIRE(runCli(base + " --backing " + q(empty), err) == 4);
  { std::ofstream(stems / "drums.wav") << "garbage"; }
  REQUIRE(runCli(base + " --backing " + q(stems), err) == 4);
  fs::remove(stems / "drums.wav");

  // An unknown stem name is summed into 'other' with a warning in the report and on stderr.
  REQUIRE(runCli(base + " --backing " + q(stems) + " --report " + q(t / "r.json"), err) == 0);
  const json rep = json::parse(slurp(t / "r.json"));
  REQUIRE(rep["backing"]["warnings"].size() == 1);
  REQUIRE(rep["backing"]["warnings"][0].get<std::string>().find("piano.wav") != std::string::npos);
  REQUIRE(slurp(err).find("piano.wav") != std::string::npos);
  REQUIRE(rep["backing"]["stems"].size() == 2);  // bass, other

  // Without --backing the output stays mono and the report has no backing object.
  REQUIRE(runCli(base + " --report " + q(t / "plain.json"), err) == 0);
  REQUIRE(readWav(t / "o.wav").channels == 1);
  REQUIRE_FALSE(json::parse(slurp(t / "plain.json")).contains("backing"));
}

TEST_CASE("CLI --backing: report carries backing.loudnessLufs", "[cli][backing][loudness]") {
  StemTempDir t;
  const std::string base = "--preset " + q(kPreset) + " --in " + q(kDi) + " --out " + q(t / "o.wav") + " --report " + q(t / "r.json");
  const fs::path err = t / "err.txt";
  const fs::path a = t.dir / "a", g = t.dir / "g";
  fs::create_directories(a);
  fs::create_directories(g);
  writeWavFloat32(a / "drums.wav", 48000.0, sine(1000.0, 48000.0, 96000, 0.07));  // -23 dBFS peak, both channels: about -23 LUFS
  writeWavFloat32(g / "guitar.wav", 48000.0, sine(1000.0, 48000.0, 96000, 0.5));
  REQUIRE(runCli(base + " --backing " + q(a), err) == 0);
  const json r = json::parse(slurp(t / "r.json"));
  REQUIRE(r["backing"]["loudnessLufs"].is_number());
  REQUIRE(std::fabs(r["backing"]["loudnessLufs"].get<double>() - (-23.0)) < 0.5);
  // Guitar-only backing: nothing to measure -> null.
  REQUIRE(runCli(base + " --backing " + q(g), err) == 0);
  REQUIRE(json::parse(slurp(t / "r.json"))["backing"]["loudnessLufs"].is_null());
}
