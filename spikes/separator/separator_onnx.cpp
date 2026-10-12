// Spike driver, candidate (b): ONNX Runtime CPU EP running the exported htdemucs core network
// (scripts/export_onnx.py). Everything around the network is our own code and mirrors demucs 4.0.1
// apply_model(shifts=0, split=True, overlap=0.25): per-song normalisation, segmentation with Python's
// TensorChunk.padded context, STFT / magnitude (HTDemucs._spec + _magnitude), mask + iSTFT
// (HTDemucs._mask + _ispec), time-branch add, linear-ramp overlap-add. Not product code.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>
#include <pffft.h>

#include "sawblade/wav_io.h"
#include "spike_common.hpp"

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace fs = std::filesystem;
using spike::Clock;
using spike::secondsSince;

namespace {

constexpr int kSeg = 343980;      // int(7.8 s * 44100), htdemucs `segment`
constexpr int kNfft = 4096;
constexpr int kHop = 1024;
constexpr int kBins = kNfft / 2;  // 2048: Nyquist bin dropped by HTDemucs._spec
constexpr int kFrames = 336;      // ceil(kSeg / kHop)
constexpr int kPad = kHop / 2 * 3;                         // 1536
constexpr int kPadRight = kPad + kFrames * kHop - kSeg;    // 1620
constexpr int kPadded = kSeg + kPad + kPadRight;           // 347136 = 339 * 1024
constexpr int kIstftFrames = kFrames + 4;                  // _ispec pads 2 zero frames each side
constexpr int kIstftLen = kHop * (kIstftFrames - 1) + kNfft;  // 351232

// ---- FTZ/DAZ on the calling thread (ORT's pool threads get it from the session option) -------
struct FtzCaller {
#if defined(__SSE__) || defined(__x86_64__)
  unsigned saved = _mm_getcsr();
  FtzCaller() { _mm_setcsr(saved | 0x8040u); }
  ~FtzCaller() { _mm_setcsr(saved); }
#elif defined(__aarch64__)
  unsigned long saved = 0;
  FtzCaller() {
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved));
    const unsigned long v = saved | (1ul << 24);
    __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
  }
  ~FtzCaller() { __asm__ __volatile__("msr fpcr, %0" : : "r"(saved)); }
#endif
};

