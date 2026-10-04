// tonerender: render a mono DI WAV through a Sawblade preset, offline.
//
// Exit codes: 0 ok, 2 usage, 3 preset error, 4 I/O / model error.
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "sawblade/render.h"
#include "sawblade/stem_player.h"
#include "sawblade/stem_set.h"

namespace {

constexpr int kExitOk = 0, kExitUsage = 2, kExitPreset = 3, kExitIo = 4;

void usage(std::ostream& os) {
  os << "usage: tonerender --preset P.json --in DI.wav --out OUT.wav [--block N=256]\n"
        "                  [--report R.json] [--normalize-peak dBFS]\n"
        "                  [--render-rate auto|HZ] [--out-rate input|render]\n"
        "                  [--backing STEMDIR [--backing-level dB=0] [--guitar-stem mute|ghost|full]]\n"
        "\n"
        "Renders a mono DI through a Sawblade preset. Output: float32 mono WAV, advanced by the\n"
        "chain's reported latency; at the input rate and length unless --out-rate render.\n"
        "--render-rate auto (default): the NAM models' training rate (they must agree, else exit 3);\n"
        "  the input is resampled to it and, by default, the result back to the input rate.\n"
        "  A model that records no rate counts as 48 kHz. With no NAM blocks, auto renders at the input\n"
        "  rate. A number forces that rate in Hz.\n"
        "--backing STEMDIR: play-along render. After the normal render, the stems in STEMDIR (drums, bass,\n"
        "  vocals, other, guitar|guitars; .wav or .flac; any other audio file is summed into 'other' with a\n"
        "  warning) are resampled to the output rate, played from 0 through the StemPlayer (free-run, no\n"
        "  count-in, zero rig latency because the render is already latency-compensated) and mixed with the\n"
        "  rendered guitar. Output: float32 STEREO WAV, L = guitar + backing L, R = guitar + backing R, the\n"
        "  length of the guitar render (the backing is truncated / zero-padded). --normalize-peak applies to\n"
        "  the guitar render only, before mixing. --backing-level: master backing level in dB (-60..12).\n"
        "  --guitar-stem: what the song's own guitar stem does: mute (default), ghost (-12 dB) or full.\n"
        "  Both options need --backing (else exit 2). The report gains a \"backing\" object.\n"
        "exit codes: 0 ok, 2 usage, 3 preset error, 4 I/O or model error\n";
}

struct Args {
  std::string preset, in, out, report, backing;
  double backingLevelDb = 0.0;
  bool backingLevelGiven = false;
  std::string guitarStem = "mute";
  bool guitarStemGiven = false;
  sawblade::RenderOptions opts;
  bool help = false;
};

bool parseNumber(const std::string& s, double& v) {
  char* end = nullptr;
  errno = 0;
  v = std::strtod(s.c_str(), &end);
  return !s.empty() && end == s.c_str() + s.size() && errno == 0 && std::isfinite(v);
}

// Returns an error message, or "" on success.
std::string parseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    if (k == "--help" || k == "-h") {
      a.help = true;
      return "";
    }
    auto value = [&](std::string& dst) {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };
    std::string v;
    if (k == "--render-rate") {
      if (!value(v)) return "missing value for " + k;
      if (v == "auto") {
        a.opts.renderRate.reset();
      } else {
        double d = 0.0;
        if (!parseNumber(v, d) || d < 1000.0 || d > 768000.0) return "--render-rate must be auto or a rate in 1000..768000 Hz";
        a.opts.renderRate = d;
      }
    } else if (k == "--out-rate") {
      if (!value(v)) return "missing value for " + k;
      if (v == "input") a.opts.outRate = sawblade::OutRate::Input;
      else if (v == "render") a.opts.outRate = sawblade::OutRate::Render;
      else return "--out-rate must be input or render";
    } else if (k == "--backing") {
      if (!value(a.backing)) return "missing value for " + k;
      if (a.backing.empty()) return "--backing must not be empty";
    } else if (k == "--backing-level") {
      if (!value(v)) return "missing value for " + k;
      if (!parseNumber(v, a.backingLevelDb) || a.backingLevelDb < -60.0 || a.backingLevelDb > 12.0)
        return "--backing-level must be a number in -60..12 (dB)";
      a.backingLevelGiven = true;
    } else if (k == "--guitar-stem") {
      if (!value(v)) return "missing value for " + k;
      if (v != "mute" && v != "ghost" && v != "full") return "--guitar-stem must be mute, ghost or full";
      a.guitarStem = v;
      a.guitarStemGiven = true;
    } else if (k == "--preset" || k == "--in" || k == "--out" || k == "--report" || k == "--block" || k == "--normalize-peak") {
      if (!value(v)) return "missing value for " + k;
      if (k == "--preset") a.preset = v;
      else if (k == "--in") a.in = v;
      else if (k == "--out") a.out = v;
      else if (k == "--report") a.report = v;
      else {
        double d = 0.0;
        if (!parseNumber(v, d)) return "invalid number for " + k + ": " + v;
        if (k == "--block") {
          if (d < 1 || d > 65536 || d != static_cast<int>(d)) return "--block must be an integer in 1..65536";
          a.opts.blockSize = static_cast<int>(d);
        } else {
          a.opts.normalizePeakDbfs = d;
        }
      }
    } else {
      return "unknown argument: " + k;
    }
  }
  if (a.preset.empty()) return "--preset is required";
  if (a.in.empty()) return "--in is required";
  if (a.out.empty()) return "--out is required";
  if (a.backing.empty() && a.backingLevelGiven) return "--backing-level requires --backing";
  if (a.backing.empty() && a.guitarStemGiven) return "--guitar-stem requires --backing";
  return "";
}

