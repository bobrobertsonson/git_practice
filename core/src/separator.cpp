// Phase 5.1b separator. The engine around the exported htdemucs core network mirrors demucs 4.0.1
// apply_model(shifts=0, split=True, overlap=0.25), as proven in spikes/separator/separator_onnx.cpp:
// per-song normalisation, segmentation with Python's TensorChunk.padded context, STFT / magnitude
// (HTDemucs._spec + _magnitude), mask + iSTFT (HTDemucs._mask + _ispec), time-branch add, linear-ramp
// overlap-add. Differences from the spike driver: every segment is overlap-added straight into the
// per-stem accumulators (no per-segment output list, no whole-song intermediate), and cancel also
// interrupts a running network evaluation.
#include "sawblade/separator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include <onnxruntime_cxx_api.h>
#include <pffft.h>

#include "sawblade/resample.h"

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace sawblade {
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

// FTZ/DAZ on the calling thread (ORT's pool threads get it from the session option).
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

std::size_t sz(int v) { return static_cast<std::size_t>(v); }

// STFT / iSTFT matching demucs.spec.spectro / ispectro with the HTDemucs crop/pad logic.
class Stft {
 public:
  Stft() {
    setup_ = pffft_new_setup(kNfft, PFFFT_REAL);
    if (!setup_) throw std::runtime_error("separator: pffft setup failed");
    buf_ = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * kNfft));
    out_ = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * kNfft));
    work_ = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * kNfft));
    window_.resize(kNfft);
    for (int n = 0; n < kNfft; ++n)  // torch.hann_window(4096), periodic
      window_[sz(n)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * n / kNfft));
    xp_.resize(kPadded);
    y_.resize(kIstftLen);
    env_.assign(kIstftLen, 0.0f);
    for (int g = 0; g < kIstftFrames; ++g)
      for (int n = 0; n < kNfft; ++n) env_[sz(g * kHop + n)] += window_[sz(n)] * window_[sz(n)];
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
        xp_[sz(i)] = mix[c][p];
      }
      for (int j = 0; j < kFrames; ++j) {  // frames 2..2+le of the centred STFT == xp[j*hop .. +nfft]
        for (int n = 0; n < kNfft; ++n) buf_[n] = xp_[sz(j * kHop + n)] * window_[sz(n)];
        pffft_transform_ordered(setup_, buf_, out_, work_, PFFFT_FORWARD);
        constexpr float kScale = 1.0f / 64.0f;  // normalized=True: n_fft^-0.5
        float* re = mag + sz(c * 2) * kBins * kFrames;
        float* im = re + sz(kBins) * kFrames;
        re[j] = out_[0] * kScale;  // bin 0: imaginary part is exactly 0
        im[j] = 0.0f;
        for (int k = 1; k < kBins; ++k) {
          re[sz(k) * kFrames + sz(j)] = out_[2 * k] * kScale;
          im[sz(k) * kFrames + sz(j)] = out_[2 * k + 1] * kScale;
        }
      }
    }
  }

  // xf: one source, (4, kBins, kFrames) cac spectrogram; xt: (2, kSeg) time-branch output of the same
  // source. Adds weight[k] * (xt + istft(mask))[left + k] to acc[c][offset + k] for k in [0, chunkLen).
  void synthAccumulate(const float* xf, const float* xt, float* const acc[2], std::size_t offset, int left,
                       int chunkLen, const float* weight) {
    for (int c = 0; c < 2; ++c) {
      std::fill(y_.begin(), y_.end(), 0.0f);
      const float* re = xf + sz(c * 2) * kBins * kFrames;
      const float* im = re + sz(kBins) * kFrames;
      for (int j = 0; j < kFrames; ++j) {
        buf_[0] = re[j];
        buf_[1] = 0.0f;  // Nyquist bin added by _ispec's F.pad is zero
        for (int k = 1; k < kBins; ++k) {
          buf_[2 * k] = re[sz(k) * kFrames + sz(j)];
          buf_[2 * k + 1] = im[sz(k) * kFrames + sz(j)];
        }
        pffft_transform_ordered(setup_, buf_, out_, work_, PFFFT_BACKWARD);
        constexpr float kScale = 1.0f / 64.0f;  // normalized=True: *sqrt(n_fft) then irfft's 1/n_fft
        float* y = y_.data() + sz(j + 2) * kHop;
        for (int n = 0; n < kNfft; ++n) y[n] += out_[n] * kScale * window_[sz(n)];
      }
      const float* xtc = xt + sz(c) * kSeg;
      float* a = acc[c] + offset;
      for (int k = 0; k < chunkLen; ++k) {
        const std::size_t n = sz(left + k);
        const std::size_t i = sz(kNfft / 2 + kPad) + n;  // center trim, then x[pad:pad+len]
        a[k] += weight[k] * (xtc[n] + y_[i] / env_[i]);
      }
    }
  }

 private:
  PFFFT_Setup* setup_ = nullptr;
  float *buf_ = nullptr, *out_ = nullptr, *work_ = nullptr;
  std::vector<float> window_, xp_, y_, env_;
};

