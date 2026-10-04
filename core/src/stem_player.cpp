#include "sawblade/stem_player.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace sawblade {
namespace {

double dbToLin(double db) noexcept { return std::pow(10.0, db / 20.0); }

int msToSamples(double ms, double fs) noexcept {
  const auto n = static_cast<long long>(std::llround(ms * 0.001 * fs));
  return n < 1 ? 1 : static_cast<int>(n);
}

double clampDb(double db, double lo, double hi) noexcept {
  if (!(db == db)) return lo;  // NaN
  return db < lo ? lo : (db > hi ? hi : db);
}

}  // namespace

StemPlayer::StemPlayer() {
  gainDb_.fill(0.0);
  mute_.fill(false);
  solo_.fill(false);
  g_.fill(0.0);
}

StemPlayer::~StemPlayer() = default;

// ---- producer thread ------------------------------------------------------------------------
void StemPlayer::prepare(const ProcessSpec& spec, int maxRigLatencySamples) {
  if (!(spec.sampleRate > 0.0) || !std::isfinite(spec.sampleRate))
    throw std::invalid_argument("StemPlayer::prepare: sample rate must be positive");
  if (spec.maxBlockSize < 1) throw std::invalid_argument("StemPlayer::prepare: maxBlockSize must be >= 1");
  if (maxRigLatencySamples < 0) throw std::invalid_argument("StemPlayer::prepare: negative max rig latency");

  fs_ = spec.sampleRate;
  maxBlock_ = spec.maxBlockSize;
  maxLatency_ = maxRigLatencySamples;
  nTrans_ = msToSamples(kTransportFadeMs, fs_);
  nSeek_ = msToSamples(kSeekFadeMs, fs_);
  nLoop_ = msToSamples(kLoopFadeMs, fs_);
  nMix_ = msToSamples(kMixRampMs, fs_);

  auto fade = [](int n, std::vector<float>& in, std::vector<float>& out) {
    in.assign(static_cast<std::size_t>(n), 0.0f);
    out.assign(static_cast<std::size_t>(n), 0.0f);
    for (int k = 0; k < n; ++k) {
      const double t = (static_cast<double>(k) + 0.5) / static_cast<double>(n);
      in[static_cast<std::size_t>(k)] = static_cast<float>(std::sin(0.5 * std::numbers::pi * t));
      out[static_cast<std::size_t>(k)] = static_cast<float>(std::cos(0.5 * std::numbers::pi * t));
    }
  };
  fade(nSeek_, seekIn_, seekOut_);
  fade(nLoop_, loopIn_, loopOut_);

  const auto clickLen = static_cast<std::size_t>(std::llround(kClickMs * 0.001 * fs_));
  auto unit = [&](double hz, std::vector<double>& u) {
    u.assign(clickLen, 0.0);
    for (std::size_t n = 0; n < clickLen; ++n) {
      const double x = static_cast<double>(n);
      u[n] = std::sin(2.0 * std::numbers::pi * hz * x / fs_) * std::exp(-x / (kClickDecayMs * 0.001 * fs_));
    }
  };
  unit(kClickAccentHz, accentUnit_);
  unit(kClickNormalHz, normalUnit_);
  accent_.assign(clickLen, 0.0f);
  normal_.assign(clickLen, 0.0f);
  rebuildClicks();

  delayL_.setMaxDelaySamples(maxLatency_);
  delayR_.setMaxDelaySamples(maxLatency_);
  delayL_.prepare(spec);
  delayR_.prepare(spec);
  latencyReq_ = std::clamp(latencyReq_, 0, maxLatency_);

  if (set_ != nullptr && set_->sampleRate != fs_) {  // rate changed: drop the adopted set
    set_ = nullptr;
    len_ = 0;
    nActive_ = 0;
    ch_.fill(nullptr);
  }
  reset();
}

void StemPlayer::reset() {
  playCmd_ = PlayCmd::None;
  seekCmd_ = false;
  wantPlay_ = false;
  advancing_ = false;
  tgOn_ = false;
  tg_.snap(0.0);
  pos_ = oldPos_ = 0;
  xfading_ = false;
  xfK_ = xfN_ = 0;
  pendingSeek_ = false;
  wrapPending_ = false;
  loopActive_ = false;
  loopA_ = loopB_ = 0;
  countingIn_ = false;
  clickBuf_ = nullptr;
  clickIdx_ = clickLeft_ = 0;
  delayL_.reset();
  delayR_.reset();
  snapMix();
}