// Plays the stems in `dir` (from sample 0, free-run) through a StemPlayer and mixes them with the
// mono guitar render into a stereo pair. Throws std::exception on stem loading errors.
struct BackingResult {
  std::vector<float> left, right;
  sawblade::StemSet set;
};

BackingResult mixBacking(const Args& a, const sawblade::RenderResult& r) {
  using namespace sawblade;
  BackingResult out;
  auto set = std::make_unique<StemSet>(loadStemDirectory(a.backing, r.sampleRate));
  out.set.present = set->present;  // metadata only: the audio itself moves into the player
  out.set.sources = set->sources;
  out.set.warnings = set->warnings;
  StemPlayer player;
  player.prepare({r.sampleRate, r.blockSize}, 0);  // rig latency 0: the render is already compensated
  player.setMasterLevelDb(a.backingLevelDb);
  player.setGuitarMode(a.guitarStem == "full" ? GuitarMode::Full
                       : a.guitarStem == "ghost" ? GuitarMode::Ghost
                                                 : GuitarMode::Muted);
  player.setStemSet(std::move(set));
  {
    // Adopt the set and let the master-level ramp finish while stopped, so the level is exact from
    // the first played sample (a stopped player only runs its mix ramps).
    std::vector<float> scratch(static_cast<std::size_t>(std::llround(kMixRampMs * 0.001 * r.sampleRate)) + 1, 0.0f);
    player.process(scratch.data(), scratch.data(), static_cast<int>(scratch.size()));
  }
  player.play();

  const std::size_t n = r.samples.size();
  out.left.assign(n, 0.0f);
  out.right.assign(n, 0.0f);
  for (std::size_t pos = 0; pos < n; pos += static_cast<std::size_t>(r.blockSize)) {
    const auto m = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(r.blockSize), n - pos));
    player.process(out.left.data() + pos, out.right.data() + pos, m);
  }
  for (std::size_t i = 0; i < n; ++i) {
    out.left[i] += r.samples[i];
    out.right[i] += r.samples[i];
  }
  return out;
}

nlohmann::json backingReport(const Args& a, const BackingResult& b) {
  using nlohmann::json;
  json stems = json::array();
  for (int k = 0; k < sawblade::kStemKindCount; ++k) {
    const auto ks = static_cast<std::size_t>(k);
    if (!b.set.present[ks]) continue;
    json files = json::array();
    for (const auto& s : b.set.sources[ks])
      files.push_back({{"file", std::filesystem::path(s.file).filename().string()},
                       {"sourceRate", s.sourceRate},
                       {"sourceChannels", s.sourceChannels},
                       {"sourceFrames", s.sourceFrames}});
    stems.push_back({{"kind", sawblade::stemKindName(static_cast<sawblade::StemKind>(k))}, {"files", files}});
  }
  return {{"dir", a.backing},
          {"levelDb", a.backingLevelDb},
          {"guitarStem", a.guitarStem},
          {"stems", stems},
          {"warnings", b.set.warnings},
          {"outputChannels", 2}};
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (const std::string err = parseArgs(argc, argv, args); !err.empty()) {
    std::cerr << "tonerender: " << err << "\n";
    usage(std::cerr);
    return kExitUsage;
  }
  if (args.help) {
    usage(std::cout);
    return kExitOk;
  }
  try {
    const sawblade::RenderResult r = sawblade::renderFile(args.preset, args.in, args.opts);
    BackingResult backing;
    if (!args.backing.empty()) {
      backing = mixBacking(args, r);
      try {
        sawblade::writeWavFloat32Stereo(args.out, r.sampleRate, backing.left, backing.right);
      } catch (const std::exception& e) {
        throw sawblade::RenderError(sawblade::RenderErrorKind::Io, e.what());
      }
    } else {
      sawblade::writeRenderedWav(args.out, r);
    }
    if (!args.report.empty()) {
      std::ofstream f(args.report);
      if (!f) throw sawblade::RenderError(sawblade::RenderErrorKind::Io, "cannot write report: " + args.report);
      nlohmann::json rep = sawblade::reportJson(r);
      if (!args.backing.empty()) rep["backing"] = backingReport(args, backing);
      f << rep.dump(2) << "\n";
      if (!f) throw sawblade::RenderError(sawblade::RenderErrorKind::Io, "cannot write report: " + args.report);
    }
    for (const auto& w : r.warnings) std::cerr << "tonerender: warning: " << w << "\n";
    for (const auto& w : backing.set.warnings) std::cerr << "tonerender: warning: " << w << "\n";
    return kExitOk;
  } catch (const sawblade::RenderError& e) {
    std::cerr << "tonerender: " << (e.kind() == sawblade::RenderErrorKind::Preset ? "preset error: " : "error: ")
              << e.what() << "\n";
    return e.kind() == sawblade::RenderErrorKind::Preset ? kExitPreset : kExitIo;
  } catch (const std::exception& e) {
    std::cerr << "tonerender: error: " << e.what() << "\n";
    return kExitIo;
  }
}