// Clears the cancel hook before the objects it touches are destroyed.
struct HookGuard {
  CancelToken& t;
  ~HookGuard() { t.clearHook(); }
};

}  // namespace

int defaultSeparatorThreads() {
  const unsigned hc = std::thread::hardware_concurrency();
  return std::max(1, static_cast<int>(hc) - 1);
}

struct Separator::Impl {
  SeparationModel model;
  int threads;
  int sources;
  Ort::Env env;
  Ort::SessionOptions so;
  std::unique_ptr<Ort::Session> session;

  Impl(const std::filesystem::path& file, SeparationModel m, int t)
      : model(m), threads(t), sources(separationModelSourceCount(m)), env(ORT_LOGGING_LEVEL_ERROR, "sawblade_separator") {
    so.SetIntraOpNumThreads(threads);
    so.SetInterOpNumThreads(1);
    so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    so.AddConfigEntry("session.set_denormal_as_zero", "1");
    so.DisableCpuMemArena();  // ORT defaults peak at 5 GB on a 4 minute song, 2.5 GB without the arena
    so.DisableMemPattern();   // measured faster and smaller (RESULTS.md, 5.1a)
#ifdef _WIN32
    session = std::make_unique<Ort::Session>(env, file.wstring().c_str(), so);
#else
    session = std::make_unique<Ort::Session>(env, file.string().c_str(), so);
#endif
    if (session->GetInputCount() != 2 || session->GetOutputCount() != 2)
      throw std::runtime_error("separator: " + file.string() + " is not a htdemucs core graph (need 2 inputs, 2 outputs)");
    const auto shape = session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();  // (1,S,4,F,T)
    if (shape.size() != 5 || shape[1] != sources || shape[3] != kBins || shape[4] != kFrames)
      throw std::runtime_error("separator: " + file.string() + " does not have the expected output shape for " +
                               separationModelId(model));
  }
};

Separator::Separator(const std::filesystem::path& onnxFile, SeparationModel model, const SeparatorOptions& opts) {
  try {
    impl_ = std::make_unique<Impl>(onnxFile, model, opts.threads > 0 ? opts.threads : defaultSeparatorThreads());
  } catch (const Ort::Exception& e) {
    throw std::runtime_error("separator: cannot load " + onnxFile.string() + ": " + e.what());
  }
}
Separator::~Separator() = default;
SeparationModel Separator::model() const noexcept { return impl_->model; }
int Separator::threads() const noexcept { return impl_->threads; }

