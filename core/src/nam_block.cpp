#include "sawblade/nam_block.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

namespace sawblade {
namespace {

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

std::string metaString(const nlohmann::json& m, const char* key) {
  if (!m.is_object()) return {};
  auto it = m.find(key);
  return (it != m.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

}  // namespace

NamBlock::~NamBlock() = default;

std::unique_ptr<NamBlock> NamBlock::load(const std::filesystem::path& path, const NamBlockConfig& cfg) {
  std::unique_ptr<NamBlock> b(new NamBlock());
  try {
    nam::dspData data;
    b->dsp_ = nam::get_dsp(path, data);
    if (!b->dsp_) throw std::runtime_error("model could not be created");
    if (b->dsp_->NumInputChannels() != 1 || b->dsp_->NumOutputChannels() != 1)
      throw std::runtime_error("only mono (1-in/1-out) models are supported");
    b->expectedRate_ = b->dsp_->GetExpectedSampleRate();
    if (b->dsp_->HasLoudness()) b->loudness_ = b->dsp_->GetLoudness();
    b->meta_.name = metaString(data.metadata, "name");
    b->meta_.gearType = metaString(data.metadata, "gear_type");
    b->meta_.modeledBy = metaString(data.metadata, "modeled_by");
  } catch (const std::exception& e) {
    throw std::runtime_error("NAM load error (" + path.string() + "): " + e.what());
  }
  b->cfg_ = cfg;
  b->updateGains();
  return b;
}

void NamBlock::updateGains() noexcept {
  inGain_ = static_cast<float>(dbToLin(cfg_.inputGainDb));
  double outDb = cfg_.outputGainDb;
  if (cfg_.normalizeLoudness && loudness_) outDb += -18.0 - *loudness_;
  outGain_ = static_cast<float>(dbToLin(outDb));
}

void NamBlock::prepare(const ProcessSpec& spec) {
  if (expectedRate_ > 0.0 && expectedRate_ != spec.sampleRate && !dsp_->SupportsArbitrarySampleRate())
    throw std::runtime_error("NAM model expects sample rate " + std::to_string(expectedRate_) + " Hz but got " +
                             std::to_string(spec.sampleRate) + " Hz");
  dsp_->ResetAndPrewarm(spec.sampleRate, spec.maxBlockSize);
  sampleRate_ = spec.sampleRate;
  maxBlock_ = spec.maxBlockSize;
  scratchIn_.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
  scratchOut_.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
}

void NamBlock::reset() {
  if (maxBlock_ <= 0) return;
  dsp_->ResetAndPrewarm(sampleRate_, maxBlock_);
}

void NamBlock::process(float* io, int numSamples) noexcept {
  if (cfg_.bypass || maxBlock_ <= 0) return;
  int done = 0;
  while (done < numSamples) {  // n <= maxBlockSize by contract; chunking is a safety net
    const int n = std::min(numSamples - done, maxBlock_);
    float* in = scratchIn_.data();
    float* out = scratchOut_.data();
    for (int i = 0; i < n; ++i) in[i] = io[done + i] * inGain_;
    dsp_->process(&in, &out, n);
    for (int i = 0; i < n; ++i) io[done + i] = out[i] * outGain_;
    done += n;
  }
}

}  // namespace sawblade