// ---- STFT / iSTFT matching demucs.spec.spectro / ispectro with the HTDemucs crop/pad logic -------
class Stft {
 public:
  Stft() {
    setup_ = pffft_new_setup(kNfft, PFFFT_REAL);
    if (!setup_) throw std::runtime_error("pffft setup failed");
    buf_ = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * kNfft));
    out_ = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * kNfft));
    work_ = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * kNfft));
    window_.resize(kNfft);
    for (int n = 0; n < kNfft; ++n)  // torch.hann_window(4096), periodic
      window_[static_cast<std::size_t>(n)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * n / kNfft));
    xp_.resize(kPadded);
    y_.resize(kIstftLen);
    env_.assign(kIstftLen, 0.0f);
    for (int g = 0; g < kIstftFrames; ++g)
      for (int n = 0; n < kNfft; ++n)
        env_[static_cast<std::size_t>(g * kHop + n)] += window_[static_cast<std::size_t>(n)] * window_[static_cast<std::size_t>(n)];
  }
  ~Stft() {
    pffft_aligned_free(buf_);
    pffft_aligned_free(out_);
    pffft_aligned_free(work_);
    pffft_destroy_setup(setup_);
  }
  Stft(const Stft&) = delete;
  Stft& operator=(const Stft&) = delete;

  // mix: 2 x kSeg (planar). mag: (4, kBins, kFrames), channel = c*2 + {re, im}.
  void magnitude(const float* const mix[2], float* mag) {
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < kPadded; ++i) {  // pad1d(x, (pad, pad + le*hl - T), "reflect")
        int p = i - kPad;
        if (p < 0) p = -p;
        if (p >= kSeg) p = 2 * (kSeg - 1) - p;
        xp_[static_cast<std::size_t>(i)] = mix[c][p];
      }
      for (int j = 0; j < kFrames; ++j) {  // frames 2..2+le of the centred STFT == xp[j*hop .. +nfft]
        for (int n = 0; n < kNfft; ++n)
          buf_[n] = xp_[static_cast<std::size_t>(j * kHop + n)] * window_[static_cast<std::size_t>(n)];
        pffft_transform_ordered(setup_, buf_, out_, work_, PFFFT_FORWARD);
        constexpr float kScale = 1.0f / 64.0f;  // normalized=True: n_fft^-0.5
        float* re = mag + static_cast<std::size_t>(c * 2) * kBins * kFrames;
        float* im = re + static_cast<std::size_t>(kBins) * kFrames;
        re[j] = out_[0] * kScale;  // bin 0: imaginary part is exactly 0
        im[j] = 0.0f;
        for (int k = 1; k < kBins; ++k) {
          re[static_cast<std::size_t>(k) * kFrames + j] = out_[2 * k] * kScale;
          im[static_cast<std::size_t>(k) * kFrames + j] = out_[2 * k + 1] * kScale;
        }
      }
    }
  }

  // xf: one source, (4, kBins, kFrames) cac spectrogram (channel = c*2 + {re, im}); xt: (2, kSeg)
  // time-branch output of the same source. Writes xt + istft(mask) to out[c][0..kSeg).
  void synth(const float* xf, const float* xt, float* out0, float* out1) {
    float* outs[2] = {out0, out1};
    for (int c = 0; c < 2; ++c) {
      std::fill(y_.begin(), y_.end(), 0.0f);
      const float* re = xf + static_cast<std::size_t>(c * 2) * kBins * kFrames;
      const float* im = re + static_cast<std::size_t>(kBins) * kFrames;
      for (int j = 0; j < kFrames; ++j) {
        buf_[0] = re[j];
        buf_[1] = 0.0f;  // Nyquist bin added by _ispec's F.pad is zero
        for (int k = 1; k < kBins; ++k) {
          buf_[2 * k] = re[static_cast<std::size_t>(k) * kFrames + j];
          buf_[2 * k + 1] = im[static_cast<std::size_t>(k) * kFrames + j];
        }
        pffft_transform_ordered(setup_, buf_, out_, work_, PFFFT_BACKWARD);
        constexpr float kScale = 1.0f / 64.0f;  // normalized=True: *sqrt(n_fft) then irfft's 1/n_fft
        float* y = y_.data() + static_cast<std::size_t>(j + 2) * kHop;
        for (int n = 0; n < kNfft; ++n) y[n] += out_[n] * kScale * window_[static_cast<std::size_t>(n)];
      }
      const float* xtc = xt + static_cast<std::size_t>(c) * kSeg;
      for (int n = 0; n < kSeg; ++n) {
        const std::size_t i = static_cast<std::size_t>(kNfft / 2 + kPad + n);  // center trim, then x[pad:pad+len]
        outs[c][n] = xtc[n] + y_[i] / env_[i];
      }
    }
  }

 private:
  PFFFT_Setup* setup_ = nullptr;
  float *buf_ = nullptr, *out_ = nullptr, *work_ = nullptr;
  std::vector<float> window_, xp_, y_, env_;
};

void writeFloats(const fs::path& p, const float* d, std::size_t n) {
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(d), static_cast<std::streamsize>(n * sizeof(float)));
}

