#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "PluginEditor.h"
#include "sawblade/preset.h"
#include "sawblade/preset_reader.h"

namespace sawblade::plugin {
namespace {

juce::AudioProcessorValueTreeState::ParameterLayout createLayout() {
  juce::AudioProcessorValueTreeState::ParameterLayout layout;
  for (int i = 0; i < kNumParams; ++i) {
    const ParamSpec& s = paramSpec(i);
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{s.id, 1}, s.name,
        juce::NormalisableRange<float>(static_cast<float>(s.min), static_cast<float>(s.max)),
        static_cast<float>(s.def),
        juce::AudioParameterFloatAttributes()
            .withLabel(s.unit)
            .withStringFromValueFunction([decimals = s.unit.empty() ? 2 : 1](float v, int) { return juce::String(v, decimals); })
            .withValueFromStringFunction([](const juce::String& t) { return t.getFloatValue(); })));
  }
  return layout;
}

double round4(double v) { return std::round(v * 1e4) / 1e4; }

constexpr int kMinChunk = 4096;  // audio-thread scratch size; larger host blocks are processed in chunks

}  // namespace

SawbladeProcessor::SawbladeProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::mono(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "SawbladeParameters", createLayout()),
      preset_(makeInitPreset()) {
  for (int i = 0; i < kNumParams; ++i) {
    const juce::String id = paramSpec(i).id;
    paramAtomic_[static_cast<std::size_t>(i)] = apvts_.getRawParameterValue(id);
    paramObj_[static_cast<std::size_t>(i)] = apvts_.getParameter(id);
  }
  mono_.assign(kMinChunk, 0.0f);
  loader_ = std::make_unique<EngineLoader>(slot_, [this](const EngineLoader::Outcome& o) { onOutcome(o); });
}

SawbladeProcessor::~SawbladeProcessor() {
  loader_.reset();  // joins the worker before the slot and the rest are destroyed
}

juce::AudioProcessorEditor* SawbladeProcessor::createEditor() { return new SawbladeEditor(*this); }

bool SawbladeProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  const auto in = layouts.getMainInputChannelSet();
  const auto out = layouts.getMainOutputChannelSet();
  const bool outOk = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
  const bool inOk = in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
  return outOk && inOk;
}

// --- parameters ---------------------------------------------------------------------------------
ParamValues SawbladeProcessor::readParams() const noexcept {
  ParamValues v{};
  for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<double>(paramAtomic_[i]->load(std::memory_order_relaxed));
  return v;
}

void SawbladeProcessor::writeParams(const ParamValues& v) {
  for (std::size_t i = 0; i < v.size(); ++i)
    paramObj_[i]->setValueNotifyingHost(paramObj_[i]->convertTo0to1(static_cast<float>(v[i])));
}

// --- presets / state ----------------------------------------------------------------------------
Preset SawbladeProcessor::presetWithParams(bool rounded) const {
  Preset p;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    p = preset_;
  }
  ParamValues v = readParams();
  if (rounded)
    for (auto& x : v) x = round4(x);
  applyParams(p, v);
  return p;
}

Preset SawbladeProcessor::currentPreset() const { return presetWithParams(true); }

SawbladeProcessor::Status SawbladeProcessor::status() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return status_;
}

SlotBands SawbladeProcessor::postEqSlots() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return postEqSlotBands(preset_);
}

void SawbladeProcessor::loadPreset(Preset preset) {
  Preset c = clampedToParams(std::move(preset));
  const ParamValues v = paramsFromPreset(c);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    preset_ = c;
    status_.presetName = c.name;
    status_.error.clear();
  }
  writeParams(v);  // the engine's baseline (built from `c`) equals these values
  submit(/*fallbackToInit=*/false);
}

bool SawbladeProcessor::loadPresetJson(const std::string& json, const std::filesystem::path& baseDir, std::string* error) {
  try {
    nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) throw PresetError("", "invalid JSON");
    loadPreset(parsePreset(j, baseDir));
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    std::lock_guard<std::mutex> lk(mutex_);
    status_.error = e.what();
    return false;
  }
}

bool SawbladeProcessor::loadPresetFile(const std::filesystem::path& file, std::string* error) {
  try {
    loadPreset(sawblade::loadPresetFile(file));
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    std::lock_guard<std::mutex> lk(mutex_);
    status_.error = e.what();
    return false;
  }
}

void SawbladeProcessor::getStateInformation(juce::MemoryBlock& dest) {
  const std::string s = presetToStateJson(currentPreset());
  dest.replaceAll(s.data(), s.size());
}

