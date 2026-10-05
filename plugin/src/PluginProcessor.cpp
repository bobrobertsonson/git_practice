#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "AppPaths.h"
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

constexpr int kMinChunk = 4096;  // audio-thread scratch size; larger host blocks are processed in chunks

}  // namespace

SawbladeProcessor::SawbladeProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::mono(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "SawbladeParameters", createLayout()),
      preset_(makeInitPreset()),
      matchSettings_(defaultSettingsFile()),
      jobs_(matchSettings_, defaultJobsDir()),
      audition_(*this) {
  for (int i = 0; i < kNumParams; ++i) {
    const juce::String id = paramSpec(i).id;
    paramAtomic_[static_cast<std::size_t>(i)] = apvts_.getRawParameterValue(id);
    paramObj_[static_cast<std::size_t>(i)] = apvts_.getParameter(id);
  }
  mono_.assign(kMinChunk, 0.0f);
  backL_.assign(kMinChunk, 0.0f);
  backR_.assign(kMinChunk, 0.0f);
  fadeBuf_.assign(kMinChunk, 0.0f);
  playAlong_.setStandalone(wrapperType == wrapperType_Standalone);
  loader_ = std::make_unique<EngineLoader>(slot_, [this](const EngineLoader::Outcome& o) { onOutcome(o); });
  jobs_.pruneAsync();  // old match job folders: on the runner's own thread, not here
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
  for (std::size_t i = 0; i < v.size(); ++i) v[i] = snapParam(static_cast<double>(paramAtomic_[i]->load(std::memory_order_relaxed)));
  return v;
}

void SawbladeProcessor::writeParams(const ParamValues& v) {
  for (std::size_t i = 0; i < v.size(); ++i)
    paramObj_[i]->setValueNotifyingHost(paramObj_[i]->convertTo0to1(static_cast<float>(v[i])));
}

// --- presets / state ----------------------------------------------------------------------------
Preset SawbladeProcessor::presetWithParams() const {
  Preset p;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    p = preset_;
  }
  ParamValues v = readParams();  // already on the 1e-4 grid
  applyParams(p, v);
  return p;
}

Preset SawbladeProcessor::currentPreset() const { return presetWithParams(); }

SawbladeProcessor::Status SawbladeProcessor::status() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return status_;
}

