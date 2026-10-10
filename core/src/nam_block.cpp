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

struct NamModel::Data {
  nam::dspData data;
};

NamModel::NamModel() = default;
NamModel::~NamModel() = default;

namespace {

struct Facts {
  double expectedRate = -1.0;
  std::optional<double> loudness;
  NamMetadata meta;
};

// Shared by both load paths: validates the DSP and reads its facts.
Facts inspect(const nam::DSP* dsp, const nam::dspData& data) {
  if (!dsp) throw std::runtime_error("model could not be created");
  if (dsp->NumInputChannels() != 1 || dsp->NumOutputChannels() != 1)
    throw std::runtime_error("only mono (1-in/1-out) models are supported");
  Facts f;
  f.expectedRate = dsp->GetExpectedSampleRate();
  if (dsp->HasLoudness()) f.loudness = dsp->GetLoudness();
  f.meta.name = metaString(data.metadata, "name");
  f.meta.gearType = metaString(data.metadata, "gear_type");
  f.meta.modeledBy = metaString(data.metadata, "modeled_by");
  return f;
}

}  // namespace

std::shared_ptr<const NamModel> NamModel::load(const std::filesystem::path& path) {
  std::shared_ptr<NamModel> m(new NamModel());
  try {
    m->data_ = std::make_unique<Data>();
    auto dsp = nam::get_dsp(path, m->data_->data);
    const Facts f = inspect(dsp.get(), m->data_->data);
    m->expectedRate_ = f.expectedRate;
    m->loudness_ = f.loudness;
    m->meta_ = f.meta;
  } catch (const std::exception& e) {
    throw std::runtime_error("NAM load error (" + path.string() + "): " + e.what());
  }
  return m;
}

std::unique_ptr<NamBlock> NamBlock::load(const NamModel& model, const NamBlockConfig& cfg) {
  std::unique_ptr<NamBlock> b(new NamBlock());
  nam::dspData copy = model.data_->data;  // get_dsp may consume its argument
  b->dsp_ = nam::get_dsp(copy);
  b->expectedRate_ = model.expectedRate_;
  b->loudness_ = model.loudness_;
  b->meta_ = model.meta_;
  if (!b->dsp_) throw std::runtime_error("NAM model could not be instantiated");
  b->cfg_ = cfg;
  b->updateGains();
  return b;
}

std::unique_ptr<NamBlock> NamBlock::load(const std::filesystem::path& path, const NamBlockConfig& cfg) {
  std::unique_ptr<NamBlock> b(new NamBlock());
  try {
    nam::dspData data;
    b->dsp_ = nam::get_dsp(path, data);
    const Facts f = inspect(b->dsp_.get(), data);
    b->expectedRate_ = f.expectedRate;
    b->loudness_ = f.loudness;
    b->meta_ = f.meta;
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

bool NamBlock::setLiveGainsDb(double inDb, double outDb, int rampSamples) noexcept {
  if (!std::isfinite(inDb) || !std::isfinite(outDb)) return true;
  double outTotal = outDb;
  if (cfg_.normalizeLoudness && loudness_) outTotal += -18.0 - *loudness_;
  const auto start = [rampSamples](float& gain, GainRamp& r, float target) {
    if (target == gain && r.remaining <= 0) return;
    if (rampSamples <= 0) {
      gain = target;
      r.remaining = 0;
      return;
    }
    if (r.remaining <= 0) r.cur = gain;
    gain = target;
    r.step = (target - r.cur) / static_cast<float>(rampSamples);
    r.remaining = rampSamples;
  };
  start(inGain_, inRamp_, static_cast<float>(dbToLin(inDb)));
  start(outGain_, outRamp_, static_cast<float>(dbToLin(outTotal)));
  return true;
}

void NamBlock::prepare(const ProcessSpec& spec) {
  if (expectedRate_ > 0.0 && expectedRate_ != spec.sampleRate && !dsp_->SupportsArbitrarySampleRate())
    throw std::runtime_error("NAM model expects sample rate " + std::to_string(expectedRate_) + " Hz but got " +
                             std::to_string(spec.sampleRate) + " Hz");
  dsp_->ResetAndPrewarm(spec.sampleRate, spec.maxBlockSize);
  inRamp_.remaining = outRamp_.remaining = 0;
  sampleRate_ = spec.sampleRate;
  maxBlock_ = spec.maxBlockSize;
  scratchIn_.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
  scratchOut_.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
}

void NamBlock::reset() {
  if (maxBlock_ <= 0) return;
  inRamp_.remaining = outRamp_.remaining = 0;
  dsp_->ResetAndPrewarm(sampleRate_, maxBlock_);
}

void NamBlock::process(float* io, int numSamples) noexcept {
  if (cfg_.bypass || maxBlock_ <= 0) return;
  int done = 0;
  while (done < numSamples) {  // n <= maxBlockSize by contract; chunking is a safety net
    const int n = std::min(numSamples - done, maxBlock_);
    float* in = scratchIn_.data();
    float* out = scratchOut_.data();
    if (inRamp_.remaining > 0) {
      const int r = std::min(inRamp_.remaining, n);
      float c = inRamp_.cur;
      int i = 0;
      for (; i < r; ++i) {
        c += inRamp_.step;
        in[i] = io[done + i] * c;
      }
      inRamp_.remaining -= r;
      inRamp_.cur = inRamp_.remaining == 0 ? inGain_ : c;
      for (; i < n; ++i) in[i] = io[done + i] * inGain_;
    } else {
      for (int i = 0; i < n; ++i) in[i] = io[done + i] * inGain_;
    }
    dsp_->process(&in, &out, n);
    if (outRamp_.remaining > 0) {
      const int r = std::min(outRamp_.remaining, n);
      float c = outRamp_.cur;
      int i = 0;
      for (; i < r; ++i) {
        c += outRamp_.step;
        io[done + i] = out[i] * c;
      }
      outRamp_.remaining -= r;
      outRamp_.cur = outRamp_.remaining == 0 ? outGain_ : c;
      for (; i < n; ++i) io[done + i] = out[i] * outGain_;
    } else {
      for (int i = 0; i < n; ++i) io[done + i] = out[i] * outGain_;
    }
    done += n;
  }
}

}  // namespace sawblade