void SawbladeProcessor::setStateInformation(const void* data, int size) {
  if (data == nullptr || size <= 0) return;
  const std::string s(static_cast<const char*>(data), static_cast<std::size_t>(size));
  loadPresetJson(s, std::filesystem::current_path());
}

// --- loader -------------------------------------------------------------------------------------
void SawbladeProcessor::submit(bool fallbackToInit) {
  if (hostRate_ <= 0.0) return;  // not prepared yet: prepareToPlay() will build
  EngineLoader::Request r;
  r.preset = presetWithParams(false);
  r.hostRate = hostRate_;
  r.maxBlock = maxBlock_;
  r.fallbackToInit = fallbackToInit;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    status_.loading = true;
  }
  const std::uint64_t id = loader_->submit(std::move(r));
  std::lock_guard<std::mutex> lk(mutex_);
  lastSubmitted_ = id;
}

void SawbladeProcessor::onOutcome(const EngineLoader::Outcome& o) {  // loader thread
  if (o.published) {
    // The host learns the new latency as soon as the engine exists; the audio thread switches to
    // it at the start of its next block. (setLatencySamples is not audio-thread safe, so it is
    // never called from processBlock.)
    setLatencySamples(o.latencySamples);
  }
  std::lock_guard<std::mutex> lk(mutex_);
  if (o.id == lastSubmitted_ || o.id > lastSubmitted_) {
    status_.loading = false;
    status_.error = o.error;
  }
  if (o.published) {
    status_.latencySamples = o.latencySamples;
    status_.hostRate = o.hostRate;
    status_.builtMaxBlock = o.maxBlock;
    status_.modelRate = o.modelRate;
    status_.liveCompatible = o.info.liveCompatible;
    status_.resampling = std::fabs(o.modelRate - o.hostRate) > 1e-6;
    status_.info = o.info;
    if (o.built) status_.presetName = o.presetName;
  }
}

bool SawbladeProcessor::waitForLoader(std::chrono::milliseconds timeout) { return loader_->waitIdle(timeout); }

// --- audio --------------------------------------------------------------------------------------
void SawbladeProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
  hostRate_ = sampleRate;
  maxBlock_ = std::max(1, samplesPerBlock);
  if (static_cast<int>(mono_.size()) < kMinChunk) mono_.assign(kMinChunk, 0.0f);
  {
    // Hosts may call prepareToPlay again with unchanged settings: the running engine is still
    // right (it handles any block size), so there is nothing to rebuild.
    std::lock_guard<std::mutex> lk(mutex_);
    if (!status_.loading && status_.error.empty() && status_.hostRate == sampleRate && status_.builtMaxBlock >= maxBlock_) return;
  }
  // The rate (or block size) changed: rebuild, and wait for it so the latency the host reads right
  // after this call is correct. A failed build must not leave an engine of the wrong rate behind:
  // fall back to pass-through.
  submit(/*fallbackToInit=*/true);
  waitForLoader();
}

void SawbladeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
  juce::ScopedNoDenormals noDenormals;
  const int n = buffer.getNumSamples();
  const int numIn = std::min(getTotalNumInputChannels(), buffer.getNumChannels());
  const int numOut = std::min(getTotalNumOutputChannels(), buffer.getNumChannels());
  if (n <= 0 || numOut <= 0) return;

  Engine* engine = slot_.current();
  if (engine != nullptr && engine->hostRate() != hostRate_) engine = nullptr;  // stale rate: pass through
  if (engine != nullptr) engine->setParams(readParams());

  const int chunk = static_cast<int>(mono_.size());
  float* mono = mono_.data();
  for (int pos = 0; pos < n; pos += chunk) {
    const int len = std::min(chunk, n - pos);
    if (numIn <= 0) {
      std::memset(mono, 0, static_cast<std::size_t>(len) * sizeof(float));
    } else if (numIn == 1) {
      std::memcpy(mono, buffer.getReadPointer(0) + pos, static_cast<std::size_t>(len) * sizeof(float));
    } else {
      const float* l = buffer.getReadPointer(0) + pos;
      const float* r = buffer.getReadPointer(1) + pos;
      for (int i = 0; i < len; ++i) mono[i] = 0.5f * (l[i] + r[i]);
    }
    if (engine != nullptr) engine->process(mono, mono, len);
    for (int ch = 0; ch < numOut; ++ch) std::memcpy(buffer.getWritePointer(ch) + pos, mono, static_cast<std::size_t>(len) * sizeof(float));
  }
  for (int ch = numOut; ch < buffer.getNumChannels(); ++ch) buffer.clear(ch, 0, n);
}

}  // namespace sawblade::plugin
