#include "sawblade/chain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <stdexcept>

#include "sawblade/capture_cache.h"
#include "sawblade/ir.h"
#include "sawblade/nam_block.h"

namespace sawblade {
namespace {

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

std::unique_ptr<Convolver> loadCab(const Capture& c, const std::string& path, double sr, bool normalize,
                                   std::vector<std::string>& warnings, CaptureCache* cache) {
  const std::string filePath = path + ".file";
  std::shared_ptr<const IrData> ir;
  if (cache) {
    ir = cache->ir(c, filePath, sr, normalize);
  } else {
    verifyCapture(c, filePath);
    try {
      ir = std::make_shared<const IrData>(loadIr(c.resolvedPath, sr, normalize));
    } catch (const std::exception& e) {
      throw CaptureError(filePath, e.what());
    }
  }
  for (const auto& w : ir->warnings) warnings.push_back(path + ": " + w);
  auto conv = std::make_unique<Convolver>();
  conv->setIr(ir->samples);
  return conv;
}

// Deterministic white noise: xorshift64* (not <random>: distributions are implementation-defined).
std::vector<float> makeProbe(double sr) {
  const auto n = static_cast<std::size_t>(std::llround(Chain::kProbeSeconds * sr));
  // Uniform in [-1, 1) has RMS 1/sqrt(3); scale so the RMS is kProbeLevelDbfs.
  const double amp = dbToLin(Chain::kProbeLevelDbfs) * std::sqrt(3.0);
  std::uint64_t s = Chain::kProbeSeed * 0x9E3779B97F4A7C15ull;
  std::vector<float> x(n);
  for (auto& v : x) {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    const std::uint64_t r = s * 0x2545F4914F6CDD1Dull;
    const double u = static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0;
    v = static_cast<float>(u * amp);
  }
  // Band-pass 80 Hz - 5 kHz: 12 dB/oct high-pass then low-pass (RBJ, q = 1/sqrt(2)).
  Biquad hp, lp;
  EqBand b;
  b.q = 0.7071067811865476;
  b.type = EqType::HighPass;
  b.freq = Chain::kProbeLowHz;
  hp.setCoeffs(designBiquad(b, sr));
  b.type = EqType::LowPass;
  b.freq = Chain::kProbeHighHz;
  lp.setCoeffs(designBiquad(b, sr));
  hp.process(x.data(), static_cast<int>(n));
  lp.process(x.data(), static_cast<int>(n));
  return x;
}

}  // namespace

ChainResources loadResources(const Preset& p, double sr, CaptureCache* cache) {
  ChainResources res;
  res.sampleRate = sr;
  const PathPreset* paths[2] = {&p.a, &p.b};
  const char* names[2] = {"paths.a", "paths.b"};
  for (int k = 0; k < 2; ++k) {
    const auto& blocks = paths[k]->blocks;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      const std::string jp = JsonObject::index(std::string(names[k]) + ".blocks", i);
      const BlockType* t = BlockRegistry::instance().find(blocks[i].type);
      if (!t) throw std::runtime_error(jp + ".type: unknown block type \"" + blocks[i].type + "\"");
      BlockBuildContext ctx{sr, &res.warnings, jp, cache};
      LoadedBlock lb;
      lb.id = blocks[i].id;
      lb.type = blocks[i].type;
      lb.traits = t->traits;
      lb.bypass = blocks[i].bypass;
      lb.processor = t->create(blocks[i], ctx);
      res.blocks[static_cast<std::size_t>(k)].push_back(std::move(lb));
    }
  }
  if (!p.cab.enabled) {
    // A disabled cab is never used (Chain ignores it): do not require its IR to exist.
  } else if (p.cab.mode == CabMode::Shared) {
    res.cabShared = loadCab(p.cab.ir, "cab.ir", sr, p.cab.normalize, res.warnings, cache);
  } else {
    res.cabA = loadCab(p.cab.irA, "cab.irA", sr, p.cab.normalize, res.warnings, cache);
    res.cabB = loadCab(p.cab.irB, "cab.irB", sr, p.cab.normalize, res.warnings, cache);
  }
  return res;
}