void StemPlayer::setStemSet(std::unique_ptr<StemSet> set) {
  if (!set) throw std::invalid_argument("StemPlayer::setStemSet: null set");
  if (fs_ <= 0.0 || set->sampleRate != fs_)
    throw std::invalid_argument("StemPlayer::setStemSet: set sample rate differs from the prepared rate");
  slot_.publish(std::move(set));
}

// ---- command application (audio thread) -------------------------------------------------------
void StemPlayer::adoptIfStopped() noexcept {
  if (advancing_ || countingIn_) return;
  const StemSet* s = slot_.current();
  if (s == nullptr || s == set_ || s->sampleRate != fs_) return;
  set_ = s;
  len_ = s->length;
  nActive_ = 0;
  ch_.fill(nullptr);
  for (int k = 0; k < kStemKindCount; ++k) {
    const auto ks = static_cast<std::size_t>(k);
    if (!s->present[ks]) continue;
    ch_[ks * 2] = s->audio[ks][0].data();
    ch_[ks * 2 + 1] = s->audio[ks][1].data();
    active_[static_cast<std::size_t>(nActive_++)] = k;
  }
  pos_ = 0;
  loopActive_ = false;
  wrapPending_ = false;
  xfading_ = false;
  pendingSeek_ = false;
}

void StemPlayer::updateMixTargets() noexcept {
  const bool anySolo = std::any_of(solo_.begin(), solo_.end(), [](bool b) { return b; });
  for (int k = 0; k < kStemKindCount; ++k) {
    const auto ks = static_cast<std::size_t>(k);
    const bool audible = !mute_[ks] && (!anySolo || solo_[ks]);
    double f = audible ? 1.0 : 0.0;
    if (k == static_cast<int>(StemKind::Guitar)) {
      if (guitarMode_ == GuitarMode::Muted) f = 0.0;
      else if (guitarMode_ == GuitarMode::Ghost) f *= dbToLin(kGhostGuideDb);
    }
    const double t = dbToLin(gainDb_[ks]) * f;
    if (t != stemRamp_[ks].target) stemRamp_[ks].begin(t, nMix_);
  }
  const double m = dbToLin(masterDb_);
  if (m != masterRamp_.target) masterRamp_.begin(m, nMix_);
}

void StemPlayer::snapMix() noexcept {
  updateMixTargets();
  for (auto& r : stemRamp_) r.snap(r.target);
  masterRamp_.snap(masterRamp_.target);
}

void StemPlayer::startPlaying() noexcept {
  advancing_ = true;
  tgOn_ = true;
  tg_.begin(1.0, nTrans_);
}

void StemPlayer::startPause() noexcept {
  tgOn_ = false;
  tg_.begin(0.0, nTrans_);
}

void StemPlayer::stopTransport() noexcept {
  advancing_ = false;
  tgOn_ = false;
  tg_.snap(0.0);
  xfading_ = false;
  pendingSeek_ = false;
  wrapPending_ = false;
}

std::int64_t StemPlayer::beatStart(std::int64_t k) const noexcept {
  return static_cast<std::int64_t>(std::llround(static_cast<double>(k) * 60.0 * fs_ / ciBpmActive_));
}

void StemPlayer::startCountIn() noexcept {
  ciBpmActive_ = ciBpm_;
  ciBpbActive_ = ciBpb_;
  ciBeats_ = static_cast<std::int64_t>(ciBars_) * ciBpb_;
  ciTotal_ = beatStart(ciBeats_);
  ciPos_ = 0;
  ciBeat_ = 0;
  ciNextAt_ = 0;
  countingIn_ = true;
}

void StemPlayer::applyHostFollow() noexcept {
  countingIn_ = false;  // the host owns the position: no internal count-in
  if (hostPlaying_) {
    wantPlay_ = true;
    if (!advancing_) {
      pos_ = hostSample_;
      startPlaying();
      return;
    }
    if (!tgOn_) {
      tgOn_ = true;
      tg_.begin(1.0, nTrans_);
    }
    const std::int64_t diff = hostSample_ >= pos_ ? hostSample_ - pos_ : pos_ - hostSample_;
    if (diff > hostThreshold_) {
      if (xfading_) {
        pendingSeek_ = true;
        pendingSeekFollow_ = true;
        pendingSeekPos_ = hostSample_;
        pendingSeekAge_ = 0;
      } else {
        doSeek(hostSample_);
      }
    } else {
      pendingSeek_ = false;
    }
  } else {
    wantPlay_ = false;
    if (advancing_ && tgOn_) startPause();
  }
}

