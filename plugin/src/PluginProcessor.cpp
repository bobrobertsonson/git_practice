#include "PluginProcessor.h"
#include "SongDecoder.h"

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
    if (!s.choices.empty()) {
      juce::StringArray names;
      for (const std::string& c : s.choices) names.add(c);
      layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{s.id, 1}, s.name, names, static_cast<int>(s.def)));
      continue;
    }
    const int decimals = s.unit.empty() ? 2 : (s.unit == "Hz" || s.unit == "%") ? 0 : 1;
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{s.id, 1}, s.name,
        juce::NormalisableRange<float>(static_cast<float>(s.min), static_cast<float>(s.max)),
        static_cast<float>(s.def),
        juce::AudioParameterFloatAttributes()
            .withLabel(s.unit)
            .withStringFromValueFunction([decimals](float v, int) { return juce::String(v, decimals); })
            .withValueFromStringFunction([](const juce::String& t) { return t.getFloatValue(); })));
  }
  return layout;
}

constexpr std::uint64_t kNoGeneration = ~std::uint64_t{0};
constexpr int kMinChunk = 4096;  // audio-thread scratch size; larger host blocks are processed in chunks

}  // namespace

SawbladeProcessor::SawbladeProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::mono(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "SawbladeParameters", createLayout()),
      preset_(makeInitPreset()),
      presetGeneration_(kNoGeneration),
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
  playAlong_.setSongDecoder(&decodeSongFile);
  loader_ = std::make_unique<EngineLoader>(slot_, [this](const EngineLoader::Outcome& o) { onOutcome(o); });
  apvts_.addParameterListener(paramSpec(kSawCircuit).id, this);
  jobs_.pruneAsync();  // old match job folders: on the runner's own thread, not here
  startTimerHz(10);
}

SawbladeProcessor::~SawbladeProcessor() {
  stopTimer();
  loader_.reset();  // joins the worker before the slot and the rest are destroyed
  apvts_.removeParameterListener(paramSpec(kSawCircuit).id, this);
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

std::optional<CircuitSlot> SawbladeProcessor::circuitSlot() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return findCircuitBlock(preset_);
}

// --- CIRCUIT switch -----------------------------------------------------------------------------
void SawbladeProcessor::parameterChanged(const juce::String&, float value) {
  // During commit() the parameters are being rewritten from a preset, but an edit that lands meanwhile
  // must not be lost: flag it, the timer retries (circuitChanged() is a no-op when nothing differs).
  if (committing_.load() > 0) {
    // Flag only an edit that is not commit's own write of the value it is publishing.
    if (static_cast<int>(std::lround(value)) != commitCircuit_.load()) circuitDirty_.store(true);
  }
  else if (juce::MessageManager::existsAndIsCurrentThread())
    circuitChanged();
  else
    circuitDirty_.store(true);  // audio or loader thread: the timer handles it on the message thread
}

void SawbladeProcessor::timerCallback() {
  if (committing_.load() > 0) return;
  if (circuitDirty_.exchange(false)) circuitChanged();
  ladderTick();
}

// --- gain ladders -------------------------------------------------------------------------------
SawbladeProcessor::LadderInfo SawbladeProcessor::ladderInfo(int path) const {
  LadderInfo li;
  std::shared_ptr<Engine> e;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    e = published_.lock();
  }
  if (!e || path < 0 || path > 1) return li;
  const LadderState st = e->ladderState(path);
  if (!st.has) return li;
  const PathPreset& pp = path == 0 ? e->builtPreset().a : e->builtPreset().b;
  const auto* nam = dynamic_cast<const NamBlockParams*>(pp.blocks[static_cast<std::size_t>(ampIndex(pp))].params.get());
  if (!nam) return li;
  const auto& l = nam->model.ladder;
  li.has = true;
  li.rungCount = st.rungCount;
  li.activeIndex = st.committed;
  li.targetIndex = st.target;
  li.pending = st.pending;
  li.activeGain = l[static_cast<std::size_t>(st.committed)].gain;
  li.activeName = l[static_cast<std::size_t>(st.committed)].name;
  li.activeModelId = l[static_cast<std::size_t>(st.committed)].modelId;
  li.targetName = l[static_cast<std::size_t>(st.target)].name;
  li.targetModelId = l[static_cast<std::size_t>(st.target)].modelId;
  li.missingRungs = rungs_.missingRungs();
  return li;
}

