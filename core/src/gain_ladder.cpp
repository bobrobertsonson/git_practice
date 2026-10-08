#include "sawblade/gain_ladder.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include "sawblade/capture_cache.h"
#include "sawblade/nam_block.h"

namespace sawblade {
namespace fs = std::filesystem;

std::vector<double> ladderPositions(const std::vector<LadderRung>& ladder) {
  std::vector<double> p(ladder.size(), 0.0);
  if (ladder.empty()) return p;
  double lo = ladder[0].gain, hi = ladder[0].gain;
  for (const auto& r : ladder) {
    lo = std::min(lo, r.gain);
    hi = std::max(hi, r.gain);
  }
  if (hi <= lo) return p;
  for (std::size_t i = 0; i < ladder.size(); ++i) p[i] = 10.0 * (ladder[i].gain - lo) / (hi - lo);
  return p;
}

int rungIndexOfModel(const std::vector<LadderRung>& ladder, const std::string& id) {
  for (std::size_t i = 0; i < ladder.size(); ++i)
    if (ladder[i].modelId == id) return static_cast<int>(i);
  return -1;
}

int ownRungIndex(const Capture& c) {
  if (!c.source || c.source->modelId.empty()) return -1;
  return rungIndexOfModel(c.ladder, c.source->modelId);
}

int selectRung(const std::vector<double>& p, int current, double knob) {
  const int n = static_cast<int>(p.size());
  if (n == 0) return -1;
  int cur = std::clamp(current, 0, n - 1);
  const auto at = [&](int i) { return p[static_cast<std::size_t>(i)]; };
  while (cur + 1 < n && knob >= 0.5 * (at(cur) + at(cur + 1)) + kLadderHysteresis) ++cur;
  while (cur > 0 && knob <= 0.5 * (at(cur - 1) + at(cur)) - kLadderHysteresis) --cur;
  return cur;
}

double ladderResidualDb(double knob, double position) {
  return std::clamp((knob - position) * kAmpDbPerKnob, -12.0, 12.0);
}

Capture rungCapture(const Capture& own, const LadderRung& r) {
  Capture c = own;
  c.ladder.clear();
  c.sha256.clear();
  c.file = r.modelId + ".nam";
  c.resolvedPath = fs::path();  // never exists: locateCapture() falls through to the capture cache
  if (c.source) c.source->modelId = r.modelId;
  return c;
}

std::optional<fs::path> locateRungFile(const Capture& own, const LadderRung& r) {
  if (!own.source) return std::nullopt;
  const Capture c = rungCapture(own, r);
  const fs::path p = locateCapture(c);
  std::error_code ec;
  if (p.empty() || !fs::exists(p, ec)) return std::nullopt;
  return p;
}

std::unique_ptr<Processor> buildRungProcessor(const NamBlockParams& block, int rung, const ProcessSpec& spec,
                                              CaptureCache* cache, std::string* why) {
  const auto fail = [&](const std::string& m) -> std::unique_ptr<Processor> {
    if (why) *why = m;
    return nullptr;
  };
  const auto& ladder = block.model.ladder;
  if (rung < 0 || rung >= static_cast<int>(ladder.size())) return fail("no such rung");
  const LadderRung& r = ladder[static_cast<std::size_t>(rung)];
  const Capture c = rungCapture(block.model, r);
  if (!locateRungFile(block.model, r)) return fail("rung " + r.modelId + " is not in the capture cache");
  NamBlockConfig cfg;
  cfg.inputGainDb = block.inputGainDb;
  cfg.outputGainDb = block.outputGainDb + block.makeupDb;
  cfg.normalizeLoudness = block.normalizeLoudness;
  cfg.makeupDb = block.makeupDb;
  try {
    std::unique_ptr<NamBlock> nb = cache ? NamBlock::load(*cache->namModel(c, "ladder." + r.modelId), cfg)
                                         : NamBlock::load(locateCapture(c), cfg);
    nb->prepare(spec);
    return nb;
  } catch (const std::exception& e) {
    return fail(std::string("rung ") + r.modelId + ": " + e.what());
  }
}

// --- LadderBlock -----------------------------------------------------------------------------------------
LadderBlock::LadderBlock(int rungCount, int activeRung, std::unique_ptr<Processor> active)
    : rungCount_(std::clamp(rungCount, 1, 64)), active_(activeRung), committed_(activeRung), target_(activeRung) {
  slots_.resize(static_cast<std::size_t>(rungCount_));
  if (activeRung < 0 || activeRung >= rungCount_ || !active) throw std::invalid_argument("LadderBlock: bad active rung");
  latency_ = active->latencySamples();
  levelInfo_ = active->levelInfo();
  slots_[static_cast<std::size_t>(activeRung)] = std::move(active);
  loaded_.store(1ull << activeRung);
}

LadderBlock::~LadderBlock() = default;

void LadderBlock::prepare(const ProcessSpec& spec) {
  maxBlock_ = std::max(1, spec.maxBlockSize);
  scratch_.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
  fadeLen_ = std::max(1, static_cast<int>(std::llround(kLadderFadeMs * 0.001 * spec.sampleRate)));
  fadeOld_.assign(static_cast<std::size_t>(fadeLen_) + 1, 0.0f);
  fadeNew_.assign(static_cast<std::size_t>(fadeLen_) + 1, 0.0f);
  for (int i = 0; i <= fadeLen_; ++i) {  // equal power: cos^2 + sin^2 = 1
    const double t = static_cast<double>(i) / fadeLen_ * std::numbers::pi * 0.5;
    fadeOld_[static_cast<std::size_t>(i)] = static_cast<float>(std::cos(t));
    fadeNew_[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(t));
  }
  warmLen_ = std::max(1, static_cast<int>(std::llround(kLadderWarmMs * 0.001 * spec.sampleRate)));
  warming_ = fading_ = false;
  const int a = active_.load();
  committed_.store(a);
  for (auto& s : slots_)
    if (s) s->prepare(spec);
}

void LadderBlock::reset() {
  warming_ = fading_ = false;
  committed_.store(active_.load());
  for (auto& s : slots_)
    if (s) s->reset();
}

bool LadderBlock::setLiveGainsDb(double inDb, double outDb, int rampSamples) noexcept {
  haveGains_ = true;
  gainIn_ = inDb;
  gainOut_ = outDb;
  gainRamp_ = rampSamples;
  bool any = false;
  for (auto& s : slots_)
    if (s) any = s->setLiveGainsDb(inDb, outDb, rampSamples) || any;
  return any;
}

void LadderBlock::setCalibration(const calibration::BlockCalibration& c, int rampSamples) noexcept {
  haveCal_ = true;
  cal_ = c;
  calRamp_ = rampSamples;
  for (auto& s : slots_)  // every rung plans its gain from its own capture metadata
    if (s) s->setCalibration(c, rampSamples);
}

void LadderBlock::setTargetRung(int rung) noexcept {
  if (rung >= 0 && rung < rungCount_) target_.store(rung, std::memory_order_relaxed);
}

void LadderBlock::drain() noexcept {
  Batch* b = slot_.current();
  if (b == nullptr || b->seq == seenSeq_) return;
  seenSeq_ = b->seq;
  for (Entry& e : b->entries) {
    if (e.rung < 0 || e.rung >= rungCount_) continue;
    const auto i = static_cast<std::size_t>(e.rung);
    const bool busy = e.rung == active_.load(std::memory_order_relaxed) || ((warming_ || fading_) && e.rung == fadeTo_);
    if (!e.proc) {
      if (!busy && slots_[i]) {
        std::swap(slots_[i], e.proc);  // the batch carries the old model away; the producer frees it
        loaded_.fetch_and(~(1ull << e.rung), std::memory_order_release);
      }
      continue;
    }
    if (busy) continue;  // never replace a model that is sounding
    if (e.proc->latencySamples() != latency_) {
      rejected_.fetch_or(1ull << e.rung, std::memory_order_release);  // dropped; the producer logs it
      continue;
    }
    if (haveCal_) e.proc->setCalibration(cal_, 1);
    if (haveGains_) e.proc->setLiveGainsDb(gainIn_, gainOut_, 1);
    std::swap(slots_[i], e.proc);
    loaded_.fetch_or(1ull << e.rung, std::memory_order_release);
  }
  consumedSeq_.store(seenSeq_, std::memory_order_release);
}

void LadderBlock::process(float* io, int n) noexcept {
  drain();
  while (n > 0) {
    int len = std::min(n, maxBlock_ > 0 ? maxBlock_ : n);
    const int tgt = target_.load(std::memory_order_relaxed);
    if (!warming_ && !fading_ && tgt != active_.load(std::memory_order_relaxed) && slot(tgt) != nullptr) {
      warming_ = true;
      fadeTo_ = tgt;
      fadePos_ = 0;  // counts warm-up samples while warming, fade samples while fading
    }
    Processor* cur = slot(active_.load(std::memory_order_relaxed));
    if (warming_) len = std::min(len, warmLen_ - fadePos_);
    if (fading_) len = std::min(len, fadeLen_ - fadePos_);
    if (!warming_ && !fading_) {
      if (cur) cur->process(io, len);
    } else {
      Processor* next = slot(fadeTo_);
      for (int i = 0; i < len; ++i) scratch_[static_cast<std::size_t>(i)] = io[i];
      if (cur) cur->process(io, len);
      next->process(scratch_.data(), len);
      if (fading_) {
        for (int i = 0; i < len; ++i) {
          const int k = std::min(fadePos_ + i, fadeLen_);
          io[i] = fadeOld_[static_cast<std::size_t>(k)] * io[i] + fadeNew_[static_cast<std::size_t>(k)] * scratch_[static_cast<std::size_t>(i)];
        }
      }
      fadePos_ += len;
      if (warming_ && fadePos_ >= warmLen_) {
        warming_ = false;
        fading_ = true;
        fadePos_ = 0;
        committed_.store(fadeTo_, std::memory_order_relaxed);
      } else if (fading_ && fadePos_ >= fadeLen_) {
        active_.store(fadeTo_, std::memory_order_relaxed);
        fading_ = false;
      }
    }
    io += len;
    n -= len;
  }
}

// --- producer side ---------------------------------------------------------------------------------------
bool LadderBlock::publishRungs(std::vector<Entry> entries) {
  std::lock_guard<std::mutex> lk(producerMutex_);
  for (Entry& e : entries) {
    if (e.rung < 0 || e.rung >= rungCount_) continue;
    staged_.erase(std::remove_if(staged_.begin(), staged_.end(), [&](const Entry& s) { return s.rung == e.rung; }), staged_.end());
    staged_.push_back(std::move(e));
  }
  slot_.collectGarbage();
  if (staged_.empty()) return true;
  if (consumedSeq_.load(std::memory_order_acquire) != publishedSeq_) return false;  // the audio thread has not taken the last batch
  auto b = std::make_unique<Batch>();
  b->seq = ++publishedSeq_;
  inflightMask_ = 0;
  for (const Entry& e : staged_)
    if (e.proc) inflightMask_ |= 1ull << e.rung;
  b->entries = std::move(staged_);
  staged_.clear();
  slot_.publish(std::move(b));
  return true;
}

bool LadderBlock::flushRungs() { return publishRungs({}); }

std::uint64_t LadderBlock::knownMask() const {
  auto* self = const_cast<LadderBlock*>(this);
  std::lock_guard<std::mutex> lk(self->producerMutex_);
  std::uint64_t m = loaded_.load(std::memory_order_acquire);
  if (consumedSeq_.load(std::memory_order_acquire) != publishedSeq_) m |= inflightMask_;
  for (const Entry& e : staged_)
    if (e.proc) m |= 1ull << e.rung;
  return m;
}

}  // namespace sawblade
