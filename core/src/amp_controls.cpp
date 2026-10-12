#include "sawblade/amp_controls.h"

#include <algorithm>
#include <cmath>

namespace sawblade {
namespace {

struct ToneDef {
  EqType type;
  double freq, q;
};
// BASS low shelf 100 Hz, MID peak 650 Hz Q 0.7, TREBLE high shelf 3 kHz, PRESENCE high shelf 5.5 kHz.
constexpr std::array<ToneDef, kAmpToneBands> kTone{{{EqType::LowShelf, 100.0, 0.7071067811865476},
                                                    {EqType::Peak, 650.0, 0.7},
                                                    {EqType::HighShelf, 3000.0, 0.7071067811865476},
                                                    {EqType::HighShelf, 5500.0, 0.7071067811865476}}};

double clampKnob(double v, double old) noexcept {
  if (!std::isfinite(v)) return old;
  return std::min(kAmpKnobMax, std::max(kAmpKnobMin, v));
}

double toneDb(int band, const AmpKnobs& k) noexcept {
  switch (band) {
    case 0: return ampKnobDb(k.bass);
    case 1: return ampKnobDb(k.mid);
    case 2: return ampKnobDb(k.treble);
    default: return ampPresenceDb(k.presence);
  }
}

}  // namespace

EqBand ampToneBand(int index, double gainDb) {
  const ToneDef& t = kTone[static_cast<std::size_t>(index)];
  EqBand b;
  b.type = t.type;
  b.freq = t.freq;
  b.gainDb = gainDb;
  b.q = t.q;
  return b;
}

// --- DbGain -----------------------------------------------------------------------------------------
void AmpStage::DbGain::setNow(double db) noexcept {
  cur = target = db;
  step = 0.0;
  remaining = 0;
  lin = static_cast<float>(std::pow(10.0, db / 20.0));
}

void AmpStage::DbGain::rampTo(double db, int samples) noexcept {
  if (db == target && remaining == 0) return;
  target = db;
  remaining = std::max(1, samples);
  step = (target - cur) / static_cast<double>(remaining);
}

void AmpStage::DbGain::process(float* io, int n) noexcept {
  int i = 0;
  for (; remaining > 0 && i < n; ++i) {
    --remaining;
    cur = remaining == 0 ? target : cur + step;
    lin = static_cast<float>(std::pow(10.0, cur / 20.0));
    io[i] *= lin;
  }
  if (i < n && cur != 0.0)
    for (; i < n; ++i) io[i] *= lin;
}

// --- AmpStage ---------------------------------------------------------------------------------------
void AmpStage::design(int band) noexcept {
  const auto b = static_cast<std::size_t>(band);
  EqBand e = ampToneBand(band, post_.band[b].cur);
  e.freq = std::min(e.freq, 0.45 * fs_);  // never throws: designBiquad's range check is always met
  post_.filter[b].setCoeffs(designBiquad(e, fs_));
}

void AmpStage::settle() noexcept {
  gain_.setNow(gain_.target);
  post_.level.setNow(post_.level.target);
  bool any = post_.level.active();
  for (int i = 0; i < kAmpToneBands; ++i) {
    GridRamp& r = post_.band[static_cast<std::size_t>(i)];
    r.cur = r.target;
    r.remaining = 0;
    post_.filter[static_cast<std::size_t>(i)].reset();
    post_.on[static_cast<std::size_t>(i)] = r.cur != 0.0;
    if (post_.on[static_cast<std::size_t>(i)]) design(i);
    any = any || post_.on[static_cast<std::size_t>(i)];
  }
  post_.idle = !any;
}

void AmpStage::prepare(const ProcessSpec& spec) {
  fs_ = spec.sampleRate;
  counter_ = 0;
  settle();
}

void AmpStage::reset() noexcept {
  counter_ = 0;
  settle();
}

void AmpStage::setKnobsNow(const AmpKnobs& k) noexcept {
  target_ = k;
  gain_.target = ampKnobDb(k.gain);
  post_.level.target = ampKnobDb(k.level);
  for (int i = 0; i < kAmpToneBands; ++i) post_.band[static_cast<std::size_t>(i)].target = toneDb(i, k);
  settle();
}

void AmpStage::setKnobs(const AmpKnobs& in, int rampSamples) noexcept {
  AmpKnobs k;
  k.gain = clampKnob(in.gain, target_.gain);
  k.bass = clampKnob(in.bass, target_.bass);
  k.mid = clampKnob(in.mid, target_.mid);
  k.treble = clampKnob(in.treble, target_.treble);
  k.presence = clampKnob(in.presence, target_.presence);
  k.level = clampKnob(in.level, target_.level);
  if (k == target_) return;
  if (k.gain != target_.gain) gain_.rampTo(ampKnobDb(k.gain), rampSamples);
  if (k.level != target_.level) {
    post_.level.rampTo(ampKnobDb(k.level), rampSamples);
    post_.idle = false;
  }
  for (int i = 0; i < kAmpToneBands; ++i) {
    const auto b = static_cast<std::size_t>(i);
    const double db = toneDb(i, k);
    if (db == post_.band[b].target) continue;
    GridRamp& r = post_.band[b];
    r.target = db;
    r.remaining = std::max(1, rampSamples);
    r.step = (db - r.cur) / static_cast<double>(r.remaining);
    post_.on[b] = true;  // a filter that was off has no state and the identity design
    post_.idle = false;
  }
  target_ = k;
}

void AmpStage::gridUpdate() noexcept {
  bool any = post_.level.active();
  for (int i = 0; i < kAmpToneBands; ++i) {
    const auto b = static_cast<std::size_t>(i);
    GridRamp& r = post_.band[b];
    if (r.remaining > 0) {
      if (r.remaining <= kGrid) {
        r.cur = r.target;
        r.remaining = 0;
      } else {
        r.cur += r.step * kGrid;
        r.remaining -= kGrid;
      }
      design(i);
    }
    if (post_.on[b] && r.remaining == 0 && r.cur == 0.0) {  // back to flat: stop running it
      post_.on[b] = false;
      post_.filter[b].reset();
    }
    any = any || post_.on[b];
  }
  post_.idle = !any;
}

void AmpStage::processPre(float* io, int n) noexcept { gain_.process(io, n); }

void AmpStage::processPost(float* io, int n) noexcept {
  if (post_.idle) {  // exact skip (the grid keeps counting)
    counter_ += static_cast<std::uint64_t>(n);
    return;
  }
  int pos = 0;
  while (pos < n) {
    const int phase = static_cast<int>(counter_ % static_cast<std::uint64_t>(kGrid));
    if (phase == 0) {
      gridUpdate();
      if (post_.idle) {
        counter_ += static_cast<std::uint64_t>(n - pos);
        return;
      }
    }
    const int len = std::min(n - pos, kGrid - phase);
    for (std::size_t b = 0; b < post_.filter.size(); ++b)
      if (post_.on[b]) post_.filter[b].process(io + pos, len);
    post_.level.process(io + pos, len);
    counter_ += static_cast<std::uint64_t>(len);
    pos += len;
  }
}

}  // namespace sawblade
