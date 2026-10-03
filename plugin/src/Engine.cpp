#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "sawblade/nam_block.h"

namespace sawblade::plugin {
namespace {

struct NamRate {
  std::string where;
  double hz;
};

// Training rates of the NAM blocks that will actually run (not bypassed, on an enabled path).
// Models without a recorded rate are rate-agnostic and do not take part. Same rule as
// tonerender's `--render-rate auto`.
std::vector<NamRate> probeNamRates(const Preset& p, const ChainResources& res) {
  std::vector<NamRate> out;
  const bool enabled[2] = {p.a.enabled, p.b.enabled};
  const char* names[2] = {"paths.a", "paths.b"};
  for (std::size_t k = 0; k < 2; ++k) {
    if (!enabled[k]) continue;
    for (std::size_t i = 0; i < res.blocks[k].size(); ++i) {
      const LoadedBlock& lb = res.blocks[k][i];
      if (lb.bypass) continue;
      const auto* nam = dynamic_cast<const NamBlock*>(lb.processor.get());
      if (nam && nam->expectedSampleRate() > 0.0)
        out.push_back({std::string(names[k]) + ".blocks[" + std::to_string(i) + "] (" + lb.id + ")", nam->expectedSampleRate()});
    }
  }
  return out;
}

std::string hz(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

}  // namespace

Engine::~Engine() = default;

std::unique_ptr<Engine> Engine::build(const Preset& preset, double hostRate, int maxBlock) {
  if (!(hostRate >= 8000.0 && hostRate <= 768000.0)) throw std::runtime_error("unsupported host sample rate " + hz(hostRate));
  std::unique_ptr<Engine> e(new Engine());
  e->hostRate_ = hostRate;
  e->hostMax_ = std::max(1, maxBlock);
  e->name_ = preset.name;

  // Models first at the host rate (the common case: the models' rate), again at their rate if it
  // differs: IRs are resampled to the rate the chain runs at.
  ChainResources res = loadResources(preset, hostRate);
  double modelRate = hostRate;
  const auto rates = probeNamRates(preset, res);
  if (!rates.empty()) {
    modelRate = rates.front().hz;
    for (const auto& r : rates)
      if (r.hz != modelRate) {
        std::string msg = "NAM blocks expect different sample rates (";
        for (std::size_t i = 0; i < rates.size(); ++i) msg += (i ? ", " : "") + rates[i].where + ": " + hz(rates[i].hz) + " Hz";
        throw std::runtime_error(msg + ")");
      }
  }
  e->resampling_ = std::fabs(modelRate - hostRate) > 1e-6;
  if (e->resampling_) res = loadResources(preset, modelRate);
  e->modelRate_ = modelRate;

  const Preset clamped = clampedToParams(preset);  // engine baseline == parameter values
  e->baseline_ = LiveParams::fromPreset(clamped);
  e->slotBand_ = postEqSlotBands(clamped);

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

void Engine::setParams(const ParamValues& v) noexcept {
  LiveParams l = baseline_;
  l.inputGainDb = v[kInputGain];
  l.outputGainDb = v[kOutputGain];
  l.gateThresholdDb = v[kGateThreshold];
  l.blend = v[kBlend];
  l.levelDbA = v[kLevelA];
  l.levelDbB = v[kLevelB];
  for (std::size_t k = 0; k < slotBand_.size(); ++k)
    if (slotBand_[k] >= 0) l.postEqGainDb[static_cast<std::size_t>(slotBand_[k])] = v[static_cast<std::size_t>(kPostEqFirst) + k];
  chain_->setLiveParams(l);
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
