// tonerender: render a mono DI WAV through a Sawblade preset, offline.
//
// Exit codes: 0 ok, 2 usage, 3 preset error, 4 I/O / model error.
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "sawblade/render.h"

namespace {

constexpr int kExitOk = 0, kExitUsage = 2, kExitPreset = 3, kExitIo = 4;

void usage(std::ostream& os) {
  os << "usage: tonerender --preset P.json --in DI.wav --out OUT.wav [--block N=256]\n"
        "                  [--report R.json] [--normalize-peak dBFS]\n"
        "                  [--render-rate auto|HZ] [--out-rate input|render]\n"
        "\n"
        "Renders a mono DI through a Sawblade preset. Output: float32 mono WAV, advanced by the\n"
        "chain's reported latency; at the input rate and length unless --out-rate render.\n"
        "--render-rate auto (default): the NAM models' training rate (they must agree, else exit 3);\n"
        "  the input is resampled to it and, by default, the result back to the input rate.\n"
        "  With no NAM blocks, auto renders at the input rate. A number forces that rate in Hz.\n"
        "exit codes: 0 ok, 2 usage, 3 preset error, 4 I/O or model error\n";
}

struct Args {
  std::string preset, in, out, report;
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
  return "";
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
    sawblade::writeRenderedWav(args.out, r);
    if (!args.report.empty()) {
      std::ofstream f(args.report);
      if (!f) throw sawblade::RenderError(sawblade::RenderErrorKind::Io, "cannot write report: " + args.report);
      f << sawblade::reportJson(r).dump(2) << "\n";
      if (!f) throw sawblade::RenderError(sawblade::RenderErrorKind::Io, "cannot write report: " + args.report);
    }
    for (const auto& w : r.warnings) std::cerr << "tonerender: warning: " << w << "\n";
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
