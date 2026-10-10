// Spike driver, candidate (a): demucs.cpp (patched, see patches/) through its library API; writes
// one float32 stereo WAV per stem. See README.md and RESULTS.md. Not product code.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <Eigen/Core>

#include "model.hpp"
#include "sawblade/wav_io.h"
#include "spike_common.hpp"
#include "tensor.hpp"
#ifdef _OPENMP
#include <omp.h>
#endif
#ifdef SAWBLADE_SEPARATOR_BLAS
extern "C" void openblas_set_num_threads(int);
#endif

namespace fs = std::filesystem;
using spike::Clock;
using spike::secondsSince;

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: separator_spike --model <ggml.bin> --in <mix.wav> --out-dir <dir>\n"
               "                       [--threads N] [--cancel-after S] [--quiet]\n"
               "  --threads N       OpenMP / Eigen / OpenBLAS threads for one demucs_inference call\n"
               "  --cancel-after S  set the library's cancel flag S seconds after inference starts and\n"
               "                    report how long the engine took to return (writes no stems)\n"
               "  --quiet           print nothing at all (proves the library itself is silent)\n"
               "  (5.0's --mode split / --ftz are gone: split mode is a different algorithm, FTZ is now always on)\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string model, in, outDir;
  int threads = 1;
  double cancelAfter = -1;
  bool quiet = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { usage(); std::exit(2); }
      return argv[++i];
    };
    if (a == "--model") model = next();
    else if (a == "--in") in = next();
    else if (a == "--out-dir") outDir = next();
    else if (a == "--threads") threads = std::atoi(next().c_str());
    else if (a == "--quiet") quiet = true;
    else if (a == "--cancel-after") cancelAfter = std::atof(next().c_str());
    else { usage(); return 2; }
  }
  if (model.empty() || in.empty() || outDir.empty() || threads < 1) {
    usage();
    return 2;
  }

  try {
    const sawblade::AudioFile wav = sawblade::readWav(in);
    if (wav.sampleRate != 44100.0 || wav.channels != 2) {
      std::fprintf(stderr, "reject: need 44.1 kHz stereo, got %.0f Hz, %d ch\n", wav.sampleRate, wav.channels);
      return 3;
    }
    const int frames = static_cast<int>(wav.interleaved.size() / 2);
    Eigen::MatrixXf audio(2, frames);
    for (int i = 0; i < frames; ++i) {
      audio(0, i) = wav.interleaved[static_cast<std::size_t>(i) * 2];
      audio(1, i) = wav.interleaved[static_cast<std::size_t>(i) * 2 + 1];
    }

    const int nt = threads;
#ifdef _OPENMP
    omp_set_num_threads(nt);
#endif
    Eigen::setNbThreads(nt);
#ifdef SAWBLADE_SEPARATOR_BLAS
    openblas_set_num_threads(nt);
#endif

    // Model: ~80 MB of float weights in a struct that owns all tensors (caller-owned).
    demucscpp::demucs_model dm{};
    auto t0 = Clock::now();
    if (!demucscpp::load_demucs_model(model, &dm)) {
      std::fprintf(stderr, "model load failed: %s\n", model.c_str());
      return 4;
    }
    const double loadS = secondsSince(t0);

    // Progress callback: (fraction 0..1, message), called from the inference thread.
    int cbCalls = 0;
    demucscpp::ProgressCallback cb = [&cbCalls](float, const std::string&) { ++cbCalls; };

    spike::CancelTimer cancel(cancelAfter);
    t0 = Clock::now();
    Eigen::Tensor3dXf out = demucscpp::demucs_inference(dm, audio, cb, cancel.flag());
    const double sepS = secondsSince(t0);
    const bool cancelled = out.size() == 0;
    const double stopS = cancel.fired() ? cancel.secondsSinceSet() : -1;

    static const char* names4[] = {"drums", "bass", "other", "vocals"};
    static const char* names6[] = {"drums", "bass", "other", "vocals", "guitar", "piano"};
    const int n = dm.is_4sources ? 4 : 6;
    if (!cancelled) {
      fs::create_directories(outDir);
      for (int s = 0; s < n; ++s)
        spike::writeStereoFloat32(fs::path(outDir) / (std::string(dm.is_4sources ? names4[s] : names6[s]) + ".wav"),
                                  frames, [&](int c, int i) { return out(s, c, i); });
    }

    const double dur = frames / 44100.0;
    if (quiet) return 0;  // library-silence check: the driver prints nothing either
    std::printf("engine=demucscpp model=%s stems=%d threads=%d\n", model.c_str(), n, threads);
    if (cancelAfter >= 0)
      std::printf("CANCEL cancel_after_s=%.3f cancelled=%d stop_latency_s=%.3f\n", cancelAfter, cancelled ? 1 : 0, stopS);
    std::printf("RESULT load_s=%.3f sep_s=%.3f audio_s=%.3f rtf=%.3f sep_s_per_min=%.2f peak_rss_mb=%.0f progress_cb_calls=%d\n",
                loadS, sepS, dur, sepS / dur, sepS / dur * 60.0, spike::peakRssMb(), cbCalls);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