void StemPlayer::applyCommands() noexcept {
  delayL_.setDelaySamples(latencyReq_);
  delayR_.setDelaySamples(latencyReq_);
  adoptIfStopped();

  if (mode_ == TransportMode::HostFollow) {
    seekCmd_ = false;
    playCmd_ = PlayCmd::None;
    applyHostFollow();
  } else {
    if (seekCmd_) {
      const std::int64_t target = std::clamp<std::int64_t>(seekCmdPos_, 0, len_);
      if (advancing_) {
        if (xfading_) {
          pendingSeek_ = true;
          pendingSeekFollow_ = false;
          pendingSeekPos_ = target;
          pendingSeekAge_ = 0;
        } else {
          doSeek(target);
        }
      } else {
        pos_ = target;  // stopped (or counting in): the playhead just jumps
      }
      seekCmd_ = false;
    }
    if (playCmd_ == PlayCmd::Play) {
      wantPlay_ = true;
      if (!advancing_ && !countingIn_) {
        if (ciBars_ > 0) startCountIn();
        else startPlaying();
      } else if (advancing_ && !tgOn_) {
        tgOn_ = true;
        tg_.begin(1.0, nTrans_);
      }
    } else if (playCmd_ == PlayCmd::Pause) {
      wantPlay_ = false;
      if (countingIn_) countingIn_ = false;
      else if (advancing_ && tgOn_) startPause();
    }
    playCmd_ = PlayCmd::None;
  }
  updateMixTargets();
}

// ---- setters ---------------------------------------------------------------------------------
void StemPlayer::play() noexcept {
  if (mode_ != TransportMode::FreeRun) return;
  playCmd_ = PlayCmd::Play;
  wantPlay_ = true;
}
void StemPlayer::pause() noexcept {
  if (mode_ != TransportMode::FreeRun) return;
  playCmd_ = PlayCmd::Pause;
  wantPlay_ = false;
}
void StemPlayer::seek(std::int64_t pos) noexcept {
  if (mode_ != TransportMode::FreeRun) return;
  seekCmd_ = true;
  seekCmdPos_ = pos;
}

bool StemPlayer::setLoop(std::int64_t a, std::int64_t b) noexcept {
  if (set_ == nullptr || a < 0 || a >= b || b > len_ || b - a < 2 * static_cast<std::int64_t>(nLoop_)) return false;
  loopActive_ = true;
  loopA_ = a;
  loopB_ = b;
  wrapPending_ = false;
  return true;
}

void StemPlayer::clearLoop() noexcept {
  loopActive_ = false;
  wrapPending_ = false;
}

void StemPlayer::setCountIn(int bars, double bpm, int beatsPerBar) noexcept {
  ciBars_ = std::clamp(bars, 0, 8);
  ciBpm_ = (bpm == bpm) ? std::clamp(bpm, 30.0, 300.0) : 120.0;
  ciBpb_ = std::clamp(beatsPerBar, 1, 12);
}

void StemPlayer::rebuildClicks() noexcept {
  const double a = dbToLin(clickLevelDb_);
  for (std::size_t n = 0; n < accent_.size(); ++n) accent_[n] = static_cast<float>(a * accentUnit_[n]);
  for (std::size_t n = 0; n < normal_.size(); ++n) normal_[n] = static_cast<float>(a * normalUnit_[n]);
}

void StemPlayer::setClickLevelDb(double db) noexcept {
  clickLevelDb_ = clampDb(db, -60.0, 0.0);
  rebuildClicks();
}

void StemPlayer::setStemGainDb(StemKind k, double db) noexcept { gainDb_[static_cast<std::size_t>(k)] = clampDb(db, -60.0, 12.0); }
void StemPlayer::setStemMute(StemKind k, bool mute) noexcept { mute_[static_cast<std::size_t>(k)] = mute; }
void StemPlayer::setStemSolo(StemKind k, bool solo) noexcept { solo_[static_cast<std::size_t>(k)] = solo; }
void StemPlayer::setMasterLevelDb(double db) noexcept { masterDb_ = clampDb(db, -60.0, 12.0); }
void StemPlayer::setGuitarMode(GuitarMode m) noexcept { guitarMode_ = m; }
void StemPlayer::setRigLatencySamples(int samples) noexcept { latencyReq_ = std::clamp(samples, 0, maxLatency_); }

// ---- audio ----------------------------------------------------------------------------------
void StemPlayer::startCrossfade(std::int64_t newPos, int n, const float* in, const float* out) noexcept {
  oldPos_ = pos_;
  pos_ = newPos;
  xfN_ = n;
  xfK_ = 0;
  xfIn_ = in;
  xfOut_ = out;
  xfading_ = true;
  wrapPending_ = false;
}

void StemPlayer::doSeek(std::int64_t target) noexcept {
  startCrossfade(target, nSeek_, seekIn_.data(), seekOut_.data());
  pendingSeek_ = false;
}