// Moving GAIN moves the active rung (audio thread); the preset records it as `gainStep`, so a save / reload / offline
// render uses the same rung. Only for the engine that matches the committed preset.
void SawbladeProcessor::ladderWriteBack(const std::shared_ptr<Engine>& e) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (presetGeneration_ != e->generation()) return;
  for (int k = 0; k < 2; ++k) {
    const LadderState st = e->ladderState(k);
    if (!st.has) continue;
    const PathPreset& built = k == 0 ? e->builtPreset().a : e->builtPreset().b;
    const auto* nam = dynamic_cast<const NamBlockParams*>(built.blocks[static_cast<std::size_t>(ampIndex(built))].params.get());
    if (!nam) continue;
    AmpControls& ac = (k == 0 ? preset_.a : preset_.b).ampControls;
    const std::string& id = nam->model.ladder[static_cast<std::size_t>(st.committed)].modelId;
    if (st.committed == st.own && ac.gainStep.empty()) continue;  // never moved off the block's own capture
    if (ac.gainStep != id) ac.gainStep = id;
  }
}

void SawbladeProcessor::ladderTick() {
  // 1. A fetched ladder goes into the preset (and rebuilds once, keeping the monitor state).
  std::vector<LadderFetchResult> done;
  {
    std::lock_guard<std::mutex> lk(fetchMutex_);
    done.swap(fetched_);
  }
  for (const LadderFetchResult& r : done) {
    if (r.rungs.empty()) continue;
    Preset p = editBasePreset();
    if (applyLadderToPreset(p, r.toneId, r.rungs)) {
      loadPreset(std::move(p), /*keepMonitor=*/true);
    } else if (const auto need = toneIdsNeedingLadder(p); std::find(need.begin(), need.end(), r.toneId) != need.end()) {
      // The ladder has the tone's `standard`-size models; a capture of another size is not one of them (the preset does not
      // record the size), so it gets no ladder: GAIN stays drive-only. Say so.
      std::lock_guard<std::mutex> lk(fetchMutex_);
      ladderNotes_.push_back("tone " + r.toneId + ": the amp capture's model is not in the " + kLadderSize +
                             "-size gain ladder (another model size?); GAIN stays drive-only");
    }
  }
  // 2. The next ladder to fetch (one run at a time, once per tone per session).
  if (ladderFetch_.load() && !networkToolsDisabled() && !fetchRunning_.load() && !ladderTool_.running()) {
    for (const std::string& id : toneIdsNeedingLadder(editBasePreset())) {
      if (!ladderTried_.insert(id).second) continue;
      std::error_code ec;
      if (!std::filesystem::exists(settings::t3kExecutable(), ec)) break;  // no tool: nothing to ask
      fetchRunning_.store(true);
      ladderFetches_.fetch_add(1);
      const bool started = ladderTool_.start(
          ladderArgs(id), nullptr,
          [this, id](const T3kTool::Result& res) {  // background thread
            LadderFetchResult r;
            if (res.status == T3kTool::Status::Ok) r = parseLadderOutput(res.output);
            r.toneId = id;
            {
              std::lock_guard<std::mutex> lk(fetchMutex_);
              fetched_.push_back(std::move(r));
            }
            fetchRunning_.store(false);
          });
      if (!started) fetchRunning_.store(false);
      break;
    }
  }
  // 3. The running engine's rungs: write the active one back, keep the nearest models loaded.
  std::shared_ptr<Engine> e;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    e = published_.lock();
  }
  if (!e) return;
  bool any = false;
  std::uint64_t key = 0;
  for (int k = 0; k < 2; ++k) {
    const LadderState st = e->ladderState(k);
    if (!st.has) continue;
    any = true;
    key = key * 131 + static_cast<std::uint64_t>(st.target + 1) * 7 + static_cast<std::uint64_t>(st.committed + 1) + (st.pending ? 1000 : 0);
  }
  if (!any) return;
  ladderWriteBack(e);
  fetchMissingRung(*e);
  if (rungArrived_.exchange(false)) lastRungKey_ = ~0ull;
  key ^= reinterpret_cast<std::uintptr_t>(e.get());
  if (key != lastRungKey_ || ++rungTicks_ >= 30) {  // on a change, else every 3 s (picks up newly cached rungs)
    lastRungKey_ = key;
    rungTicks_ = 0;
    rungs_.request(e);
  }
}