void usage() {
  std::fprintf(stderr,
               "usage: separator_onnx --model <core.onnx> --in <mix.wav> --out-dir <dir>\n"
               "                      [--threads N] [--cancel-after S] [--profile PREFIX] [--dump-seg DIR]\n"
               "  --threads N       ORT intra_op_num_threads (inter_op = 1)\n"
               "  --cancel-after S  set the cancel flag S seconds after inference starts (checked between\n"
               "                    segments) and report how long the engine took to return\n"
               "  --profile PREFIX  enable ORT's profiler (JSON file PREFIX_<time>.json)\n"
               "  --arena           enable ORT's CPU memory arena (default OFF: much lower peak RSS, see RESULTS.md)\n"
               "  --mem-pattern     enable ORT's memory-pattern planner (default OFF)\n"
               "  --no-spin         session.intra_op.allow_spinning=0\n"
               "  --dump-seg DIR    write the first segment's mix/mag/x_freq/x_time as raw float32 (debug)\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string model, in, outDir, profile, dumpDir;
  bool arena = false, memPattern = false, noSpin = false;
  int threads = 1;
  double cancelAfter = -1;
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
    else if (a == "--cancel-after") cancelAfter = std::atof(next().c_str());
    else if (a == "--profile") profile = next();
    else if (a == "--dump-seg") dumpDir = next();
    else if (a == "--arena") arena = true;
    else if (a == "--mem-pattern") memPattern = true;
    else if (a == "--no-spin") noSpin = true;
    else { usage(); return 2; }
  }
  if (model.empty() || in.empty() || outDir.empty() || threads < 1) { usage(); return 2; }

  try {
    const sawblade::AudioFile wav = sawblade::readWav(in);
    if (wav.sampleRate != 44100.0 || wav.channels != 2) {
      std::fprintf(stderr, "reject: need 44.1 kHz stereo, got %.0f Hz, %d ch\n", wav.sampleRate, wav.channels);
      return 3;
    }
    const int length = static_cast<int>(wav.interleaved.size() / 2);
    std::vector<float> audio[2];
    for (int c = 0; c < 2; ++c) audio[c].resize(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i)
      for (int c = 0; c < 2; ++c) audio[c][static_cast<std::size_t>(i)] = wav.interleaved[static_cast<std::size_t>(i) * 2 + static_cast<std::size_t>(c)];

    // ---- ORT session --------------------------------------------------------------------------
    Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "separator_onnx");
    Ort::SessionOptions so;
    so.SetIntraOpNumThreads(threads);
    so.SetInterOpNumThreads(1);
    so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    so.AddConfigEntry("session.set_denormal_as_zero", "1");
    if (!arena) so.DisableCpuMemArena();        // default OFF: arena = 5.0 GB peak RSS here, 2.5 GB without
    if (!memPattern) so.DisableMemPattern();    // default OFF: measured faster and smaller (RESULTS.md)
    if (noSpin) so.AddConfigEntry("session.intra_op.allow_spinning", "0");
    if (!profile.empty()) so.EnableProfiling(profile.c_str());
    auto t0 = Clock::now();
    Ort::Session session(env, model.c_str(), so);
    const double loadS = secondsSince(t0);
    const auto outShape = session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();  // (1,S,4,F,T)
    const int S = static_cast<int>(outShape[1]);
    if (S != 4 && S != 6) throw std::runtime_error("unexpected source count");

    // ---- per-song normalisation: ref = mix.mean(0); (mix - ref.mean()) / ref.std() ----------------
    double sum = 0;
    for (int i = 0; i < length; ++i) sum += 0.5 * (static_cast<double>(audio[0][static_cast<std::size_t>(i)]) + audio[1][static_cast<std::size_t>(i)]);
    const double refMean = sum / length;
    double ss = 0;
    for (int i = 0; i < length; ++i) {
      const double d = 0.5 * (static_cast<double>(audio[0][static_cast<std::size_t>(i)]) + audio[1][static_cast<std::size_t>(i)]) - refMean;
      ss += d * d;
    }
    const double refStd = std::sqrt(ss / (length - 1));  // unbiased, like torch.std
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < length; ++i)
        audio[c][static_cast<std::size_t>(i)] = static_cast<float>((audio[c][static_cast<std::size_t>(i)] - refMean) / refStd);

    // ---- buffers ----------------------------------------------------------------------------------
    const std::size_t magN = static_cast<std::size_t>(4) * kBins * kFrames;
    const std::size_t xfN = static_cast<std::size_t>(S) * magN;
    const std::size_t xtN = static_cast<std::size_t>(S) * 2 * kSeg;
    std::vector<float> seg[2] = {std::vector<float>(kSeg), std::vector<float>(kSeg)};
    std::vector<float> mag(magN), xf(xfN), xt(xtN);
    std::vector<float> segOut(static_cast<std::size_t>(S) * 2 * kSeg);  // [s][c][n]
    std::vector<float> out(static_cast<std::size_t>(S) * 2 * static_cast<std::size_t>(length), 0.0f);  // [s][c][i]
    std::vector<float> sumWeight(static_cast<std::size_t>(length), 0.0f);
    std::vector<float> weight(kSeg);
    for (int k = 0; k < kSeg / 2; ++k) {
      weight[static_cast<std::size_t>(k)] = static_cast<float>(k + 1) / static_cast<float>(kSeg / 2);
      weight[static_cast<std::size_t>(kSeg - 1 - k)] = weight[static_cast<std::size_t>(k)];
    }
    Stft stft;

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::array<int64_t, 3> mixShape{1, 2, kSeg};
    const std::array<int64_t, 4> magShape{1, 4, kBins, kFrames};
    std::vector<float> mixIn(static_cast<std::size_t>(2) * kSeg);
    const char* inNames[] = {"mix", "mag"};
    const char* outNames[] = {"x_freq", "x_time"};
    const std::array<int64_t, 5> xfShape{1, S, 4, kBins, kFrames};
    const std::array<int64_t, 4> xtShape{1, S, 2, kSeg};

    const int stride = static_cast<int>((1 - 0.25f) * static_cast<float>(kSeg));  // 257985, as demucs.apply
    int cbCalls = 0;
    const int totalChunks = (length + stride - 1) / stride;
    spike::CancelTimer cancel(cancelAfter);
    bool cancelled = false;
    bool dumped = false;

    t0 = Clock::now();
    {
      FtzCaller ftz;
      for (int offset = 0; offset < length; offset += stride) {
        if (cancel.flag() && cancel.flag()->load(std::memory_order_relaxed)) { cancelled = true; break; }
        const int chunkLen = std::min(kSeg, length - offset);
        const int delta = kSeg - chunkLen, left = delta / 2;  // TensorChunk.padded(valid_length)
        const int start = offset - left;
        const int cs = std::max(0, start), ce = std::min(length, start + kSeg);
        for (int c = 0; c < 2; ++c) {
          std::fill(seg[c].begin(), seg[c].end(), 0.0f);
          if (ce > cs) std::memcpy(seg[c].data() + (cs - start), audio[c].data() + cs, sizeof(float) * static_cast<std::size_t>(ce - cs));
          std::memcpy(mixIn.data() + static_cast<std::size_t>(c) * kSeg, seg[c].data(), sizeof(float) * kSeg);
        }
        const float* segP[2] = {seg[0].data(), seg[1].data()};
        stft.magnitude(segP, mag.data());

        std::vector<Ort::Value> ins, outs;
        ins.push_back(Ort::Value::CreateTensor<float>(mem, mixIn.data(), mixIn.size(), mixShape.data(), mixShape.size()));
        ins.push_back(Ort::Value::CreateTensor<float>(mem, mag.data(), mag.size(), magShape.data(), magShape.size()));
        outs.push_back(Ort::Value::CreateTensor<float>(mem, xf.data(), xf.size(), xfShape.data(), xfShape.size()));
        outs.push_back(Ort::Value::CreateTensor<float>(mem, xt.data(), xt.size(), xtShape.data(), xtShape.size()));
        Ort::RunOptions ro;
        session.Run(ro, inNames, ins.data(), 2, outNames, outs.data(), 2);

        for (int s = 0; s < S; ++s)
          stft.synth(xf.data() + static_cast<std::size_t>(s) * magN, xt.data() + static_cast<std::size_t>(s) * 2 * kSeg,
                     segOut.data() + static_cast<std::size_t>(s) * 2 * kSeg, segOut.data() + (static_cast<std::size_t>(s) * 2 + 1) * kSeg);
        if (!dumpDir.empty() && !dumped) {
          fs::create_directories(dumpDir);
          writeFloats(fs::path(dumpDir) / "mix.f32", mixIn.data(), mixIn.size());
          writeFloats(fs::path(dumpDir) / "mag.f32", mag.data(), mag.size());
          writeFloats(fs::path(dumpDir) / "x_freq.f32", xf.data(), xf.size());
          writeFloats(fs::path(dumpDir) / "x_time.f32", xt.data(), xt.size());
          writeFloats(fs::path(dumpDir) / "seg_out.f32", segOut.data(), segOut.size());
          dumped = true;
        }

        for (int s = 0; s < S; ++s)
          for (int c = 0; c < 2; ++c) {
            float* o = out.data() + (static_cast<std::size_t>(s) * 2 + static_cast<std::size_t>(c)) * static_cast<std::size_t>(length);
            const float* p = segOut.data() + (static_cast<std::size_t>(s) * 2 + static_cast<std::size_t>(c)) * kSeg + left;
            for (int k = 0; k < chunkLen; ++k) o[offset + k] += weight[static_cast<std::size_t>(k)] * p[k];
          }
        for (int k = 0; k < chunkLen; ++k) sumWeight[static_cast<std::size_t>(offset + k)] += weight[static_cast<std::size_t>(k)];
        ++cbCalls;  // progress callback: one call per segment, fraction cbCalls / totalChunks
      }
    }
    const double sepS = secondsSince(t0);
    const double stopS = cancel.fired() ? cancel.secondsSinceSet() : -1;

    if (!profile.empty()) {
      Ort::AllocatorWithDefaultOptions alloc;
      auto name = session.EndProfilingAllocated(alloc);
      std::printf("PROFILE %s\n", name.get());
    }

    static const char* names4[] = {"drums", "bass", "other", "vocals"};
    static const char* names6[] = {"drums", "bass", "other", "vocals", "guitar", "piano"};
    if (!cancelled) {
      fs::create_directories(outDir);
      for (int s = 0; s < S; ++s) {
        const float* o0 = out.data() + static_cast<std::size_t>(s) * 2 * static_cast<std::size_t>(length);
        const float* o1 = o0 + length;
        spike::writeStereoFloat32(fs::path(outDir) / (std::string(S == 4 ? names4[s] : names6[s]) + ".wav"), length,
                                  [&](int c, int i) {
                                    const float v = (c == 0 ? o0 : o1)[i] / sumWeight[static_cast<std::size_t>(i)];
                                    return static_cast<float>(v * refStd + refMean);  // undo normalisation
                                  });
      }
    }
    const double dur = length / 44100.0;
    std::printf("engine=onnx model=%s stems=%d threads=%d segments=%d\n", model.c_str(), S, threads, totalChunks);
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