void StemPlayer::doWrap() noexcept { startCrossfade(loopA_, nLoop_, loopIn_.data(), loopOut_.data()); }

void StemPlayer::startClick(bool accent) noexcept {
  clickBuf_ = accent ? &accent_ : &normal_;
  clickIdx_ = 0;
  clickLeft_ = static_cast<int>(clickBuf_->size());
}

void StemPlayer::stepSample(float& outL, float& outR) noexcept {
  for (std::size_t k = 0; k < g_.size(); ++k) g_[k] = stemRamp_[k].next();
  const double master = masterRamp_.next();

  bool stems = advancing_;
  if (countingIn_) {
    if (ciBeat_ < ciBeats_ && ciPos_ == ciNextAt_) {
      startClick(ciBeat_ % ciBpbActive_ == 0);
      ++ciBeat_;
      ciNextAt_ = ciBeat_ < ciBeats_ ? beatStart(ciBeat_) : -1;
    }
    if (ciPos_ >= ciTotal_) {
      countingIn_ = false;
      startPlaying();
      stems = true;
    } else {
      ++ciPos_;
    }
  }
  double click = 0.0;
  if (clickLeft_ > 0) {
    click = static_cast<double>((*clickBuf_)[static_cast<std::size_t>(clickIdx_)]);
    ++clickIdx_;
    --clickLeft_;
  }

  double vl = click, vr = click;
  if (stems) {
    const double tg = tg_.next();
    const bool inNew = pos_ >= 0 && pos_ < len_;
    const bool inOld = xfading_ && oldPos_ >= 0 && oldPos_ < len_;
    const double fin = xfading_ ? static_cast<double>(xfIn_[xfK_]) : 1.0;
    const double fout = xfading_ ? static_cast<double>(xfOut_[xfK_]) : 0.0;
    double sl = 0.0, sr = 0.0;
    if (inNew || inOld) {
      for (int a = 0; a < nActive_; ++a) {
        const auto k = static_cast<std::size_t>(active_[static_cast<std::size_t>(a)]);
        const double g = g_[k];
        if (g == 0.0) continue;
        double xl = 0.0, xr = 0.0;
        if (inNew) {
          xl = fin * static_cast<double>(ch_[k * 2][pos_]);
          xr = fin * static_cast<double>(ch_[k * 2 + 1][pos_]);
        }
        if (inOld) {
          xl += fout * static_cast<double>(ch_[k * 2][oldPos_]);
          xr += fout * static_cast<double>(ch_[k * 2 + 1][oldPos_]);
        }
        sl += g * xl;
        sr += g * xr;
      }
    }
    vl += tg * master * sl;
    vr += tg * master * sr;

    // Advance the heads, then resolve crossfade end / pending seek / loop wrap for the next sample.
    if (xfading_) {
      ++xfK_;
      ++oldPos_;
      if (xfK_ >= xfN_) xfading_ = false;
    }
    ++pos_;
    if (pendingSeek_) ++pendingSeekAge_;
    if (!xfading_ && pendingSeek_) {
      doSeek(pendingSeekFollow_ ? pendingSeekPos_ + pendingSeekAge_ : pendingSeekPos_);
    } else if (mode_ == TransportMode::FreeRun && loopActive_ && pos_ == loopB_) {
      if (xfading_) wrapPending_ = true;
      else doWrap();
    } else if (!xfading_ && wrapPending_) {
      // Deferred wrap: land at A + (overshoot past B) so the loop period stays exact.
      std::int64_t over = pos_ - loopB_;
      if (over < 0) over = 0;
      over %= (loopB_ - loopA_);
      startCrossfade(loopA_ + over, nLoop_, loopIn_.data(), loopOut_.data());
    }
    if (!tgOn_ && !tg_.active()) stopTransport();
  }
  outL = static_cast<float>(vl);
  outR = static_cast<float>(vr);
}

void StemPlayer::process(float* outL, float* outR, int n) noexcept {
  applyCommands();
  int i = 0;
  while (i < n) {
    if (!advancing_ && !countingIn_ && clickLeft_ == 0) {
      const int m = n - i;  // idle: silence; only the mix ramps keep running
      std::fill(outL + i, outL + n, 0.0f);
      std::fill(outR + i, outR + n, 0.0f);
      for (auto& r : stemRamp_) r.skip(m);
      masterRamp_.skip(m);
      break;
    }
    stepSample(outL[i], outR[i]);
    ++i;
  }
  if (n > 0) {
    delayL_.process(outL, n);
    delayR_.process(outR, n);
  }
}

}  // namespace sawblade
