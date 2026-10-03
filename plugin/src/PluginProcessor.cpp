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
  fadeBuf_.assign(kMinChunk, 0.0f);
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
  auto c = std::make_shared<const Preset>(clampedToParams(std::move(preset)));
  bool buildNow;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    wanted_ = c;
    status_.error.clear();
    buildNow = hostRate_ > 0.0;
  }
  // Nothing is committed (preset_, parameters, state) until the engine for it has been built: a
  // failed load leaves the previous preset and parameter values intact, and the running engine
  // is not nudged towards values it will never get.
  if (buildNow) {
    submit(/*fallbackToInit=*/false);
  } else {
    commit(*c);  // not prepared yet: nothing to build; prepareToPlay() will
    std::lock_guard<std::mutex> lk(mutex_);
    wanted_.reset();
  }
}

// Makes `p` the current preset and writes its values into the parameters (its engine's baseline
// equals them). Called from the loader thread right before the engine is published, or directly.
void SawbladeProcessor::commit(const Preset& p) {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    preset_ = p;
    status_.presetName = p.name;
  }
  writeParams(paramsFromPreset(p));
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
  // Capture paths in a saved state are absolute; the base only matters for hand-edited relative ones.
  // (current_path() throws if the working directory was deleted: use the error_code overload.)
  std::error_code ec;
  std::filesystem::path base = std::filesystem::current_path(ec);
  if (ec) base = std::filesystem::path("/");
  loadPresetJson(s, base);
}

// --- loader -------------------------------------------------------------------------------------
void SawbladeProcessor::submit(bool fallbackToInit) {
  if (hostRate_ <= 0.0) return;  // not prepared yet: prepareToPlay() will build
  EngineLoader::Request r;
  std::shared_ptr<const Preset> wanted;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    wanted = wanted_;
    status_.loading = true;
  }
  if (wanted) {
    // A user-requested preset that is not committed yet: build that one (a rebuild for a new rate
    // must not drop a load that is still in flight).
    r.preset = *wanted;
    r.wanted = wanted;
    r.beforePublish = [this, wanted] {
      commit(*wanted);
      std::lock_guard<std::mutex> lk(mutex_);
      if (wanted_ == wanted) wanted_.reset();
    };
  } else {
    r.preset = presetWithParams(false);
  }
  r.hostRate = hostRate_;
  r.maxBlock = maxBlock_;
  r.fallbackToInit = fallbackToInit;
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
  if (!o.built && !o.superseded && o.wanted && wanted_ == o.wanted) wanted_.reset();  // failed: keep the previous preset
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
  if (fadeBuf_.size() != mono_.size()) fadeBuf_.assign(mono_.size(), 0.0f);
  fadeLen_ = std::max(1, static_cast<int>(std::lround(kFadeSeconds * sampleRate)));
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

  // Adopt a newly published engine; the one it replaces fades out. (A stale-rate engine is never
  // used or faded.)
  // While fading nothing is adopted: engines published meanwhile coalesce in the slot (the newest
  // wins) and are adopted when the fade has finished, so a fade is never cut short or restarted.
  if (EngineRef* ref = fading_ ? nullptr : slot_.current(); ref != nullptr && ref->engine.get() != cur_.get()) {
    fading_ = std::move(cur_);
    cur_ = ref->engine;
    fadePos_ = 0;
  }
  if (fading_ && (fading_->hostRate() != hostRate_ || cur_->hostRate() != hostRate_)) fading_.reset();
  Engine* engine = cur_ && cur_->hostRate() == hostRate_ ? cur_.get() : nullptr;
  if (engine == nullptr) fading_.reset();
  if (engine != nullptr) {
    const ParamValues pv = readParams();
    engine->setParams(pv);
    if (fading_) fading_->setParams(pv);
  }

  const int chunk = static_cast<int>(mono_.size());
  float* mono = mono_.data();
  float* old = fadeBuf_.data();
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
    if (fading_) {
      std::memcpy(old, mono, static_cast<std::size_t>(len) * sizeof(float));
      fading_->process(old, old, len);
    }
    if (engine != nullptr) engine->process(mono, mono, len);
    if (fading_) {
      constexpr float kHalfPi = 1.57079632679489662f;
      const float inv = kHalfPi / static_cast<float>(fadeLen_);
      for (int i = 0; i < len && fadePos_ + i < fadeLen_; ++i) {
        const float th = (static_cast<float>(fadePos_ + i) + 0.5f) * inv;
        mono[i] = old[i] * std::cos(th) + mono[i] * std::sin(th);
      }
      fadePos_ += len;
      if (fadePos_ >= fadeLen_) fading_.reset();
    }
    for (int ch = 0; ch < numOut; ++ch) std::memcpy(buffer.getWritePointer(ch) + pos, mono, static_cast<std::size_t>(len) * sizeof(float));
  }
  for (int ch = numOut; ch < buffer.getNumChannels(); ++ch) buffer.clear(ch, 0, n);
}

}  // namespace sawblade::plugin