std::vector<NamRateProbe> probeNamRates(const Preset& p, CaptureCache* cache) {
  std::vector<NamRateProbe> out;
  const PathPreset* paths[2] = {&p.a, &p.b};
  const char* names[2] = {"paths.a", "paths.b"};
  for (int k = 0; k < 2; ++k) {
    if (!paths[k]->enabled) continue;
    for (std::size_t i = 0; i < paths[k]->blocks.size(); ++i) {
      const Block& b = paths[k]->blocks[i];
      const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get());
      if (!nam || b.bypass) continue;
      const std::string where = JsonObject::index(std::string(names[k]) + ".blocks", i);
      const std::string filePath = where + ".model.file";
      double hz = -1.0;
      try {
        if (cache) {
          hz = cache->namModel(nam->model, filePath)->expectedSampleRate();
        } else {
          hz = NamBlock::load(nam->model.resolvedPath, NamBlockConfig{})->expectedSampleRate();
        }
      } catch (const CaptureError&) {
        throw;
      } catch (const std::exception& e) {
        throw CaptureError(filePath, e.what());
      }
      out.push_back({where + " (" + b.id + ")", hz > 0.0 ? hz : kAssumedNamSampleRate, hz > 0.0});
    }
  }
  return out;
}

ModelRate commonModelRate(const std::vector<NamRateProbe>& probes) {
  ModelRate r;
  if (probes.empty()) return r;
  const double hz = probes.front().hz;
  for (const auto& p : probes)
    if (p.hz != hz) r.ambiguous = true;
  if (!r.ambiguous) {
    r.hz = hz;
    return r;
  }
  char buf[32];
  for (std::size_t i = 0; i < probes.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%g", probes[i].hz);
    r.listing += (i ? ", " : "") + probes[i].where + ": " + buf + " Hz" + (probes[i].recorded ? "" : " (assumed)");
  }
  return r;
}

LiveParams LiveParams::fromPreset(const Preset& p) {
  LiveParams l;
  l.inputGainDb = p.inputGainDb;
  l.outputGainDb = p.outputGainDb;
  l.gateThresholdDb = p.gate.thresholdDb;
  l.blend = p.blend;
  l.levelDbA = p.a.levelDb;
  l.levelDbB = p.b.levelDb;
  const auto fill = [](LiveEq& dst, const std::vector<EqBand>& src) {
    for (std::size_t i = 0; i < src.size() && i < dst.size(); ++i) dst[i] = {src[i].freq, src[i].gainDb, src[i].q};
  };
  fill(l.postEq, p.postEq);
  const PathPreset* pp[2] = {&p.a, &p.b};
  for (std::size_t k = 0; k < 2; ++k) {
    fill(l.preEq[k], pp[k]->preEq);
    fill(l.pathEq[k], pp[k]->eq);
    for (std::size_t i = 0; i < pp[k]->blocks.size() && i < l.blocks[k].size(); ++i)
      if (const auto* nam = dynamic_cast<const NamBlockParams*>(pp[k]->blocks[i].params.get()))
        l.blocks[k][i] = {nam->inputGainDb, nam->outputGainDb};
  }
  return l;
}