SeparationResult Separator::separate(AudioFile audio, const SeparationProgress& progress, CancelToken& cancel) {
  using Clock = std::chrono::steady_clock;
  if (audio.channels < 1 || audio.sampleRate <= 0.0) throw std::runtime_error("separator: invalid audio");
  const int S = impl_->sources;

  // ---- input: planar stereo at 44.1 kHz --------------------------------------------------------
  std::vector<float> in[2];
  {
    const std::size_t ch = static_cast<std::size_t>(audio.channels), frames = audio.interleaved.size() / ch;
    if (frames == 0) throw std::runtime_error("separator: no audio");
    for (std::size_t c = 0; c < 2; ++c) {
      if (c == 1 && ch == 1) {
        in[1] = in[0];
        break;
      }
      std::vector<float> x(frames);
      for (std::size_t i = 0; i < frames; ++i) x[i] = audio.interleaved[i * ch + c];
      in[c] = audio.sampleRate == kSeparatorSampleRate ? std::move(x) : resample(x, audio.sampleRate, kSeparatorSampleRate);
    }
    audio.interleaved = {};
    audio.interleaved.shrink_to_fit();
  }
  const std::size_t N = in[0].size();
  if (N == 0 || N > 0x7fffffffu - kSeg) throw std::runtime_error("separator: unsupported audio length");
  const int length = static_cast<int>(N);

  // ---- per-song normalisation: ref = mix.mean(0); (mix - ref.mean()) / ref.std() -----------------
  double sum = 0;
  for (std::size_t i = 0; i < N; ++i) sum += 0.5 * (static_cast<double>(in[0][i]) + in[1][i]);
  const double refMean = sum / static_cast<double>(N);
  double ss = 0;
  for (std::size_t i = 0; i < N; ++i) {
    const double d = 0.5 * (static_cast<double>(in[0][i]) + in[1][i]) - refMean;
    ss += d * d;
  }
  double refStd = N > 1 ? std::sqrt(ss / static_cast<double>(N - 1)) : 0.0;  // unbiased, like torch.std
  if (!(refStd > 0.0)) refStd = 1.0;  // digital silence: demucs would divide by zero
  for (int c = 0; c < 2; ++c)
    for (std::size_t i = 0; i < N; ++i) in[c][i] = static_cast<float>((in[c][i] - refMean) / refStd);

  // ---- buffers ---------------------------------------------------------------------------------
  const std::size_t magN = sz(4) * kBins * kFrames;
  std::vector<float> mag(magN), xf(sz(S) * magN), xt(sz(S) * 2 * kSeg), mixIn(sz(2) * kSeg);
  std::vector<std::vector<float>> acc(sz(S) * 2, std::vector<float>(N, 0.0f));  // [s*2 + c][i]
  std::vector<float> sumWeight(N, 0.0f);
  std::vector<float> weight(kSeg);
  for (int k = 0; k < kSeg / 2; ++k) {
    weight[sz(k)] = static_cast<float>(k + 1) / static_cast<float>(kSeg / 2);
    weight[sz(kSeg - 1 - k)] = weight[sz(k)];
  }
  Stft stft;

  Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const std::array<int64_t, 3> mixShape{1, 2, kSeg};
  const std::array<int64_t, 4> magShape{1, 4, kBins, kFrames};
  const std::array<int64_t, 5> xfShape{1, S, 4, kBins, kFrames};
  const std::array<int64_t, 4> xtShape{1, S, 2, kSeg};
  const char* inNames[] = {"mix", "mag"};
  const char* outNames[] = {"x_freq", "x_time"};

  const int stride = static_cast<int>((1 - 0.25f) * static_cast<float>(kSeg));  // 257985, as demucs.apply
  const int totalChunks = (length + stride - 1) / stride;

  Ort::RunOptions ro;
  cancel.setHook([&ro] { ro.SetTerminate(); });
  HookGuard hookGuard{cancel};
  if (cancel.cancelled()) throw SeparationCancelled();
  if (progress) progress(0.0, -1.0);

  const auto t0 = Clock::now();
  int done = 0;
  {
    FtzCaller ftz;
    std::vector<float> seg0(kSeg), seg1(kSeg);
    for (int offset = 0; offset < length; offset += stride) {
      if (cancel.cancelled()) throw SeparationCancelled();
      const int chunkLen = std::min(kSeg, length - offset);
      const int delta = kSeg - chunkLen, left = delta / 2;  // TensorChunk.padded(valid_length)
      const int start = offset - left;
      const int cs = std::max(0, start), ce = std::min(length, start + kSeg);
      float* segs[2] = {seg0.data(), seg1.data()};
      for (int c = 0; c < 2; ++c) {
        std::fill(segs[c], segs[c] + kSeg, 0.0f);
        if (ce > cs) std::memcpy(segs[c] + (cs - start), in[c].data() + cs, sizeof(float) * sz(ce - cs));
        std::memcpy(mixIn.data() + sz(c) * kSeg, segs[c], sizeof(float) * kSeg);
      }
      const float* segP[2] = {seg0.data(), seg1.data()};
      stft.magnitude(segP, mag.data());

      try {
        std::vector<Ort::Value> ins, outs;
        ins.push_back(Ort::Value::CreateTensor<float>(mem, mixIn.data(), mixIn.size(), mixShape.data(), mixShape.size()));
        ins.push_back(Ort::Value::CreateTensor<float>(mem, mag.data(), mag.size(), magShape.data(), magShape.size()));
        outs.push_back(Ort::Value::CreateTensor<float>(mem, xf.data(), xf.size(), xfShape.data(), xfShape.size()));
        outs.push_back(Ort::Value::CreateTensor<float>(mem, xt.data(), xt.size(), xtShape.data(), xtShape.size()));
        impl_->session->Run(ro, inNames, ins.data(), 2, outNames, outs.data(), 2);
      } catch (const Ort::Exception& e) {
        if (cancel.cancelled()) throw SeparationCancelled();
        throw std::runtime_error(std::string("separator: network evaluation failed: ") + e.what());
      }
      if (cancel.cancelled()) throw SeparationCancelled();  // terminate may leave partial outputs

      for (int s = 0; s < S; ++s) {
        float* a[2] = {acc[sz(s) * 2].data(), acc[sz(s) * 2 + 1].data()};
        stft.synthAccumulate(xf.data() + sz(s) * magN, xt.data() + sz(s) * 2 * kSeg, a, sz(offset), left, chunkLen, weight.data());
      }
      for (int k = 0; k < chunkLen; ++k) sumWeight[sz(offset + k)] += weight[sz(k)];

      ++done;
      if (progress) {
        const double el = std::chrono::duration<double>(Clock::now() - t0).count();
        progress(static_cast<double>(done) / totalChunks,
                 done < totalChunks ? el / done * (totalChunks - done) : 0.0);
      }
    }
  }
  in[0] = {};  // (shrink: the input is no longer needed)
  in[1] = {};
  mag = {};
  xf = {};
  xt = {};
  mixIn = {};

  // ---- finalise: divide by the weight sum, undo the normalisation, map the sources ------------------
  for (auto& a : acc)
    for (std::size_t i = 0; i < N; ++i) {
      const float v = a[i] / sumWeight[i];
      a[i] = static_cast<float>(v * refStd + refMean);
    }
  sumWeight = {};

  SeparationResult res;
  res.length = static_cast<std::int64_t>(N);
  res.segments = totalChunks;
  // Source order of htdemucs: drums, bass, other, vocals, [guitar, piano].
  static constexpr StemKind order[6] = {StemKind::Drums, StemKind::Bass,   StemKind::Other,
                                        StemKind::Vocals, StemKind::Guitar, StemKind::Other /* piano */};
  for (int s = 0; s < S; ++s) {
    const auto k = static_cast<std::size_t>(order[s]);
    std::vector<float>& l = acc[sz(s) * 2];
    std::vector<float>& r = acc[sz(s) * 2 + 1];
    if (s == 5) {  // piano: no StemKind of its own, summed into other
      auto& o = *res.stems[k];
      for (std::size_t i = 0; i < N; ++i) {
        o[0][i] += l[i];
        o[1][i] += r[i];
      }
    } else {
      res.stems[k] = StemAudio{std::move(l), std::move(r)};
    }
    l = {};
    r = {};
  }
  return res;
}

}  // namespace sawblade
