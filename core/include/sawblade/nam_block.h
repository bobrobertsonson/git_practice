#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sawblade/processor.h"

namespace nam {
class DSP;
}

namespace sawblade {

struct NamBlockConfig {
  bool bypass = false;
  double inputGainDb = 0.0;   // into the model
  double outputGainDb = 0.0;  // after the model
  // If true and the model carries metadata.loudness, add (-18 - loudness) dB after the model.
  bool normalizeLoudness = false;
};

struct NamMetadata {
  std::string name;
  std::string gearType;
  std::string modeledBy;
};

// A parsed .nam file (architecture config, weights, metadata): immutable and shareable between
// threads. Building a NamBlock from it skips the file read and JSON parse; every block gets its
// own independent DSP state. Used by CaptureCache.
class NamModel {
 public:
  // Throws std::runtime_error (message contains the path) like NamBlock::load.
  static std::shared_ptr<const NamModel> load(const std::filesystem::path& path);
  ~NamModel();
  NamModel(const NamModel&) = delete;
  NamModel& operator=(const NamModel&) = delete;

  double expectedSampleRate() const noexcept { return expectedRate_; }  // <= 0: unknown
  std::optional<double> loudnessDb() const noexcept { return loudness_; }
  const NamMetadata& metadata() const noexcept { return meta_; }

 private:
  friend class NamBlock;
  NamModel();
  struct Data;
  std::unique_ptr<Data> data_;
  double expectedRate_ = -1.0;
  std::optional<double> loudness_;
  NamMetadata meta_{};
};

// One Neural Amp Modeler capture (NeuralAmpModelerCore, float samples) as a Processor:
//   process(): inputGain -> model -> outputGain (+ loudness normalization).
//
// Latency is 0: NAM models are causal and their latency is calibrated away at training time
// (the training pipeline aligns the capture), so the loaded model adds no reported delay.
//
// Threading: load()/prepare() are load-time (allocate, may throw). process() allocates nothing
// (verified by the test suite for the shipped WaveNet, LSTM and Linear architectures).
// reset() calls NAM's ResetAndPrewarm, which is NOT guaranteed allocation-free; call it from a
// non-audio thread or just before audio starts. To swap models live, build and prepare a
// new NamBlock on a background thread and hand it over with SwapSlot.
class NamBlock : public Processor {
 public:
  // Throws std::runtime_error (message contains the path) if the file cannot be loaded or the
  // model is not mono-in/mono-out.
  static std::unique_ptr<NamBlock> load(const std::filesystem::path& path, const NamBlockConfig& cfg);
  // Instantiates a fresh block (own state) from an already parsed model. Bit-identical to load(path).
  static std::unique_ptr<NamBlock> load(const NamModel& model, const NamBlockConfig& cfg);

  ~NamBlock() override;
  NamBlock(const NamBlock&) = delete;
  NamBlock& operator=(const NamBlock&) = delete;

  // Training sample rate; <= 0 means unknown.
  double expectedSampleRate() const noexcept { return expectedRate_; }
  std::optional<double> loudnessDb() const noexcept { return loudness_; }
  const NamMetadata& metadata() const noexcept { return meta_; }
  const NamBlockConfig& config() const noexcept { return cfg_; }

  // Throws std::runtime_error if spec.sampleRate != expectedSampleRate() and the model cannot
  // run at arbitrary rates. Resets the model and prewarms it.
  void prepare(const ProcessSpec& spec) override;
  void reset() override;
  void process(float* io, int numSamples) noexcept override;

 private:
  NamBlock() = default;
  void updateGains() noexcept;

  std::unique_ptr<nam::DSP> dsp_;
  NamBlockConfig cfg_{};
  NamMetadata meta_{};
  double expectedRate_ = -1.0;
  std::optional<double> loudness_;
  double sampleRate_ = 0.0;
  int maxBlock_ = 0;
  float inGain_ = 1.0f, outGain_ = 1.0f;
  std::vector<float> scratchIn_, scratchOut_;
};

}  // namespace sawblade