Chain::Chain(const Preset& preset, ChainResources&& resources) : preset_(preset), res_(std::move(resources)) {
  const double sr = res_.sampleRate;
  if (sr <= 0.0) throw std::runtime_error("ChainResources has no sample rate");
  const PathPreset* pp[2] = {&preset_.a, &preset_.b};
  for (std::size_t k = 0; k < 2; ++k) {
    Path& p = path_[k];
    if (res_.blocks[k].size() != pp[k]->blocks.size())
      throw std::runtime_error("ChainResources does not match the preset's blocks");
    p.blocks = std::move(res_.blocks[k]);
    p.enabled = pp[k]->enabled;
    const std::string pathName = k == 0 ? "paths.a" : "paths.b";
    try {
      p.preEq.configure(sr, pp[k]->preEq);
    } catch (const std::invalid_argument& e) {
      throw PresetError(pathName + ".preEq", e.what());
    }
    try {
      p.eq.configure(sr, pp[k]->eq);
    } catch (const std::invalid_argument& e) {
      throw PresetError(pathName + ".eq", e.what());
    }
    p.level.setGainLinear(static_cast<float>((pp[k]->invert ? -1.0 : 1.0) * dbToLin(pp[k]->levelDb)));
    for (const auto& lb : p.blocks)
      if (!lb.processor) throw std::runtime_error("ChainResources contains a null block");
  }
  try {
    postEq_.configure(sr, preset_.postEq);
  } catch (const std::invalid_argument& e) {
    throw PresetError("postEq", e.what());
  }
  if (preset_.cab.enabled) {
    if (preset_.cab.mode == CabMode::Shared) {
      if (!res_.cabShared) throw std::runtime_error("ChainResources lacks the shared cab");
      cabShared_ = std::move(res_.cabShared);
    } else {
      if (!res_.cabA || !res_.cabB) throw std::runtime_error("ChainResources lacks the per-path cabs");
      path_[0].cab = std::move(res_.cabA);
      path_[1].cab = std::move(res_.cabB);
    }
  }
  inGain_.setGainDb(preset_.inputGainDb);
  outGain_.setGainDb(preset_.outputGainDb);
  gate_.setParams(preset_.gate);
  gateOn_ = preset_.gate.enabled;
  comp_.setParams(preset_.busComp);
  compOn_ = preset_.busComp.enabled;
  blendA_ = static_cast<float>(1.0 - preset_.blend);
  blendB_ = static_cast<float>(preset_.blend);
  blendCurA_ = blendA_;
  blendCurB_ = blendB_;
  live_ = LiveParams::fromPreset(preset_);

  warnings_ = res_.warnings;
  for (std::size_t k = 0; k < 2; ++k) {
    if (!path_[k].enabled) continue;
    for (const auto& lb : path_[k].blocks)
      if (!lb.bypass && !lb.traits.namTrainable)
        warnings_.push_back("block '" + lb.id + "' (type '" + lb.type + "') is not NAM-trainable");
  }
  if (compOn_ && preset_.busComp.releaseMs > kBusCompMaxTrainableReleaseMs)
    warnings_.push_back("busComp: releaseMs " + std::to_string(preset_.busComp.releaseMs) + " > " +
                        std::to_string(static_cast<int>(kBusCompMaxTrainableReleaseMs)) +
                        " ms is not NAM-trainable");
}

Chain::~Chain() = default;

void Chain::prepare(const ProcessSpec& spec) {
  if (spec.sampleRate != res_.sampleRate)
    throw std::runtime_error("Chain::prepare sample rate " + std::to_string(spec.sampleRate) +
                             " differs from the rate the resources were built for (" +
                             std::to_string(res_.sampleRate) + ")");
  if (spec.maxBlockSize < 1) throw std::runtime_error("Chain::prepare: maxBlockSize must be >= 1");
  maxBlock_ = spec.maxBlockSize;
  rampSamples_ = std::max(1, static_cast<int>(std::llround(kLiveRampMs * 0.001 * spec.sampleRate)));
  const auto nb = static_cast<std::size_t>(maxBlock_);
  work_.assign(nb, 0.0f);
  bufA_.assign(nb, 0.0f);
  bufB_.assign(nb, 0.0f);

  inGain_.prepare(spec);
  gate_.prepare(spec);
  postEq_.prepare(spec);
  comp_.prepare(spec);
  outGain_.prepare(spec);
  if (cabShared_) cabShared_->prepare(spec);
  for (auto& p : path_) {
    p.preEq.prepare(spec);
    p.eq.prepare(spec);
    for (auto& lb : p.blocks) lb.processor->prepare(spec);
    if (p.cab) p.cab->prepare(spec);
    p.level.prepare(spec);
    p.latency = 0;
    for (const auto& lb : p.blocks)
      if (!lb.bypass) p.latency += lb.processor->latencySamples();
    if (p.cab) p.latency += p.cab->latencySamples();
  }

  const int longest = std::max(path_[0].latency, path_[1].latency);
  const AlignParams& al = preset_.align;
  const int maxLag = static_cast<int>(std::llround(al.maxLagMs * spec.sampleRate / 1000.0));
  const int alignRoom = al.mode == AlignMode::Auto ? maxLag : al.mode == AlignMode::Manual ? std::abs(al.delaySamplesB) : 0;
  for (auto& p : path_) {
    p.compDelay = longest - p.latency;
    p.delay.setMaxDelaySamples(p.compDelay + alignRoom);
    p.delay.prepare(spec);
    p.alignDelay = 0;
    p.delay.setDelaySamples(p.compDelay);
  }

  prepared_ = true;
  alignWarnings_.clear();
  if (al.mode == AlignMode::Auto) {
    applyAlignment(resolveAlignment());
  } else if (al.mode == AlignMode::Manual) {
    applyAlignment({al.delaySamplesB, al.invertB, 0.0});
  } else {
    applyAlignment({});
  }
}

