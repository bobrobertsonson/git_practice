// Phase 5.0 spike driver: runs demucs.cpp through its library API (not its CLI) and writes one
// float32 stereo WAV per stem. See README.md and RESULTS.md. Not product code.
#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <Eigen/Core>
#include <dr_wav.h>

#include "model.hpp"
#include "sawblade/wav_io.h"
#include "tensor.hpp"
#include "threaded_inference.hpp"  // demucs.cpp cli-apps/: its "demucs_mt" song-splitting path
#ifdef _OPENMP
#include <omp.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double secondsSince(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

void writeStereoFloat32(const fs::path& path, const Eigen::Tensor3dXf& t, int source, int frames) {
  std::vector<float> inter(static_cast<std::size_t>(frames) * 2);
  for (int i = 0; i < frames; ++i) {
    inter[static_cast<std::size_t>(i) * 2] = t(source, 0, i);
    inter[static_cast<std::size_t>(i) * 2 + 1] = t(source, 1, i);
  }
  drwav_data_format fmt{};
  fmt.container = drwav_container_riff;
  fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
  fmt.channels = 2;
  fmt.sampleRate = 44100;
  fmt.bitsPerSample = 32;
  drwav wav;
  if (!drwav_init_file_write(&wav, path.string().c_str(), &fmt, nullptr))
    throw std::runtime_error("cannot write " + path.string());
  drwav_write_pcm_frames(&wav, static_cast<drwav_uint64>(frames), inter.data());
  drwav_uninit(&wav);
}

void usage() {
  std::fprintf(stderr,
               "usage: separator_spike --model <ggml.bin> --in <mix.wav> --out-dir <dir>\n"
               "                       [--threads N] [--mode single|split]\n"
               "  --threads N   OpenMP/Eigen GEMM threads (single) or song-split workers (split)\n"
               "  --mode single (default) library demucs_inference(): the real 7.8 s/25%% overlap\n"
               "                pipeline, one segment at a time\n"
               "  --mode split  demucs.cpp's demucs_mt path: song cut into N parts, 0.75 s overlap,\n"
               "                each part run on its own std::thread (different algorithm)\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string model, in, outDir, mode = "single";
  int threads = 1;
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
    else if (a == "--mode") mode = next();
    else { usage(); return 2; }
  }
  if (model.empty() || in.empty() || outDir.empty() || threads < 1 || (mode != "single" && mode != "split")) {
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

#ifdef _OPENMP
    omp_set_num_threads(mode == "single" ? threads : 1);
#endif
    Eigen::setNbThreads(mode == "single" ? threads : 1);

    // Model: ~80 MB of float weights in a struct that owns all tensors (caller-owned).
    demucscpp::demucs_model dm{};
    auto t0 = Clock::now();
    if (!demucscpp::load_demucs_model(model, &dm)) {
      std::fprintf(stderr, "model load failed: %s\n", model.c_str());
      return 4;
    }
    const double loadS = secondsSince(t0);

    // Progress callback: (fraction 0..1, message). Called from the inference thread. There is no
    // cancel token; the only way to stop is to not call (or to kill the thread).
    int cbCalls = 0;
    demucscpp::ProgressCallback cb = [&cbCalls](float, const std::string&) { ++cbCalls; };

    t0 = Clock::now();
    Eigen::Tensor3dXf out = mode == "single" ? demucscpp::demucs_inference(dm, audio, cb)
                                             : demucscppthreaded::threaded_inference(dm, audio, threads);
    const double sepS = secondsSince(t0);

    static const char* names4[] = {"drums", "bass", "other", "vocals"};
    static const char* names6[] = {"drums", "bass", "other", "vocals", "guitar", "piano"};
    const int n = dm.is_4sources ? 4 : 6;
    fs::create_directories(outDir);
    for (int s = 0; s < n; ++s)
      writeStereoFloat32(fs::path(outDir) / (std::string(dm.is_4sources ? names4[s] : names6[s]) + ".wav"), out, s, frames);

    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const double dur = frames / 44100.0;
    std::printf("model=%s stems=%d mode=%s threads=%d\n", model.c_str(), n, mode.c_str(), threads);
    std::printf("RESULT load_s=%.3f sep_s=%.3f audio_s=%.3f rtf=%.3f sep_s_per_min=%.2f peak_rss_mb=%.0f progress_cb_calls=%d\n",
                loadS, sepS, dur, sepS / dur, sepS / dur * 60.0, ru.ru_maxrss / 1024.0, cbCalls);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
