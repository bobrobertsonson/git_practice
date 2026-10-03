#include "sawblade/chain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "sawblade/ir.h"

namespace sawblade {
namespace {

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

std::unique_ptr<Convolver> loadCab(const Capture& c, const std::string& path, double sr, bool normalize,
                                   std::vector<std::string>& warnings) {
  verifyCapture(c, path + ".file");
  IrData ir = loadIr(c.resolvedPath, sr, normalize);
  for (const auto& w : ir.warnings) warnings.push_back(path + ": " + w);
  auto conv = std::make_unique<Convolver>();
  conv->setIr(ir.samples);
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

ChainResources loadResources(const Preset& p, double sr) {
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
      BlockBuildContext ctx{sr, &res.warnings, jp};
      LoadedBlock lb;
      lb.id = blocks[i].id;
      lb.type = blocks[i].type;
      lb.traits = t->traits;
      lb.bypass = blocks[i].bypass;
      lb.processor = t->create(blocks[i], ctx);
      res.blocks[static_cast<std::size_t>(k)].push_back(std::move(lb));
    }
  }
  if (p.cab.mode == CabMode::Shared) {
    res.cabShared = loadCab(p.cab.ir, "cab.ir", sr, p.cab.normalize, res.warnings);
  } else {
    res.cabA = loadCab(p.cab.irA, "cab.irA", sr, p.cab.normalize, res.warnings);
    res.cabB = loadCab(p.cab.irB, "cab.irB", sr, p.cab.normalize, res.warnings);
  }
  return res;
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
    try {
      p.preEq.configure(sr, pp[k]->preEq);
      p.eq.configure(sr, pp[k]->eq);
    } catch (const std::invalid_argument& e) {
      throw std::runtime_error(std::string(k == 0 ? "paths.a" : "paths.b") + ": " + e.what());
    }
    p.level.setGainLinear(static_cast<float>((pp[k]->invert ? -1.0 : 1.0) * dbToLin(pp[k]->levelDb)));
    for (const auto& lb : p.blocks)
      if (!lb.processor) throw std::runtime_error("ChainResources contains a null block");
  }
  try {
    postEq_.configure(sr, preset_.postEq);
  } catch (const std::invalid_argument& e) {
    throw std::runtime_error(std::string("postEq: ") + e.what());
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
  blendB_ = static_cast<float>(preset_.blend) * (r.invertB ? -1.0f : 1.0f);
  const int total = std::max(path_[0].latency + path_[0].compDelay + path_[0].alignDelay,
                             path_[1].latency + path_[1].compDelay + path_[1].alignDelay);
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

void Chain::reset() { resetAll(); }

AlignResult Chain::resolveAlignment() {
  if (!prepared_) throw std::logic_error("Chain::resolveAlignment requires prepare()");
  alignWarnings_.clear();
  AlignResult result;
  if (!path_[0].enabled || !path_[1].enabled) {
    alignWarnings_.push_back("alignment skipped: a path is disabled");
    return result;
  }
  const double sr = res_.sampleRate;
  const int maxLag = static_cast<int>(std::llround(preset_.align.maxLagMs * sr / 1000.0));

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
    renderPath(path_[0], bufA_.data(), len);
    renderPath(path_[1], bufB_.data(), len);
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

void Chain::renderPath(Path& p, float* io, int n) noexcept {
  if (!p.enabled) {
    std::fill(io, io + n, 0.0f);
    return;
  }
  p.preEq.process(io, n);
  for (auto& lb : p.blocks)
    if (!lb.bypass) lb.processor->process(io, n);
  p.eq.process(io, n);
  p.level.process(io, n);
  if (p.cab) p.cab->process(io, n);
  p.delay.process(io, n);
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
  renderPath(path_[0], a, n);
  renderPath(path_[1], b, n);
  const float ga = blendA_, gb = blendB_;
  for (int i = 0; i < n; ++i) w[i] = ga * a[i] + gb * b[i];
  if (cabShared_) cabShared_->process(w, n);
  postEq_.process(w, n);
  if (compOn_) comp_.process(w, n);
  outGain_.process(w, n);
  std::copy(w, w + n, out);
}

}  // namespace sawblade