void Chain::applyAlignment(const AlignResult& r) {
  align_ = r;
  path_[0].alignDelay = std::max(0, -r.delaySamplesB);
  path_[1].alignDelay = std::max(0, r.delaySamplesB);
  for (auto& p : path_) p.delay.setDelaySamples(p.compDelay + p.alignDelay);
  blendB_ = static_cast<float>(live_.blend) * (r.invertB ? -1.0f : 1.0f);
  blendCurB_ = blendB_;
  blendRamp_ = 0;
  // Processing latency only; the alignment delay is part of the tone and is reported separately.
  const int total = std::max(path_[0].latency, path_[1].latency);
  latency_ = total + (cabShared_ ? cabShared_->latencySamples() : 0);
}

void Chain::resetAll() {
  inGain_.reset();
  gate_.reset();
  postEq_.reset();
  comp_.reset();
  outGain_.reset();
  if (cabShared_) cabShared_->reset();
  for (auto& p : path_) {
    p.preEq.reset();
    p.eq.reset();
    for (auto& lb : p.blocks) lb.processor->reset();
    if (p.cab) p.cab->reset();
    p.level.reset();
    p.delay.reset();
  }
}

void Chain::reset() {
  if (!prepared_) return;  // nothing to clear; blocks (e.g. NAM) are not reset-safe before prepare()
  resetAll();
}

AlignResult Chain::resolveAlignment() {
  if (!prepared_) throw std::logic_error("Chain::resolveAlignment requires prepare()");
  alignWarnings_.clear();
  AlignResult result;
  if (!path_[0].enabled || !path_[1].enabled) {
    alignWarnings_.push_back("alignment skipped: a path is disabled");
    return result;
  }
  const double sr = res_.sampleRate;
  // The scan needs maxLag < probe length (the schema caps maxLagMs at 50, far below the 1 s probe);
  // clamp defensively so corr() can never index out of range.
  const int maxLag = std::min(static_cast<int>(std::llround(preset_.align.maxLagMs * sr / 1000.0)),
                              static_cast<int>(std::llround(kProbeSeconds * sr)) - 1);

  // Measure with latency compensation only (no alignment delay), gate bypassed, at the blend point.
  const int savedDelay[2] = {path_[0].compDelay + path_[0].alignDelay, path_[1].compDelay + path_[1].alignDelay};
  resetAll();
  for (auto& p : path_) p.delay.setDelaySamples(p.compDelay);

  std::vector<float> probe = makeProbe(sr);
  inGain_.process(probe.data(), static_cast<int>(probe.size()));
  const std::size_t n = probe.size();
  std::vector<double> a(n), b(n);
  for (std::size_t pos = 0; pos < n; pos += static_cast<std::size_t>(maxBlock_)) {
    const int len = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(maxBlock_), n - pos));
    std::copy_n(probe.data() + pos, len, bufA_.data());
    std::copy_n(probe.data() + pos, len, bufB_.data());
    renderPath(path_[0], bufA_.data(), len, 0, false);
    renderPath(path_[1], bufB_.data(), len, 0, false);
    for (int i = 0; i < len; ++i) {
      a[pos + static_cast<std::size_t>(i)] = bufA_[static_cast<std::size_t>(i)];
      b[pos + static_cast<std::size_t>(i)] = bufB_[static_cast<std::size_t>(i)];
    }
  }

  double ea = 0.0, eb = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    ea += a[i] * a[i];
    eb += b[i] * b[i];
  }
  if (ea <= 1e-20 || eb <= 1e-20) {
    alignWarnings_.push_back("alignment skipped: a path produced silence for the probe");
  } else {
    // r(k) = sum_n a[n] * b[n + k]: a positive peak at k means B lags A by k samples, so A is
    // delayed (delaySamplesB = -k). Lags are scanned 0, +1, -1, +2, ... and only a strictly
    // larger |r| replaces the best, so ties resolve to the smallest |lag|, deterministically.
    auto corr = [&](int k) {
      const std::size_t lo = k < 0 ? static_cast<std::size_t>(-k) : 0;
      const std::size_t hi = k > 0 ? n - static_cast<std::size_t>(k) : n;
      double s = 0.0;
      for (std::size_t i = lo; i < hi; ++i) s += a[i] * b[static_cast<std::size_t>(static_cast<long long>(i) + k)];
      return s;
    };
    double best = -1.0, bestR = 0.0;
    int bestK = 0;
    for (int m = 0; m <= 2 * maxLag; ++m) {
      const int k = (m & 1) ? -((m + 1) / 2) : m / 2;
      if (k == 0 && m != 0) continue;
      const double r = corr(k);
      if (std::fabs(r) > best) {
        best = std::fabs(r);
        bestR = r;
        bestK = k;
      }
    }
    result.delaySamplesB = -bestK;
    result.invertB = bestR < 0.0;
    result.peakCorrelation = best / std::sqrt(ea * eb);
  }

  resetAll();
  path_[0].delay.setDelaySamples(savedDelay[0]);
  path_[1].delay.setDelaySamples(savedDelay[1]);
  return result;
}