// A ladder rung whose model is not in the capture cache is fetched through `sawblade-t3k fetch <tone> --model <id>` (the
// same tool the resolve flow uses), one at a time, nearest the sounding rung first, once per rung per session. Only if the
// tool is configured and exists; never on the audio thread. The rung loader picks the file up when it arrives.
void SawbladeProcessor::fetchMissingRung(const Engine& e) {
  if (!ladderFetch_.load() || networkToolsDisabled() || fetchRunning_.load() || ladderTool_.running()) return;
  std::error_code ec;
  if (!std::filesystem::exists(settings::t3kExecutable(), ec)) return;
  for (int k = 0; k < 2; ++k) {
    const LadderState st = e.ladderState(k);
    if (!st.has) continue;
    const PathPreset& pp = k == 0 ? e.builtPreset().a : e.builtPreset().b;
    const auto* nam = dynamic_cast<const NamBlockParams*>(pp.blocks[static_cast<std::size_t>(ampIndex(pp))].params.get());
    if (!nam || !nam->model.source || nam->model.source->provider != "tone3000") continue;
    const auto& l = nam->model.ladder;
    std::vector<int> order(l.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return std::abs(a - st.target) < std::abs(b - st.target); });
    for (int i = 0; i < std::min(static_cast<int>(l.size()), kMaxLoadedRungs); ++i) {  // only the rungs that would be loaded
      const int ri = order[static_cast<std::size_t>(i)];
      const LadderRung& r = l[static_cast<std::size_t>(ri)];
      if (ri == st.own) continue;  // the block's own capture is a file of the preset, not a cache entry
      if (locateRungFile(nam->model, r)) continue;
      const std::string tone = nam->model.source->id;
      if (!rungTried_.insert(tone + ":" + r.modelId).second) continue;
      fetchRunning_.store(true);
      rungFetches_.fetch_add(1);
      const bool started = ladderTool_.start(
          {"fetch", tone, "--model", r.modelId, "--json", "--cache-dir", captureCacheRoot().string()}, nullptr,
          [this](const T3kTool::Result&) {  // background thread: success or not, the rung loader looks again
            rungArrived_.store(true);
            fetchRunning_.store(false);
          });
      if (!started) fetchRunning_.store(false);
      return;
    }
  }
}

std::vector<std::string> SawbladeProcessor::ladderMessages() const {
  std::vector<std::string> v;
  {
    std::lock_guard<std::mutex> lk(fetchMutex_);
    v = ladderNotes_;
  }
  std::shared_ptr<Engine> e;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    e = published_.lock();
  }
  if (e) {
    const auto m = e->ladderMessages();
    v.insert(v.end(), m.begin(), m.end());
  }
  return v;
}

bool SawbladeProcessor::waitForLadderWork(std::chrono::milliseconds timeout) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (!fetchRunning_.load() && !ladderTool_.running() && rungs_.waitIdle(std::chrono::milliseconds(20))) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

// The user (or the host) moved the CIRCUIT switch: if it names a different circuit than the preset's
// first circuit block, rebuild once with that block replaced (level/volume, mix, tightness and clip
// carried over, the rest at the new circuit's defaults). Inert when the preset has no circuit block.
void SawbladeProcessor::circuitChanged() {
  const int idx = std::clamp(static_cast<int>(std::lround(paramAtomic_[kSawCircuit]->load())), 0, kNumCircuits - 1);
  std::shared_ptr<const Preset> wanted;
  Preset p;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    wanted = wanted_;
    if (!wanted) p = preset_;
  }
  if (wanted) p = *wanted;  // a load still in flight is the latest intent: switch on top of it
  else applyParams(p, readParams());
  const auto slot = findCircuitBlock(p);
  if (!slot || static_cast<int>(slot->circuit) == idx) return;
  loadPreset(switchCircuit(p, static_cast<Circuit>(idx)));
}