SawbladeProcessor::EngineParamState SawbladeProcessor::engineParamState() const {
  EngineParamState st;
  if (cur_) {
    st.live = cur_->liveParams();
    st.baseline = cur_->baseline();
    st.valid = true;
  }
  return st;
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

void SawbladeProcessor::restorePreset(Preset preset) {
  const Preset c = clampedToParams(std::move(preset));
  {
    std::lock_guard<std::mutex> lk(mutex_);
    wanted_.reset();
    status_.error.clear();
  }
  commit(c);
  submit(/*fallbackToInit=*/false);  // builds the committed preset (no pending "wanted")
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

bool SawbladeProcessor::loadPresetJson(const std::string& json, const std::filesystem::path& baseDir, std::string* error,
                                       bool restore) {
  try {
    nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) throw PresetError("", "invalid JSON");
    if (restore)
      restorePreset(parsePreset(j, baseDir));
    else
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
  std::string s = presetToStateJson(currentPreset());
  // Play-along UI state (not tone) rides along as an optional object; untouched sessions save exactly the preset.
  if (const PlayAlongSettings pa = playAlong_.settings(); !pa.isDefault()) {
    nlohmann::json j = nlohmann::json::parse(s);
    j["playAlong"] = playAlongToJson(pa);
    s = j.dump(2);
  }
  dest.replaceAll(s.data(), s.size());
}

void SawbladeProcessor::setStateInformation(const void* data, int size) {
  if (data == nullptr || size <= 0) return;
  std::string s(static_cast<const char*>(data), static_cast<std::size_t>(size));
  // Capture paths in a saved state are absolute; the base only matters for hand-edited relative ones.
  // (current_path() throws if the working directory was deleted: use the error_code overload.)
  std::error_code ec;
  std::filesystem::path base = std::filesystem::current_path(ec);
  if (ec) base = std::filesystem::path("/");
  // A `playAlong` that is not an object would make the core parser reject the whole state: drop it, so the
  // tone still loads.
  nlohmann::json j = nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
  if (j.is_object())
    if (auto it = j.find("playAlong"); it != j.end() && !it->is_object()) {
      j.erase(it);
      s = j.dump();
    }
  loadPresetJson(s, base, nullptr, /*restore=*/true);
  // A state without `playAlong` (older sessions) leaves the play-along as it is. Never throws: a missing
  // or unreadable folder shows up in playAlong().loadStatus().
  if (j.is_object())
    if (auto it = j.find("playAlong"); it != j.end()) playAlong_.restore(playAlongFromJson(*it));
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
    r.preset = presetWithParams();
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
    playAlong_.setRigLatencySamples(o.latencySamples);  // the backing is delayed by the rig latency
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
  if (static_cast<int>(backL_.size()) < kMinChunk) {
    backL_.assign(kMinChunk, 0.0f);
    backR_.assign(kMinChunk, 0.0f);
  }
  fadeLen_ = std::max(1, static_cast<int>(std::lround(kFadeSeconds * sampleRate)));
  playAlong_.prepare(sampleRate, std::min(samplesPerBlock, kMinChunk), static_cast<int>(std::lround(sampleRate)));  // up to 1 s of rig latency
  recorder_.prepare(sampleRate);  // ring for the DI recorder (>= 2 s), writer thread
  {
    // Hosts may call prepareToPlay again with unchanged settings: the running engine is still
    // right (it handles any block size), so there is nothing to rebuild.
    std::lock_guard<std::mutex> lk(mutex_);
    if (!status_.loading && status_.error.empty() && status_.hostRate == sampleRate && status_.builtMaxBlock >= maxBlock_) {
      playAlong_.setRigLatencySamples(getLatencySamples());
      return;
    }
  }
  // The rate (or block size) changed: rebuild, and wait for it so the latency the host reads right
  // after this call is correct. A failed build must not leave an engine of the wrong rate behind:
  // fall back to pass-through.
  submit(/*fallbackToInit=*/true);
  waitForLoader();
  playAlong_.setRigLatencySamples(getLatencySamples());
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

  // The host transport for the play-along (plugin mode only; Standalone free-runs).
  PlayAlong::HostTransport host;
  if (!playAlong_.standalone()) {
    if (auto* head = getPlayHead()) {
      if (const auto pos = head->getPosition()) {
        host.playing = pos->getIsPlaying();
        if (const auto t = pos->getTimeInSamples()) host.sample = *t;
        else if (const auto sec = pos->getTimeInSeconds()) host.sample = static_cast<std::int64_t>(std::llround(*sec * hostRate_));
        else host.playing = false;
      }
    }
  }

  const int chunk = static_cast<int>(mono_.size());
  float* mono = mono_.data();
  float* backL = backL_.data();
  float* backR = backR_.data();
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
    {
      // DI recorder: the clean input, before the gate and the rig (mono is overwritten in place below).
      // When a take begins in this chunk, the play-along reports the stem sample it plays at the chunk's first sample.
      TakeStartInfo startInfo;
      const TakeStartInfo* startPtr = nullptr;
      if (recorder_.startPending()) {
        const PlayAlong::HostTransport h{host.playing, host.sample + pos};
        playAlong_.prepareBlock(h);
        startInfo = playAlong_.takeStartInfo(h);
        startPtr = &startInfo;
      }
      recorder_.process(mono, len, startPtr);
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

    // Backing track, after the rig, on the same latency (the player delays it by the rig latency).
    playAlong_.process(mono, backL, backR, len, {host.playing, host.sample + pos});
    if (numOut >= 2) {
      float* l = buffer.getWritePointer(0) + pos;
      float* r = buffer.getWritePointer(1) + pos;
      for (int i = 0; i < len; ++i) {
        l[i] += backL[i];
        r[i] += backR[i];
      }
    } else {
      float* l = buffer.getWritePointer(0) + pos;
      for (int i = 0; i < len; ++i) l[i] += 0.5f * (backL[i] + backR[i]);
    }
  }
  for (int ch = numOut; ch < buffer.getNumChannels(); ++ch) buffer.clear(ch, 0, n);
}

}  // namespace sawblade::plugin