ChainInfo Chain::info() const {
  ChainInfo i;
  for (std::size_t k = 0; k < 2; ++k) {
    i.pathLatency[k] = path_[k].latency;
    i.compensationDelay[k] = path_[k].compDelay;
    i.alignDelay[k] = path_[k].alignDelay;
  }
  i.latencySamples = latency_;
  i.alignMode = preset_.align.mode;
  i.align = align_;
  i.liveCompatible = preset_.cab.mode == CabMode::Shared;
  i.exportExactness = {true, i.liveCompatible};
  i.warnings = warnings_;
  i.warnings.insert(i.warnings.end(), alignWarnings_.begin(), alignWarnings_.end());
  return i;
}

void Chain::renderPath(Path& p, float* io, int n, std::uint64_t counter, bool allowRamp) noexcept {
  if (!p.enabled) {
    std::fill(io, io + n, 0.0f);
    return;
  }
  const auto k = static_cast<std::size_t>(&p - path_.data());
  if (allowRamp) processEq(p.preEq, preRamps_[k], io, n, counter);
  else p.preEq.process(io, n);
  for (auto& lb : p.blocks)
    if (!lb.bypass) lb.processor->process(io, n);
  if (allowRamp) processEq(p.eq, pathRamps_[k], io, n, counter);
  else p.eq.process(io, n);
  p.level.process(io, n);
  if (p.cab) p.cab->process(io, n);
  p.delay.process(io, n);
}

namespace {

void advanceRamp(double& cur, double target, double step, int& remaining) noexcept {
  if (remaining <= 0) return;
  if (remaining <= Chain::kEqSubBlock) {
    cur = target;
    remaining = 0;
  } else {
    cur += step * Chain::kEqSubBlock;
    remaining -= Chain::kEqSubBlock;
  }
}

}  // namespace

