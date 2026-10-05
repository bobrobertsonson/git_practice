#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "sawblade/capture_cache.h"

namespace sawblade::plugin {
namespace {

std::string hz(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

}  // namespace

Engine::~Engine() = default;

std::unique_ptr<Engine> Engine::build(const Preset& preset, double hostRate, int maxBlock, CaptureCache* sharedCache) {
  if (!(hostRate >= 8000.0 && hostRate <= 768000.0)) throw std::runtime_error("unsupported host sample rate " + hz(hostRate));
  std::unique_ptr<Engine> e(new Engine());
  e->hostRate_ = hostRate;
  e->hostMax_ = std::max(1, maxBlock);
  e->name_ = preset.name;

  // The models' rate (shared rule with tonerender: recorded rate, else 48 kHz; blocks must agree;
  // none: the host rate). The cache means each file is read once even though the rate is probed
  // before the resources are built.
  CaptureCache localCache;
  CaptureCache& cache = sharedCache ? *sharedCache : localCache;
  double modelRate = hostRate;
  try {
    const ModelRate mr = commonModelRate(probeNamRates(preset, &cache));
    if (mr.ambiguous) throw std::runtime_error("NAM blocks expect different sample rates (" + mr.listing + ")");
    if (mr.hz) modelRate = *mr.hz;
  } catch (const CaptureError& ce) {
    throw std::runtime_error(ce.what());  // the message already names the file
  }
  e->resampling_ = std::fabs(modelRate - hostRate) > 1e-6;
  ChainResources res = loadResources(preset, modelRate, &cache);
  e->modelRate_ = modelRate;

  const Preset clamped = clampedToParams(preset);  // engine baseline == parameter values
  e->baseline_ = LiveParams::fromPreset(clamped);
  e->slotBand_ = postEqSlotBands(clamped);
  e->circuit_ = findCircuitBlock(clamped);
  if (e->circuit_) {
    const PathPreset& path = e->circuit_->path == 0 ? clamped.a : clamped.b;
    e->circuitLiveCount_ = blockLiveValues(path.blocks[static_cast<std::size_t>(e->circuit_->block)], e->circuitLive_.data());
  }

  if (!e->resampling_) {
    e->chain_ = std::make_unique<Chain>(clamped, std::move(res));
    e->chain_->prepare({modelRate, e->hostMax_});
    e->latency_.chainModelSamples = e->chain_->latencySamples();
    e->latency_.total = e->latency_.chainModelSamples;
    e->latency_.resamplerHostSamples = 0;
  } else {
    const RtResampler::Ratio r1 = RtResampler::ratioFor(hostRate, modelRate);  // model/host
    const RtResampler::Ratio r2{r1.M, r1.L};                                    // host/model
    e->down_.prepare(hostRate, modelRate, r1, e->hostMax_);
    const int modelMax = e->down_.maxOutputFor(e->hostMax_);
    e->chain_ = std::make_unique<Chain>(clamped, std::move(res));
    e->chain_->prepare({modelRate, modelMax});
    const int C = e->chain_->latencySamples();
    e->up_.prepare(modelRate, hostRate, r2, modelMax);
    // Total delay in host samples: H1 + (C + H2 + s2/L2) * L2/M2. Choose s2 so it is an integer.
    e->up_.setPhaseOffset(e->up_.phaseOffsetForIntegerDelay(C));
    const std::int64_t num = (static_cast<std::int64_t>(C) + e->up_.wholeDelay()) * r2.L + e->up_.phaseOffset();
    if (num % r2.M != 0) throw std::logic_error("Engine: latency alignment failed");
    const auto total = static_cast<std::int64_t>(e->down_.wholeDelay()) + num / r2.M;
    e->latency_.chainModelSamples = C;
    e->latency_.total = static_cast<int>(total);
    e->latency_.resamplerHostSamples = static_cast<int>(total - std::llround(static_cast<double>(C) * hostRate / modelRate));
    e->latency_.resampling = true;
    e->mod_.assign(static_cast<std::size_t>(modelMax), 0.0f);
    e->tmp_.assign(static_cast<std::size_t>(e->up_.maxOutputFor(modelMax)), 0.0f);
    e->fifo_.assign(static_cast<std::size_t>(e->hostMax_) + e->tmp_.size() + 8, 0.0f);
  }
  e->info_ = e->chain_->info();
  return e;
}

void Engine::setParams(const ParamValues& v, const LiveParams* extras, const LiveParams* mutesFrom) noexcept {
  LiveParams l = extras ? *extras : baseline_;
  if (!extras && mutesFrom) {
    l.muteA = mutesFrom->muteA;
    l.muteB = mutesFrom->muteB;
  }
  l.inputGainDb = v[kInputGain];
  l.outputGainDb = v[kOutputGain];
  l.gateThresholdDb = v[kGateThreshold];
  l.blend = v[kBlend];
  l.levelDbA = v[kLevelA];
  l.levelDbB = v[kLevelB];
  for (std::size_t k = 0; k < slotBand_.size(); ++k)
    if (slotBand_[k] >= 0) l.postEq[static_cast<std::size_t>(slotBand_[k])].gainDb = v[static_cast<std::size_t>(kPostEqFirst) + k];
  chain_->setLiveParams(l);
  if (circuit_ && circuitLiveCount_ > 0) {
    const int n = circuitLiveValues(circuit_->circuit, v, circuitScratch_.data());
    if (n == circuitLiveCount_ && std::memcmp(circuitScratch_.data(), circuitLive_.data(), static_cast<std::size_t>(n) * sizeof(float)) != 0) {
      circuitLive_ = circuitScratch_;
      chain_->setBlockLiveParams(circuit_->path, circuit_->block, circuitLive_.data(), n);
    }
  }
}

void Engine::process(const float* in, float* out, int n) noexcept {
  if (!resampling_) {
    chain_->process(in, out, n);
    return;
  }
  while (n > 0) {
    const int len = std::min(n, hostMax_);
    const int m = down_.process(in, len, mod_.data());
    chain_->process(mod_.data(), mod_.data(), m);
    const int k = up_.process(mod_.data(), m, tmp_.data());
    std::memcpy(fifo_.data() + fifoCount_, tmp_.data(), static_cast<std::size_t>(k) * sizeof(float));
    fifoCount_ += k;
    const int take = std::min(len, fifoCount_);
    std::memcpy(out, fifo_.data(), static_cast<std::size_t>(take) * sizeof(float));
    if (take < len) {
      std::memset(out + take, 0, static_cast<std::size_t>(len - take) * sizeof(float));
      underruns_ += static_cast<std::uint64_t>(len - take);
    }
    fifoCount_ -= take;
    std::memmove(fifo_.data(), fifo_.data() + take, static_cast<std::size_t>(fifoCount_) * sizeof(float));
    in += len;
    out += len;
    n -= len;
  }
}

}  // namespace sawblade::plugin