void SawbladeProcessor::loadPreset(Preset preset, bool keepMonitor) {
  auto c = std::make_shared<const Preset>(clampedToParams(std::move(preset)));
  if (!keepMonitor) userLoadSerial_.fetch_add(1);
  bool buildNow;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    wanted_ = c;
    wantedKeepsMonitor_ = keepMonitor;
    dropReplacedRemeasure();
    status_.error.clear();
    buildNow = hostRate_ > 0.0;
  }
  // Nothing is committed (preset_, parameters, state) until the engine for it has been built: a
  // failed load leaves the previous preset and parameter values intact, and the running engine
  // is not nudged towards values it will never get.
  if (buildNow) {
    submit(/*fallbackToInit=*/false);
  } else {
    commit(*c, kNoGeneration, !keepMonitor);  // not prepared yet: nothing to build; prepareToPlay() will
    std::lock_guard<std::mutex> lk(mutex_);
    wanted_.reset();
    dropReplacedRemeasure();
  }
}

// mutex_ held. A re-measure request that a later load / edit / restore has replaced in the loader never
// produces an outcome: forget it, so the editor does not wait for it and the preset does not become Auto.
void SawbladeProcessor::dropReplacedRemeasure() {
  if (remeasureWanted_ && wanted_ != remeasureWanted_) {
    remeasureWanted_.reset();
    remeasureBase_.reset();
    status_.alignMeasuring = status_.levelsMeasuring = false;
  }
}

void SawbladeProcessor::restorePreset(Preset preset) {
  const Preset c = clampedToParams(std::move(preset));
  userLoadSerial_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    wanted_.reset();
    dropReplacedRemeasure();
    status_.error.clear();
  }
  commit(c, kNoGeneration, /*clearMonitor=*/true);
  submit(/*fallbackToInit=*/false);  // builds the committed preset (no pending "wanted")
}

// Makes `p` the current preset and writes its values into the parameters (its engine's baseline
// equals them). Called from the loader thread right before the engine is published, or directly.
// `generation` is the loader request the engine for `p` is built by (kNoGeneration: not known yet, no
// engine matches until the build publishes and records its id).
void SawbladeProcessor::commit(const Preset& p, std::uint64_t generation, bool clearMonitor) {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    preset_ = p;
    status_.presetName = p.name;
    presetGeneration_ = generation;
    ++presetSerial_;
    if (clearMonitor) monitor_ = {};
    publishLive();
  }
  const ParamValues pv = paramsFromPreset(p);
  commitCircuit_.store(static_cast<int>(std::lround(pv[kSawCircuit])));
  committing_.fetch_add(1);
  writeParams(pv);
  committing_.fetch_sub(1);
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
  // Likewise the export panel's settings (phase 12), omitted while they are all defaults.
  if (const ExportSettings es = exportSettings(); !es.isDefault()) {
    nlohmann::json j = nlohmann::json::parse(s);
    j["export"] = exportSettingsToJson(es);
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
  if (j.is_object()) {
    bool dropped = false;
    for (const char* key : {"playAlong", "export"})
      if (auto it = j.find(key); it != j.end() && !it->is_object()) {
        j.erase(it);
        dropped = true;
      }
    // States saved by builds before v0.1.1 hold the "no capture" placeholder as an absolute path ("<cwd>/(none)"):
    // put it back to the bare placeholder, so it is never taken for a file.
    if (auto cab = j.find("cab"); cab != j.end() && cab->is_object())
      for (const char* key : {"ir", "irA", "irB"})
        if (auto ir = cab->find(key); ir != cab->end() && ir->is_object())
          if (auto f = ir->find("file"); f != ir->end() && f->is_string() && std::filesystem::path(f->get<std::string>()).filename() == kNoCaptureFile) {
            *f = kNoCaptureFile;
            dropped = true;
          }
    if (dropped) s = j.dump();
  }
  loadPresetJson(s, base, nullptr, /*restore=*/true);
  // A state without `playAlong` (older sessions) leaves the play-along as it is. Never throws: a missing
  // or unreadable folder shows up in playAlong().loadStatus().
  if (j.is_object())
    if (auto it = j.find("playAlong"); it != j.end()) playAlong_.restore(playAlongFromJson(*it));
  // `export`: absent = defaults (a session saved before the panel existed, or one that never touched it).
  if (j.is_object()) {
    auto it = j.find("export");
    setExportSettings(it != j.end() ? exportSettingsFromJson(*it) : ExportSettings{});
  }
}

