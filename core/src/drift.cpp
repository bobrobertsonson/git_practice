#include "sawblade/drift.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sawblade::drift {

double binCenterDb(int bin) noexcept { return kMinDb + (static_cast<double>(bin) + 0.5) * kBinDb; }

int binForDb(double db) noexcept {
  if (!(db > kMinDb)) return 0;
  const int b = static_cast<int>(std::floor((db - kMinDb) / kBinDb));
  return std::min(b, kBins - 1);
}

namespace {
std::atomic<std::uint64_t> g_nextTapId{1};
}

PeakTap::PeakTap() : id_(g_nextTapId.fetch_add(1, std::memory_order_relaxed)) {
  GateParams p;
  p.enabled = true;
  p.thresholdMode = GateThresholdMode::FloorRelative;
  floor_.setParams(p);
}

void PeakTap::prepare(double sampleRate) {
  winLen_ = std::max(1, static_cast<int>(std::llround(kWindowMs * 0.001 * sampleRate)));
  pos_ = 0;
  peak_ = 0.0f;
  floor_.prepare({sampleRate, winLen_});
  written_.store(0, std::memory_order_relaxed);
}

void PeakTap::process(const float* di, int n) noexcept {
  if (!enabled_.load(std::memory_order_relaxed)) return;
  int i = 0;
  while (i < n) {
    const int len = std::min(n - i, winLen_ - pos_);
    float pk = peak_;
    for (int k = 0; k < len; ++k) pk = std::max(pk, std::fabs(di[i + k]));
    peak_ = pk;
    floor_.followFloor(di + i, len);
    pos_ += len;
    i += len;
    if (pos_ >= winLen_) {
      const double db = 20.0 * std::log10(std::max(static_cast<double>(peak_), 1e-9));
      if (db >= floor_.floorEstimateDb() + kPlayedAboveFloorDb) {  // played
        const std::uint32_t w = written_.load(std::memory_order_relaxed);
        ring_[w % kRing].store(static_cast<std::uint16_t>(binForDb(db)), std::memory_order_relaxed);
        written_.store(w + 1, std::memory_order_release);
      }
      pos_ = 0;
      peak_ = 0.0f;
    }
  }
}

int PeakTap::read(std::uint32_t& cursor, std::uint16_t* out, int maxOut) const noexcept {
  const std::uint32_t w = written_.load(std::memory_order_acquire);
  if (cursor > w) cursor = 0;  // the tap was prepared again
  // Lapped: the oldest slot is the one the producer writes next, so it may be torn; resume one window later.
  if (w - cursor >= static_cast<std::uint32_t>(kRing)) cursor = w - kRing + 1;
  int n = 0;
  while (cursor != w && n < maxOut) out[n++] = ring_[cursor++ % kRing].load(std::memory_order_relaxed);
  return n;
}

std::string driftNoticeText(const DriftNotice& n) {
  return "Your input seems ~" + std::to_string(n.db) + " dB " + (n.hotter ? "hotter" : "quieter") +
         " than when you calibrated \xe2\x80\x94 did the interface gain change?";
}

DriftTracker::DriftTracker(const DriftConfig& cfg) : cfg_(cfg) {
  const auto windows = [](double s) { return std::max(1, static_cast<int>(std::llround(s * 1000.0 / kWindowMs))); };
  rollMax_ = std::min(windows(cfg.rollS), static_cast<int>(rollBins_.size()));
  learnNeed_ = windows(cfg.learnS);
  sustainNeed_ = windows(cfg.sustainS);
  rollMin_ = std::min(windows(cfg.minRollS), rollMax_);
}

void DriftTracker::reset() {
  rollHead_ = rollCount_ = learnCount_ = sustain_ = 0;
  rollHist_.fill(0);
  learnHist_.fill(0);
  baseline_.reset();
  newBaseline_.reset();
  ignored_.reset();
  active_ = false;
  drift_ = 0.0;
}

void DriftTracker::setBaseline(std::optional<double> p95Dbfs) {
  if (p95Dbfs == baseline_) return;
  reset();
  baseline_ = p95Dbfs;
}

double DriftTracker::p95Of(const std::array<int, kBins>& h, int total) const {
  const int need = static_cast<int>(std::ceil(0.95 * total));
  int cum = 0;
  for (int b = 0; b < kBins; ++b) {
    cum += h[static_cast<std::size_t>(b)];
    if (cum >= need) return binCenterDb(b);
  }
  return binCenterDb(kBins - 1);
}

std::optional<double> DriftTracker::rollingP95Db() const {
  if (rollCount_ < rollMin_) return std::nullopt;
  return p95Of(rollHist_, rollCount_);
}

int DriftTracker::consume(const PeakTap& tap) {
  if (tap.id() != tapId_) {
    tapId_ = tap.id();
    cursor_ = 0;
  }
  std::array<std::uint16_t, 64> buf{};
  int total = 0;
  for (;;) {
    const int n = tap.read(cursor_, buf.data(), static_cast<int>(buf.size()));
    if (n == 0) break;
    for (int i = 0; i < n; ++i) addWindow(buf[static_cast<std::size_t>(i)]);
    total += n;
  }
  return total;
}

void DriftTracker::addWindow(int bin) {
  bin = std::clamp(bin, 0, kBins - 1);
  if (rollCount_ == rollMax_) {  // drop the oldest
    --rollHist_[rollBins_[static_cast<std::size_t>(rollHead_)]];
    --rollCount_;
  }
  rollBins_[static_cast<std::size_t>(rollHead_)] = static_cast<std::uint16_t>(bin);
  rollHead_ = (rollHead_ + 1) % rollMax_;
  ++rollHist_[static_cast<std::size_t>(bin)];
  ++rollCount_;
  // Note: rollHead_ is the next write slot; the oldest window sits at rollHead_ once the ring is full.

  if (!baseline_) {
    ++learnHist_[static_cast<std::size_t>(bin)];
    if (++learnCount_ >= learnNeed_) {
      baseline_ = p95Of(learnHist_, learnCount_);
      newBaseline_ = baseline_;
    }
    return;
  }
  evaluate();
}

void DriftTracker::evaluate() {
  const auto p = rollingP95Db();
  if (!p) return;
  drift_ = *p - *baseline_;
  if (ignored_ && std::fabs(drift_) < cfg_.ignoreClearDb) ignored_.reset();  // back near the calibrated level: forget the ignore
  const double dev = drift_ - (ignored_ ? *ignored_ : 0.0);
  const bool beyond = std::fabs(dev) >= (active_ ? cfg_.clearBelowDb : cfg_.thresholdDb);
  if (!beyond) {
    sustain_ = 0;
    active_ = false;
    return;
  }
  if (sustain_ < sustainNeed_) ++sustain_;
  if (sustain_ >= sustainNeed_) active_ = true;
}

void DriftTracker::ignore() {
  if (!active_) return;
  ignored_ = drift_;
  active_ = false;
  sustain_ = 0;
}

std::optional<double> DriftTracker::takeNewBaseline() {
  auto v = newBaseline_;
  newBaseline_.reset();
  return v;
}

DriftNotice DriftTracker::notice() const {
  DriftNotice n;
  n.active = active_;
  n.db = static_cast<int>(std::lround(std::fabs(drift_)));
  n.hotter = drift_ >= 0.0;
  return n;
}

}  // namespace sawblade::drift