void Chain::applyLiveEq(EqRamps& rs, const std::vector<EqBand>& cfg, const LiveEq& oldL,
                        LiveEq& newL) noexcept {
  const double fs = res_.sampleRate;
  const auto n = std::min(cfg.size(), newL.size());
  for (std::size_t i = 0; i < n; ++i) {
    const LiveEqBand& o = oldL[i];
    LiveEqBand& nw = newL[i];
    // Reject invalid values (keep the old ones); a band without a freq (0) is not live.
    if (!(nw.freq >= 10.0 && nw.freq < 0.49 * fs)) nw.freq = o.freq;
    if (!std::isfinite(nw.gainDb)) nw.gainDb = o.gainDb;
    if (!(nw.q >= 0.05 && nw.q <= 36.0)) nw.q = o.q;
    const EqBand& c = cfg[i];
    if (!c.enabled || o.freq <= 0.0 || nw.freq <= 0.0) continue;
    const bool pass = c.type == EqType::HighPass || c.type == EqType::LowPass;
    const bool gCh = !pass && nw.gainDb != o.gainDb;
    const bool fCh = nw.freq != o.freq;
    const bool qCh = nw.q != o.q;
    if (!gCh && !fCh && !qCh) continue;
    BandRamp& b = rs.band[i];
    if (!b.active()) {
      b.gNow = o.gainDb;
      b.fNow = o.freq;
      b.qNow = o.q;
      b.shape = false;
      ++rs.active;
    }
    const auto start = [this](Ramp1& r, double from, double to) {
      if (r.remaining <= 0) r.cur = from;
      r.target = to;
      r.step = (to - r.cur) / static_cast<double>(rampSamples_);
      r.remaining = rampSamples_;
    };
    if (gCh) start(b.gain, o.gainDb, nw.gainDb);
    if (fCh) {
      start(b.logF, std::log2(o.freq), std::log2(nw.freq));
      b.shape = true;
    }
    if (qCh) {
      start(b.logQ, std::log(o.q), std::log(nw.q));
      b.shape = true;
    }
    // Linear targets, so a finished ramp lands exactly on the requested value.
    if (fCh) b.fTarget = nw.freq;
    if (qCh) b.qTarget = nw.q;
  }
}

void Chain::setLiveParams(const LiveParams& in) noexcept {
  if (!prepared_) return;
  LiveParams p = in;
  const auto lin = [](double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); };
  if (p.inputGainDb != live_.inputGainDb) inGain_.rampToLinear(lin(p.inputGainDb), rampSamples_);
  if (p.outputGainDb != live_.outputGainDb) outGain_.rampToLinear(lin(p.outputGainDb), rampSamples_);
  const double lv[2] = {p.levelDbA, p.levelDbB};
  const double old[2] = {live_.levelDbA, live_.levelDbB};
  const bool mute[2] = {p.muteA, p.muteB};
  const bool oldMute[2] = {live_.muteA, live_.muteB};
  const PathPreset* pp[2] = {&preset_.a, &preset_.b};
  for (std::size_t k = 0; k < 2; ++k) {
    if (lv[k] == old[k] && mute[k] == oldMute[k]) continue;
    if (mute[k] && mute[k] == oldMute[k]) continue;  // a level change while muted only moves the stored target
    path_[k].level.rampToLinear(mute[k] ? 0.0f : (pp[k]->invert ? -1.0f : 1.0f) * lin(lv[k]), rampSamples_);
  }
  if (p.gateThresholdDb != live_.gateThresholdDb && std::isfinite(p.gateThresholdDb)) {
    GateParams g = gate_.params();
    g.thresholdDb = p.gateThresholdDb;
    gate_.setParams(g);
  }
  if (p.blend != live_.blend) {
    if (blendRamp_ <= 0) {  // otherwise continue from the current (mid-ramp) value
      blendCurA_ = blendA_;
      blendCurB_ = blendB_;
    }
    blendA_ = static_cast<float>(1.0 - p.blend);
    blendB_ = static_cast<float>(p.blend) * (align_.invertB ? -1.0f : 1.0f);
    blendStepA_ = (blendA_ - blendCurA_) / static_cast<float>(rampSamples_);
    blendStepB_ = (blendB_ - blendCurB_) / static_cast<float>(rampSamples_);
    blendRamp_ = rampSamples_;
  }
  applyLiveEq(postRamps_, preset_.postEq, live_.postEq, p.postEq);
  for (std::size_t k = 0; k < 2; ++k) {
    applyLiveEq(preRamps_[k], pp[k]->preEq, live_.preEq[k], p.preEq[k]);
    applyLiveEq(pathRamps_[k], pp[k]->eq, live_.pathEq[k], p.pathEq[k]);
    auto& blocks = path_[k].blocks;
    for (std::size_t i = 0; i < blocks.size() && i < p.blocks[k].size(); ++i) {
      LiveBlock& nb = p.blocks[k][i];
      const LiveBlock& ob = live_.blocks[k][i];
      if (!std::isfinite(nb.inputGainDb)) nb.inputGainDb = ob.inputGainDb;
      if (!std::isfinite(nb.outputGainDb)) nb.outputGainDb = ob.outputGainDb;
      if (nb != ob) blocks[i].processor->setLiveGainsDb(nb.inputGainDb, nb.outputGainDb, rampSamples_);
    }
  }
  live_ = p;
}

