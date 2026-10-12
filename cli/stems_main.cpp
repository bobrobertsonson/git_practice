// sawblade-stems: separate a song into stems with the on-device htdemucs model (phase 5.1b).
//   sawblade-stems <song.mp3|wav|flac> <out-dir> [--model htdemucs_6s|htdemucs] [--threads N]
// Uses the stem cache (a second run on the same file is instant). Exit codes: see separate_cli.h.
#include <iostream>
#include <string>

#include "separate_cli.h"

namespace {
void usage(std::ostream& os) {
  os << "usage: sawblade-stems <song.mp3|wav|flac> <out-dir> [--model htdemucs_6s|htdemucs] [--threads N]\n"
        "exit codes: 0 ok, 2 usage, 3 model missing (the fetch command is printed), 4 error, 130 cancelled\n";
}
}  // namespace

int main(int argc, char** argv) {
  std::string song, out, model = "htdemucs_6s", threads;
  int positional = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(std::cout);
      return 0;
    }
    if (a == "--model" || a == "--threads") {
      if (i + 1 >= argc) {
        std::cerr << "sawblade-stems: missing value for " << a << "\n";
        usage(std::cerr);
        return 2;
      }
      (a == "--model" ? model : threads) = argv[++i];
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "sawblade-stems: unknown option " << a << "\n";
      usage(std::cerr);
      return 2;
    } else if (positional == 0) {
      song = a;
      ++positional;
    } else if (positional == 1) {
      out = a;
      ++positional;
    } else {
      usage(std::cerr);
      return 2;
    }
  }
  if (positional != 2) {
    usage(std::cerr);
    return 2;
  }
  return sawblade_cli::runSeparate("sawblade-stems", song, out, model, threads);
}