ExportSettings SawbladeProcessor::exportSettings() const {
  std::lock_guard<std::mutex> lk(exportMutex_);
  return exportSettings_;
}

void SawbladeProcessor::setExportSettings(const ExportSettings& s) {
  {
    std::lock_guard<std::mutex> lk(exportMutex_);
    if (exportSettings_ == s) return;
    exportSettings_ = s;
  }
  ++exportSerial_;
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
    const bool keep = wantedKeepsMonitor_;
    r.beforePublish = [this, wanted, keep](std::uint64_t id) {
      commit(*wanted, id, !keep);
      std::lock_guard<std::mutex> lk(mutex_);
      if (wanted_ == wanted) wanted_.reset();
    };
  } else {
    std::uint64_t serial;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      serial = presetSerial_;
    }
    r.preset = presetWithParams();
    // Record the generation only if no newer preset was committed since this request was made: otherwise
    // the engine (built from the old preset) must not receive the new preset's live values.
    r.beforePublish = [this, serial](std::uint64_t id) {
      std::lock_guard<std::mutex> lk(mutex_);
      if (presetSerial_ == serial) presetGeneration_ = id;
    };
  }
  // A new engine starts with the current mutes (no blip while it fades in).
  r.configure = [this](Engine& e) {
    std::lock_guard<std::mutex> lk(mutex_);
    e.setInitialMutes(monitor_.muteA, monitor_.muteB);
  };
  r.hostRate = hostRate_;
  r.maxBlock = maxBlock_;
  r.fallbackToInit = fallbackToInit;
  const std::uint64_t id = loader_->submit(std::move(r));
  std::lock_guard<std::mutex> lk(mutex_);
  lastSubmitted_ = id;
}

void SawbladeProcessor::onOutcome(const EngineLoader::Outcome& o) {  // loader thread
  bool restoreCircuit = false;
  {
  if (o.published) {
    // The host learns the new latency as soon as the engine exists; the audio thread switches to
    // it at the start of its next block. (setLatencySamples is not audio-thread safe, so it is
    // never called from processBlock.)
    setLatencySamples(o.latencySamples);
    playAlong_.setRigLatencySamples(o.latencySamples);  // the backing is delayed by the rig latency
  }
  std::lock_guard<std::mutex> lk(mutex_);
  if (o.wanted && o.wanted == remeasureWanted_) {
    remeasureWanted_.reset();
    remeasureBase_.reset();
    status_.alignMeasuring = status_.levelsMeasuring = false;
    if (o.built && remeasureLevels_) {
      // Built with levelMatch auto: its resolved trims are what a manual preset with the same numbers
      // does, so the write-back needs no rebuild.
      preset_.levelMatch = {LevelMatchMode::Manual, o.info.trimDb[0], o.info.trimDb[1]};
    } else if (o.built) {
      // The engine was built in auto mode: its resolved values are what a manual preset with the same
      // numbers does, so the write-back needs no rebuild.
      status_.measuredAlign = o.info.align;
      preset_.align = {AlignMode::Manual, preset_.align.maxLagMs, o.info.align.delaySamplesB, o.info.align.invertB};
    }
  }
  if (o.id == lastSubmitted_ || o.id > lastSubmitted_) {
    status_.loading = false;
    status_.error = o.error;
  }
  if (!o.built && !o.superseded && o.wanted && wanted_ == o.wanted) {
    wanted_.reset();  // failed: keep the previous preset
    restoreCircuit = true;
  }
  if (o.published) {
    published_ = o.engine;
    status_.latencySamples = o.latencySamples;
    status_.hostRate = o.hostRate;
    status_.builtMaxBlock = o.maxBlock;
    status_.modelRate = o.modelRate;
    status_.liveCompatible = o.info.liveCompatible;
    status_.resampling = std::fabs(o.modelRate - o.hostRate) > 1e-6;
    status_.info = o.info;
    status_.generation = o.id;
    if (o.built) status_.presetName = o.presetName;
    publishLive();  // a fresh engine always gets a snapshot of its own generation (mutes survive rebuilds)
  }
  }
  if (restoreCircuit) {
    // A failed load (e.g. a circuit switch) keeps the previous preset: the CIRCUIT lever must show what is
    // sounding and saved, not what was asked for.
    Preset prev;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      prev = preset_;
    }
    // Only the lever, and only if it disagrees: knob tweaks made during the failed build survive.
    if (const auto slot = findCircuitBlock(prev); slot && static_cast<int>(std::lround(paramAtomic_[kSawCircuit]->load())) != static_cast<int>(slot->circuit)) {
      commitCircuit_.store(static_cast<int>(slot->circuit));
      committing_.fetch_add(1);
      paramObj_[kSawCircuit]->setValueNotifyingHost(paramObj_[kSawCircuit]->convertTo0to1(static_cast<float>(static_cast<int>(slot->circuit))));
      committing_.fetch_sub(1);
    }
  }
}

