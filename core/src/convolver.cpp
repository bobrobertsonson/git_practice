#include "sawblade/convolver.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "pffft.h"

namespace sawblade {
namespace {

constexpr int P = Convolver::kPartition;
constexpr int N = Convolver::kFftSize;

struct AlignedFree {
  void operator()(float* p) const noexcept { pffft_aligned_free(p); }
};
using AlignedBuf = std::unique_ptr<float[], AlignedFree>;

AlignedBuf makeAligned(std::size_t count) {
  auto* p = static_cast<float*>(pffft_aligned_malloc(count * sizeof(float)));
  if (!p) throw std::bad_alloc();
  std::memset(p, 0, count * sizeof(float));
  return AlignedBuf(p);
}

struct SetupDeleter {
  void operator()(PFFFT_Setup* s) const noexcept { pffft_destroy_setup(s); }
};

}  // namespace

struct Convolver::Impl {
  std::unique_ptr<PFFFT_Setup, SetupDeleter> setup;

  // Head (taps 0..P-1), stored reversed so the dot product runs oldest -> newest.
  float headRev[P] = {};
  // Doubled ring of the last P inputs: window = hist[w+1 .. w+P], oldest -> newest.
  float hist[2 * P] = {};
  int w = 0;

  // Tail.
  int numPartitions = 0;          // partitions q = 1..numPartitions
  AlignedBuf kernelSpec;          // numPartitions * N floats; partition q at (q-1)*N
  AlignedBuf fdl;                 // numPartitions * N floats; ring of input spectra
  int fdlNewest = 0;              // index of the newest spectrum (S_b)
  AlignedBuf prevBlock, curBlock; // P floats each (separate so we can build [prev, cur])
  AlignedBuf fftIn, fftOut, acc, work;  // N floats each
  float tailBlock[P] = {};        // tail contribution for the block being received
  int fill = 0;                   // samples received in the current block

  void clearState() {
    std::memset(hist, 0, sizeof hist);
    w = 0;
    fill = 0;
    fdlNewest = 0;
    std::memset(tailBlock, 0, sizeof tailBlock);
    if (numPartitions > 0) {
      std::memset(fdl.get(), 0, sizeof(float) * static_cast<std::size_t>(numPartitions) * N);
      std::memset(prevBlock.get(), 0, sizeof(float) * P);
      std::memset(curBlock.get(), 0, sizeof(float) * P);
    }
  }

  // Called when a full block of P input samples (curBlock) has arrived: computes the tail
  // contribution for the *next* block (needs only input up to now since all tail taps >= P).
  void boundary() noexcept {
    std::memcpy(fftIn.get(), prevBlock.get(), sizeof(float) * P);
    std::memcpy(fftIn.get() + P, curBlock.get(), sizeof(float) * P);
    fdlNewest = (fdlNewest + 1) % numPartitions;
    float* newest = fdl.get() + static_cast<std::size_t>(fdlNewest) * N;
    pffft_transform(setup.get(), fftIn.get(), newest, work.get(), PFFFT_FORWARD);

    std::memset(acc.get(), 0, sizeof(float) * N);
    // Partition q (1-based) pairs with spectrum S_{b+1-q}: q-1 slots behind the newest.
    for (int q = 1; q <= numPartitions; ++q) {
      int idx = fdlNewest - (q - 1);
      if (idx < 0) idx += numPartitions;
      pffft_zconvolve_accumulate(setup.get(), fdl.get() + static_cast<std::size_t>(idx) * N,
                                 kernelSpec.get() + static_cast<std::size_t>(q - 1) * N, acc.get(),
                                 1.0f / static_cast<float>(N));
    }
    pffft_transform(setup.get(), acc.get(), fftOut.get(), work.get(), PFFFT_BACKWARD);
    std::memcpy(tailBlock, fftOut.get() + P, sizeof tailBlock);  // overlap-save: last P are valid
    std::swap(prevBlock, curBlock);
  }
};

Convolver::Convolver() : impl_(std::make_unique<Impl>()) {}
Convolver::~Convolver() = default;

void Convolver::setIr(const std::vector<float>& ir) {
  if (ir.empty()) throw std::invalid_argument("Convolver: empty impulse response");
  auto impl = std::make_unique<Impl>();

  for (int k = 0; k < P; ++k)
    impl->headRev[P - 1 - k] = static_cast<std::size_t>(k) < ir.size() ? ir[static_cast<std::size_t>(k)] : 0.0f;

  if (ir.size() > static_cast<std::size_t>(P)) {
    const std::size_t tailLen = ir.size() - P;
    impl->numPartitions = static_cast<int>((tailLen + P - 1) / P);
    impl->setup.reset(pffft_new_setup(N, PFFFT_REAL));
    if (!impl->setup) throw std::runtime_error("Convolver: FFT setup failed");
    const auto np = static_cast<std::size_t>(impl->numPartitions);
    impl->kernelSpec = makeAligned(np * N);
    impl->fdl = makeAligned(np * N);
    impl->prevBlock = makeAligned(P);
    impl->curBlock = makeAligned(P);
    impl->fftIn = makeAligned(N);
    impl->fftOut = makeAligned(N);
    impl->acc = makeAligned(N);
    impl->work = makeAligned(N);
    for (std::size_t q = 1; q <= np; ++q) {
      std::memset(impl->fftIn.get(), 0, sizeof(float) * N);
      const std::size_t start = q * P;
      const std::size_t cnt = std::min<std::size_t>(P, ir.size() - start);
      std::memcpy(impl->fftIn.get(), ir.data() + start, cnt * sizeof(float));
      pffft_transform(impl->setup.get(), impl->fftIn.get(), impl->kernelSpec.get() + (q - 1) * N,
                      impl->work.get(), PFFFT_FORWARD);
    }
  }
  impl->clearState();
  impl_ = std::move(impl);
  irLength_ = ir.size();
}

void Convolver::prepare(const ProcessSpec&) { reset(); }

void Convolver::reset() noexcept { impl_->clearState(); }

void Convolver::process(float* io, int numSamples) noexcept {
  Impl& s = *impl_;
  if (irLength_ == 0) return;  // no IR: passthrough
  int done = 0;
  while (done < numSamples) {
    const int c = s.numPartitions > 0 ? std::min(numSamples - done, P - s.fill) : numSamples - done;
    for (int i = 0; i < c; ++i) {
      const float x = io[done + i];
      s.hist[s.w] = x;
      s.hist[s.w + P] = x;
      const float* win = s.hist + s.w + 1;
      float a0 = 0.f, a1 = 0.f, a2 = 0.f, a3 = 0.f;
      for (int k = 0; k < P; k += 4) {
        a0 += s.headRev[k] * win[k];
        a1 += s.headRev[k + 1] * win[k + 1];
        a2 += s.headRev[k + 2] * win[k + 2];
        a3 += s.headRev[k + 3] * win[k + 3];
      }
      float y = (a0 + a1) + (a2 + a3);
      if (++s.w == P) s.w = 0;
      if (s.numPartitions > 0) {
        y += s.tailBlock[s.fill + i];
        s.curBlock[s.fill + i] = x;
      }
      io[done + i] = y;
    }
    done += c;
    if (s.numPartitions > 0) {
      s.fill += c;
      if (s.fill == P) {
        s.boundary();
        s.fill = 0;
      }
    }
  }
}

}  // namespace sawblade