void Chain::processEq(ParametricEq& eq, EqRamps& rs, float* w, int n, std::uint64_t counter) noexcept {
  if (rs.active == 0) {
    eq.process(w, n);
    return;
  }
  // The redesign grid is absolute (multiples of kEqSubBlock of the running sample counter), so the
  // result does not depend on how the host splits the stream into blocks.
  int pos = 0;
  while (pos < n) {
    const int phase = static_cast<int>(counter % static_cast<std::uint64_t>(kEqSubBlock));
    if (phase == 0 && rs.active > 0) {
      for (std::size_t i = 0; i < rs.band.size(); ++i) {
        BandRamp& b = rs.band[i];
        if (!b.active()) continue;
        if (b.gain.remaining > 0) {
          advanceRamp(b.gain.cur, b.gain.target, b.gain.step, b.gain.remaining);
          b.gNow = b.gain.cur;
        }
        if (b.logF.remaining > 0) {
          advanceRamp(b.logF.cur, b.logF.target, b.logF.step, b.logF.remaining);
          b.fNow = b.logF.remaining == 0 ? b.fTarget : std::exp2(b.logF.cur);
        }
        if (b.logQ.remaining > 0) {
          advanceRamp(b.logQ.cur, b.logQ.target, b.logQ.step, b.logQ.remaining);
          b.qNow = b.logQ.remaining == 0 ? b.qTarget : std::exp(b.logQ.cur);
        }
        if (b.shape) eq.setBand(static_cast<int>(i), b.fNow, b.gNow, b.qNow);
        else eq.setBandGainDb(static_cast<int>(i), b.gNow);
        if (!b.active()) --rs.active;
      }
    }
    const int len = std::min(n - pos, kEqSubBlock - phase);
    eq.process(w + pos, len);
    counter += static_cast<std::uint64_t>(len);
    pos += len;
  }
}

void Chain::process(const float* in, float* out, int n) noexcept {
  if (!prepared_) {
    if (in != out) std::copy(in, in + n, out);
    return;
  }
  while (n > 0) {
    const int len = std::min(n, maxBlock_);
    processChunk(in, out, len);
    in += len;
    out += len;
    n -= len;
  }
}

void Chain::processChunk(const float* in, float* out, int n) noexcept {
  float* w = work_.data();
  float* a = bufA_.data();
  float* b = bufB_.data();
  std::copy(in, in + n, w);
  inGain_.process(w, n);
  if (gateOn_) gate_.processKeyed(w, w, n);
  std::copy(w, w + n, a);
  std::copy(w, w + n, b);
  renderPath(path_[0], a, n, eqCounter_, true);
  renderPath(path_[1], b, n, eqCounter_, true);
  int i = 0;
  if (blendRamp_ > 0) {
    const int r = std::min(blendRamp_, n);
    float ca = blendCurA_, cb = blendCurB_;
    for (; i < r; ++i) {
      ca += blendStepA_;
      cb += blendStepB_;
      w[i] = ca * a[i] + cb * b[i];
    }
    blendRamp_ -= r;
    if (blendRamp_ == 0) {
      ca = blendA_;
      cb = blendB_;
    }
    blendCurA_ = ca;
    blendCurB_ = cb;
  }
  const float ga = blendA_, gb = blendB_;
  for (; i < n; ++i) w[i] = ga * a[i] + gb * b[i];
  if (cabShared_) cabShared_->process(w, n);
  processEq(postEq_, postRamps_, w, n, eqCounter_);
  eqCounter_ += static_cast<std::uint64_t>(n);
  if (compOn_) comp_.process(w, n);
  outGain_.process(w, n);
  std::copy(w, w + n, out);
}

}  // namespace sawblade