// --- rig editor hooks ---------------------------------------------------------------------------
void SawbladeProcessor::publishLive() {
  auto snap = std::make_unique<LiveSnapshot>();
  snap->generation = presetGeneration_;
  snap->live = LiveParams::fromPreset(clampedToParams(preset_));
  snap->live.muteA = monitor_.muteA;
  snap->live.muteB = monitor_.muteB;
  liveSlot_.publish(std::move(snap));
  liveSlot_.collectGarbage();
}

Preset SawbladeProcessor::editBasePreset() const {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (wanted_) return (wanted_ == remeasureWanted_ && remeasureBase_) ? *remeasureBase_ : *wanted_;
  }
  return presetWithParams();
}

void SawbladeProcessor::applyLiveEdit(const std::function<void(Preset&)>& edit) {
  std::lock_guard<std::mutex> lk(mutex_);
  edit(preset_);
  publishLive();
}

void SawbladeProcessor::setMonitor(bool muteA, bool muteB) {
  std::lock_guard<std::mutex> lk(mutex_);
  monitor_ = {muteA, muteB};
  publishLive();
}

SawbladeProcessor::Monitor SawbladeProcessor::monitor() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return monitor_;
}

std::uint64_t SawbladeProcessor::presetGeneration() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return presetGeneration_;
}

void SawbladeProcessor::remeasureAlignment() { remeasure(/*levels=*/false); }
void SawbladeProcessor::matchLevels() { remeasure(/*levels=*/true); }

void SawbladeProcessor::remeasure(bool levels) {
  if (hostRate_ <= 0.0) return;
  Preset p = editBasePreset();
  if (!p.a.enabled || !p.b.enabled) return;  // the chain skips alignment / level matching with a path disabled
  auto base = std::make_shared<const Preset>(p);  // what edits made while this is pending start from
  if (levels) p.levelMatch.mode = LevelMatchMode::Auto;
  else p.align.mode = AlignMode::Auto;
  auto c = std::make_shared<const Preset>(clampedToParams(std::move(p)));
  {
    std::lock_guard<std::mutex> lk(mutex_);
    wanted_ = c;
    wantedKeepsMonitor_ = true;
    remeasureWanted_ = c;
    remeasureBase_ = base;
    remeasureLevels_ = levels;
    status_.alignMeasuring = !levels;
    status_.levelsMeasuring = levels;
    status_.error.clear();
  }
  submit(/*fallbackToInit=*/false);
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
  preview_.prepare(sampleRate);
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
    const LiveSnapshot* snap = liveSlot_.current();
    engine->setParams(pv, snap && snap->generation == engine->generation() ? &snap->live : nullptr);
    if (fading_) fading_->setParams(pv, snap && snap->generation == fading_->generation() ? &snap->live : nullptr, snap ? &snap->live : nullptr);
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
      float peak = 0.0f;
      for (int i = 0; i < len; ++i) peak = std::max(peak, std::fabs(mono[i]));
      inputMeter_.push(peak);
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
  preview_.process(buffer.getArrayOfWritePointers(), numOut, n);  // a capture-browser audition replaces the rig
}

}  // namespace sawblade::plugin
