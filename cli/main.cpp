// tonerender: render a mono DI WAV through a Sawblade preset, offline.
//
// Exit codes: 0 ok, 2 usage, 3 preset error, 4 I/O / model error.
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifdef SAWBLADE_WITH_SEPARATOR
#include "separate_cli.h"
#endif
#include "sawblade/auto_trim.h"
#include "sawblade/capture_cache.h"
#include "sawblade/render.h"
#include "sawblade/stem_player.h"
#include "sawblade/stem_set.h"

namespace {

constexpr int kExitOk = 0, kExitUsage = 2, kExitPreset = 3, kExitIo = 4;

void usage(std::ostream& os) {
  os << "usage: tonerender --preset P.json --in DI.wav --out OUT.wav [--block N=256]\n"
        "                  [--report R.json] [--normalize-peak dBFS]\n"
        "                  [--render-rate auto|HZ] [--out-rate input|render] [--level-match]\n"
        "                  [--backing STEMDIR [--backing-level dB=0] [--guitar-stem mute|ghost|full]]\n"
        "\n"
        "Renders a mono DI through a Sawblade preset. Output: float32 mono WAV, advanced by the\n"
        "chain's reported latency; at the input rate and length unless --out-rate render.\n"
        "--render-rate auto (default): the NAM models' training rate (they must agree, else exit 3);\n"
        "  the input is resampled to it and, by default, the result back to the input rate.\n"
        "  A model that records no rate counts as 48 kHz. With no NAM blocks, auto renders at the input\n"
        "  rate. A number forces that rate in Hz.\n"
        "--device-dbu X: the interface's level in dBu at 0 dBFS (-60..60) for input calibration. Absent: the assumed +12 dBu,\n"
        "  and the report says so (calibration.deviceAssumed). Calibration follows the preset's calibration.mode (preset v5: legacy =\n"
        "  off, bit-identical to before; calibrated = each NAM block's input gain planned from the device level and the captures'\n"
        "  metadata); a v1-v4 preset is legacy. With --level-match a calibrated preset uses its output.autoTrimCalDb (measured at\n"
        "  +12 dBu), or a trim measured at --device-dbu when that differs.\n"
        "--di-channel L|R|mix: which channel of a stereo (multi-channel) DI file is rendered. Default: the louder of the first two\n"
        "  channels by whole-file RMS (a tie picks L); mix = their mean. Mono files are used as is. The rule used, and the channel\n"
        "  RMS levels, are in the report (diChannel). Never depends on --block.\n"
        "--level-match: apply the preset's output.autoTrim.db (computed first when missing or stale), the trim that brings the\n"
        "  preset to -18 LUFS on the built-in reference DI; off by default (levels as the preset says; NAM export and the matcher\n"
        "  never see the trim).\n"
        "       tonerender --trim-report PRESET.json... [--out REPORT.json]\n"
        "--trim-report: no render to a file. For every preset prints (JSON, to stdout or --out) its loudness on the reference DI\n"
        "  before and after the trim, the trim and the staleness hash; presets whose captures are not on this machine are\n"
        "  reported as skipped. scripts/compute_trims.py writes the result into presets/** and the loudness table.\n"
        "--backing STEMDIR: play-along render. After the normal render, the stems in STEMDIR (drums, bass,\n"
        "  vocals, other, guitar|guitars; .wav or .flac; any other audio file is summed into 'other' with a\n"
        "  warning) are resampled to the output rate, played from 0 through the StemPlayer (free-run, no\n"
        "  count-in, zero rig latency because the render is already latency-compensated) and mixed with the\n"
        "  rendered guitar. Output: float32 STEREO WAV, L = guitar + backing L, R = guitar + backing R, the\n"
        "  length of the guitar render (the backing is truncated / zero-padded). --normalize-peak applies to\n"
        "  the guitar render only, before mixing. --backing-level: master backing level in dB (-60..12).\n"
        "  --guitar-stem: what the song's own guitar stem does: mute (default), ghost (-12 dB) or full.\n"
        "  --other-role: guitar (default) loads a 4-stem 'other' as the guitar stem when the folder has no\n"
        "  guitar/guitars file, so --guitar-stem mute removes it; other keeps it as 'other' (keys, synths).\n"
        "  A real guitar/guitars file always wins. --backing-offset-ms: where the DI starts inside the song,\n"
        "  in ms (-600000..600000), the same sign and meaning as the matcher's --offset-ms: the DI's t = 0\n"
        "  sounds at song time `ms`. Positive: the stems lead (stem audio from `ms` plays at t = 0); negative:\n"
        "  the DI starts before the song, so the backing begins `-ms` into the render. Default 0.\n"
        "  The options above need --backing (else exit 2). The report gains a \"backing\" object.\n"
        "       tonerender --separate SONG --stems-out DIR [--model htdemucs_6s|htdemucs] [--threads N]\n"
        "exit codes: 0 ok, 2 usage, 3 preset error, 4 I/O or model error\n"
        "--separate SONG: stem separation instead of a render. Separates a song (mp3, wav, flac) with the\n"
        "  on-device htdemucs model into DIR (drums, bass, vocals, other[, guitar] .wav; float32, 44.1 kHz),\n"
        "  through the stem cache. Needs the model (printed fetch command if missing: exit 3). Same as the\n"
        "  sawblade-stems tool. Not available when built with SAWBLADE_WITH_SEPARATOR=OFF.\n";
}

struct Args {
  std::string preset, in, out, report, backing;
  double backingLevelDb = 0.0;
  bool backingLevelGiven = false;
  std::string guitarStem = "mute";
  bool guitarStemGiven = false;
  double backingOffsetMs = 0.0;
  bool backingOffsetGiven = false;
  std::string otherRole = "guitar";
  bool otherRoleGiven = false;
  sawblade::RenderOptions opts;
  bool levelMatch = false;
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
    if (k == "--level-match") {
      a.levelMatch = true;
    } else if (k == "--render-rate") {
      if (!value(v)) return "missing value for " + k;
      if (v == "auto") {
        a.opts.renderRate.reset();
      } else {
        double d = 0.0;
        if (!parseNumber(v, d) || d < 1000.0 || d > 768000.0) return "--render-rate must be auto or a rate in 1000..768000 Hz";
        a.opts.renderRate = d;
      }
    } else if (k == "--device-dbu") {
      if (!value(v)) return "missing value for " + k;
      double d = 0.0;
      if (!parseNumber(v, d) || d < sawblade::calibration::kMinPlausibleDbu || d > sawblade::calibration::kMaxPlausibleDbu)
        return "--device-dbu must be a number in -60..60 (dBu at 0 dBFS)";
      a.opts.deviceDbu = d;
    } else if (k == "--di-channel") {
      if (!value(v)) return "missing value for " + k;
      if (v == "L") a.opts.diChannel = sawblade::DiChannel::Left;
      else if (v == "R") a.opts.diChannel = sawblade::DiChannel::Right;
      else if (v == "mix") a.opts.diChannel = sawblade::DiChannel::Mix;
      else return "--di-channel must be L, R or mix";
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
    } else if (k == "--backing-offset-ms") {
      if (!value(v)) return "missing value for " + k;
      if (!parseNumber(v, a.backingOffsetMs) || std::fabs(a.backingOffsetMs) > 600000.0)
        return "--backing-offset-ms must be a number in -600000..600000 (ms)";
      a.backingOffsetGiven = true;
    } else if (k == "--other-role") {
      if (!value(v)) return "missing value for " + k;
      if (v != "guitar" && v != "other") return "--other-role must be guitar or other";
      a.otherRole = v;
      a.otherRoleGiven = true;
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
  if (a.backing.empty() && a.backingOffsetGiven) return "--backing-offset-ms requires --backing";
  if (a.backing.empty() && a.otherRoleGiven) return "--other-role requires --backing";
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
  auto set = std::make_unique<StemSet>(
      loadStemDirectory(a.backing, r.sampleRate, a.otherRole == "other" ? OtherRole::Other : OtherRole::Guitar));
  out.set.present = set->present;
  out.set.otherMappedToGuitar = set->otherMappedToGuitar;  // metadata only: the audio itself moves into the player
  out.set.sources = set->sources;
  out.set.warnings = set->warnings;
  out.set.backingLoudnessLufs = set->backingLoudnessLufs;
  StemPlayer player;
  player.prepare({r.sampleRate, r.blockSize}, 0);  // rig latency 0: the render is already compensated
  player.setMasterLevelDb(a.backingLevelDb);
  // CLI sign (matcher convention, DI start inside the song) is the opposite of the player's (playhead p
  // plays stem sample p - offset): the DI starting `ms` into the song means stem sample p + ms plays at p.
  player.setStartOffsetSamples(-static_cast<std::int64_t>(std::llround(a.backingOffsetMs * 0.001 * r.sampleRate)));
  player.setGuitarMode(a.guitarStem == "full" ? GuitarMode::Full
                       : a.guitarStem == "ghost" ? GuitarMode::Ghost
                                                 : GuitarMode::Muted);
  player.setStemSet(std::move(set));
  {
    // Adopt the set and let the master-level ramp finish while stopped, so the level is exact from
    // the first played sample (a stopped player only runs its mix ramps).
    // n may exceed the prepared maxBlockSize: process() accepts any n.
    const auto n = static_cast<std::size_t>(std::llround(kMixRampMs * 0.001 * r.sampleRate)) + 1;
    std::vector<float> scratchL(n, 0.0f), scratchR(n, 0.0f);
    player.process(scratchL.data(), scratchR.data(), static_cast<int>(n));
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
          {"offsetMs", a.backingOffsetMs},
          {"otherRole", a.otherRole},
          {"otherMappedToGuitar", b.set.otherMappedToGuitar},
          {"stems", stems},
          {"warnings", b.set.warnings},
          {"loudnessLufs", b.set.backingLoudnessLufs ? json(*b.set.backingLoudnessLufs) : json(nullptr)},
          {"outputChannels", 2}};
}

// `tonerender --separate SONG --stems-out DIR [--model M] [--threads N]`.
int separateMode(int argc, char** argv) {
#ifdef SAWBLADE_WITH_SEPARATOR
  std::string song, out, model = "htdemucs_6s", threads;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    if (k == "--help" || k == "-h") {
      usage(std::cout);
      return kExitOk;
    }
    if (k != "--separate" && k != "--stems-out" && k != "--model" && k != "--threads") {
      std::cerr << "tonerender: " << k << " cannot be combined with --separate\n";
      usage(std::cerr);
      return kExitUsage;
    }
    if (i + 1 >= argc) {
      std::cerr << "tonerender: missing value for " << k << "\n";
      return kExitUsage;
    }
    const std::string v = argv[++i];
    if (k == "--separate") song = v;
    else if (k == "--stems-out") out = v;
    else if (k == "--model") model = v;
    else threads = v;
  }
  if (song.empty() || out.empty()) {
    std::cerr << "tonerender: --separate needs a song and --stems-out DIR\n";
    usage(std::cerr);
    return kExitUsage;
  }
  return sawblade_cli::runSeparate("tonerender", song, out, model, threads);
#else
  (void)argc;
  (void)argv;
  std::cerr << "tonerender: error: separation is not available in this build (configure with -DSAWBLADE_WITH_SEPARATOR=ON)\n";
  return kExitIo;
#endif
}

// `tonerender --trim-report PRESET.json... [--out REPORT.json]` (v0.3 Task B): see usage().
int trimReportMode(int argc, char** argv) {
  using nlohmann::json;
  std::vector<std::string> files;
  std::string out;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    if (k == "--trim-report") continue;
    if (k == "--help" || k == "-h") {
      usage(std::cout);
      return kExitOk;
    }
    if (k == "--out") {
      if (i + 1 >= argc) {
        std::cerr << "tonerender: missing value for --out\n";
        return kExitUsage;
      }
      out = argv[++i];
    } else if (k.rfind("--", 0) == 0) {
      std::cerr << "tonerender: " << k << " cannot be combined with --trim-report\n";
      return kExitUsage;
    } else {
      files.push_back(k);
    }
  }
  if (files.empty()) {
    std::cerr << "tonerender: --trim-report needs at least one preset\n";
    return kExitUsage;
  }
  json rep = {{"target", sawblade::kAutoTrimTargetLufs}, {"version", sawblade::kAutoTrimVersion}, {"presets", json::array()}};
  for (const std::string& f : files) {
    json e = {{"file", f}};
    try {
      sawblade::Preset p = sawblade::loadPresetFile(f);
      e["name"] = p.name;
      e["outputGainDb"] = p.outputGainDb;
      const auto missing = sawblade::missingCaptures(p);
      if (!missing.empty()) {
        e["status"] = "skipped";
        e["reason"] = "capture not cached";
        e["missing"] = missing;
      } else {
        sawblade::CaptureCache cache;
        const auto r = sawblade::computeAutoTrim(p, &cache);
        if (!r) {
          e["status"] = "error";
          e["reason"] = "the render of the reference DI is silent";
        } else {
          p.autoTrim.db = r->trimDb;
          p.autoTrim.hash = r->hash;
          const auto after = sawblade::measureReferenceLufs(p, &cache, /*applyTrim=*/true);
          e["status"] = "ok";
          e["lufsBefore"] = r->lufs;
          e["trimDb"] = r->trimDb;
          e["hash"] = r->hash;
          e["lufsAfter"] = after ? json(*after) : json(nullptr);
        }
      }
    } catch (const std::exception& ex) {
      e["status"] = "error";
      e["reason"] = ex.what();
    }
    rep["presets"].push_back(std::move(e));
  }
  if (out.empty()) {
    std::cout << rep.dump(2) << "\n";
  } else {
    std::ofstream f(out);
    f << rep.dump(2) << "\n";
    if (!f) {
      std::cerr << "tonerender: error: cannot write " << out << "\n";
      return kExitIo;
    }
  }
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--separate") return separateMode(argc, argv);
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--trim-report") return trimReportMode(argc, argv);
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
    sawblade::RenderResult r;
    args.opts.calibrationFromPreset = true;  // calibration follows the preset's calibration.mode
    nlohmann::json trimNote;  // report: where the --level-match trim came from
    if (args.levelMatch) {
      sawblade::RenderOptions o = args.opts;
      o.applyAutoTrim = true;
      sawblade::Preset p;
      try {
        p = sawblade::loadPresetFile(args.preset);
      } catch (const sawblade::PresetError& e) {
        throw sawblade::RenderError(sawblade::RenderErrorKind::Preset, args.preset + ": " + e.what());
      } catch (const std::exception& e) {
        throw sawblade::RenderError(sawblade::RenderErrorKind::Io, e.what());
      }
      bool silent = false;
      if (p.calibrationMode == sawblade::CalibrationMode::Calibrated) {
        // Calibrated trim (preset v5). At the assumed device (no --device-dbu, or +12) the stored autoTrimCal is used, measured
        // when missing or stale; at another device it is re-measured there (not an offset: the level is not linear in the device
        // level). Either way nothing is written back to the preset.
        const bool atAssumed = !args.opts.deviceDbu || *args.opts.deviceDbu == sawblade::calibration::kAssumedDeviceDbu;
        if (atAssumed) {
          const bool stored = sawblade::autoTrimCalFresh(p);
          silent = !sawblade::ensureAutoTrimCal(p, o.cache);
          p.autoTrim = p.autoTrimCal;
          trimNote = {{"source", stored ? "stored autoTrimCalDb" : "measured at +12 dBu"}, {"deviceDbu", sawblade::calibration::kAssumedDeviceDbu}};
        } else {
          sawblade::ChainCalibration cc = sawblade::assumedDeviceCalibration();
          cc.device.dbu = args.opts.deviceDbu;
          const auto res = sawblade::computeAutoTrim(p, o.cache, nullptr, cc);
          silent = !res;
          if (res) {
            p.autoTrim.db = res->trimDb;
            p.autoTrim.hash = res->hash;
          }
          trimNote = {{"source", "measured at the given device level"}, {"deviceDbu", *args.opts.deviceDbu}};
        }
      } else {
        silent = !sawblade::autoTrimFresh(p) && !sawblade::stampAutoTrim(p, o.cache);
        trimNote = {{"source", "legacy autoTrimDb"}};
      }
      if (silent) {  // never apply a stale stored trim when the reference cannot be measured
        p.autoTrim.db = 0.0;
        p.autoTrim.hash.clear();
        trimNote = {{"source", "none (reference DI renders silent)"}};
      }
      if (silent)
        std::cerr << "tonerender: warning: --level-match: the reference DI renders silent through this preset; no trim applied\n";
      sawblade::AudioFile in;
      try {
        in = sawblade::readWav(args.in);
      } catch (const std::exception& e) {
        throw sawblade::RenderError(sawblade::RenderErrorKind::Io, e.what());
      }
      r = sawblade::renderPreset(p, in, o);
    } else {
      r = sawblade::renderFile(args.preset, args.in, args.opts);
    }
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
      if (!trimNote.is_null()) rep["levelMatchTrim"] = trimNote;
      if (!args.backing.empty()) rep["backing"] = backingReport(args, backing);
      f << rep.dump(2) << "\n";
      if (!f) throw sawblade::RenderError(sawblade::RenderErrorKind::Io, "cannot write report: " + args.report);
    }
    if (!args.report.empty() && r.info.lufs[0] > sawblade::LevelMatchResult::kNoLufs) {
      char buf[256];
      std::snprintf(buf, sizeof buf,
                    "level match: A %+.1f dB, B %+.1f dB; make-up [%.1f, %.1f, %.1f, %.1f, %.1f] dB at blend 0..1",
                    r.info.trimDb[0], r.info.trimDb[1], r.info.makeupDb[0], r.info.makeupDb[1], r.info.makeupDb[2],
                    r.info.makeupDb[3], r.info.makeupDb[4]);
      std::cerr << "tonerender: " << buf << "\n";
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
