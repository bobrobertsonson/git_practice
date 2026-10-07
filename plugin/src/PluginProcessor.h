#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "EditHistory.h"
#include "Engine.h"
#include "ExportSettings.h"
#include "EngineLoader.h"
#include "JobRunner.h"
#include "LevelWorker.h"
#include "LadderFetch.h"
#include "RungPreloader.h"
#include "presets/T3kTool.h"
#include "PlayAlong.h"
#include "PresetAudition.h"
#include "TakeRecorder.h"
#include "PresetMapping.h"
#include "browser/PreviewPlayer.h"
#include "pedals/CircuitParams.h"
#include "rig/InputMeter.h"
#include "sawblade/swap_slot.h"

namespace sawblade::plugin {

// The plugin's AudioProcessor: mono in (a stereo input is summed to mono), mono or dual-mono
// stereo out, wrapping sawblade::Chain through an Engine.
//
// Threading model
//   audio thread   processBlock(): reads the parameters (atomics), takes the current Engine from
//                  the SwapSlot (lock-free), processes. Never allocates, locks, does I/O.
//   loader thread  EngineLoader's worker: builds Engines for submitted requests, publishes them
//                  into the SwapSlot, destroys the ones the audio thread replaced, and reports
//                  the new latency to the host (setLatencySamples).
//   other threads  (message thread, tests) loadPreset*/get/setStateInformation/status(): guarded
//                  by mutex_, which the audio thread never touches.
// Parameter changes never rebuild anything: they only change the atomics the audio thread reads
// and the chain smooths them in place. A rebuild happens only for a preset load, a state restore,
// or prepareToPlay (new sample rate / block size). The one exception is the CIRCUIT switch
// (`sawCircuit`): it swaps the first pedal block's type through loadPreset (see circuitChanged()).
class SawbladeProcessor : public juce::AudioProcessor,
                          private juce::AudioProcessorValueTreeState::Listener,
                          private juce::Timer {
 public:
  struct Status {
    std::string presetName = "Init";
    std::string error;   // last load failure ("" if the last load succeeded)
    bool loading = false;
    int latencySamples = 0;      // host-rate samples, as reported to the host
    double hostRate = 0.0, modelRate = 0.0;
    int builtMaxBlock = 0;        // block size the running engine was built for
    bool liveCompatible = true;   // no cab or a shared cab: the no-cab export is exact (the Init preset has no cab)
    bool resampling = false;
    ChainInfo info;
    AlignResult measuredAlign;     // the last RE-MEASURE result (docs/PLUGIN.md "Rig editor")
    bool alignMeasuring = false;   // a re-measure build is in flight
    bool levelsMeasuring = false;  // a MATCH LEVELS build is in flight
    std::uint64_t generation = 0;  // of the running engine (the loader request id it was built for)
    // v0.3 level matching: LEVEL MATCH is on and the trim of the current rig is still being measured (the editor's chip then reads
    // "LEVEL ..."; until it is known the trim is 0 or the previous one). trimDb is the trim the audio thread is asked to apply.
    bool levelMatchOn = true;
    bool levelPending = false;
    bool levelFailed = false;  // the rig could not be measured (silent, a capture missing): no trim
    double trimDb = 0.0;
  };

  SawbladeProcessor();
  ~SawbladeProcessor() override;

  // --- juce::AudioProcessor ----------------------------------------------------------------
  const juce::String getName() const override { return "Sawblade"; }
  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override {}
  bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
  using juce::AudioProcessor::processBlock;
  bool supportsDoublePrecisionProcessing() const override { return false; }
  // Equal-power cross-fade between the outgoing and the new engine on a swap.
  static constexpr double kFadeSeconds = 0.030;

  double getTailLengthSeconds() const override { return 0.0; }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  bool hasEditor() const override { return true; }
  juce::AudioProcessorEditor* createEditor() override;
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return "Default"; }
  void changeProgramName(int, const juce::String&) override {}
  void getStateInformation(juce::MemoryBlock& destData) override;
  void setStateInformation(const void* data, int sizeInBytes) override;

  // --- presets ------------------------------------------------------------------------------
  // Any non-audio thread. The preset is parsed here (synchronously, cheap); the models are loaded
  // and the engine built on the loader thread. Return false (and set *error) if parsing failed;
  // load failures that only show up while building are reported through status().error.
  // `undoable`: a user's load (preset browser, file chooser, resolve): recorded as an undo step of kind Load (see "undo / redo").
  // The undoable path touches the history, which is message-thread data: `undoable = true` is for the MESSAGE THREAD only (the callers are UI
  // callbacks); every other thread passes false.
  bool loadPresetFile(const std::filesystem::path& file, std::string* error = nullptr, bool undoable = false);
  bool loadPresetJson(const std::string& json, const std::filesystem::path& baseDir, std::string* error = nullptr,
                      bool restore = false);
  // `keepMonitor`: the rig editor's structural edits keep the transient mute / solo state; user
  // loads clear it.
  // `provisionalTrimDb`: a user load (not keepMonitor) whose trim is not known yet plays at this trim until the measurement lands
  // instead of dropping to 0 (a capture swap: the rig is the same one, so the old trim is the best guess; no level jump).
  void loadPreset(Preset preset, bool keepMonitor = false, std::optional<double> provisionalTrimDb = std::nullopt);
  // Host-driven state restore: the preset and its parameter values are applied immediately (hosts
  // and validators read the parameters right after setStateInformation); the engine follows.
  void restorePreset(Preset preset);

  // The preset with the current parameter values written into it (what getStateInformation saves).
  Preset currentPreset() const;
  // levelMatchOn / trimDb / levelPending / levelFailed reflect the setting as of the last levelTick() (10 Hz) or levelMatchEnabled()
  // call, so they can lag a Settings toggle by up to 100 ms. status() deliberately does NOT re-read Settings: it is const and
  // callable from any non-audio thread, and touching Settings::shared() here would instantiate that singleton (whose load() clears
  // the core capture-cache override) in tests that set the override first and never reach a tick. Use levelMatchEnabled() when
  // the current setting matters.
  Status status() const;
  juce::AudioProcessorValueTreeState& parameters() { return apvts_; }
  // Which post-EQ slots currently control a band (for the UI).
  SlotBands postEqSlots() const;
  // The pedal circuit block the circuit parameters control in the current preset (empty: none).
  std::optional<CircuitSlot> circuitSlot() const;

  // Blocks until the loader has nothing queued or in progress (tests, prepareToPlay).
  bool waitForLoader(std::chrono::milliseconds timeout = std::chrono::milliseconds(60000));
  // The play-along backing (docs/PLUGIN.md "Play-along"): transport, settings, load status. Its setters
  // are safe from any non-audio thread.
  PlayAlong& playAlong() noexcept { return playAlong_; }
  const PlayAlong& playAlong() const noexcept { return playAlong_; }
  // The capture browser's audition: plays a rendered riff instead of the rig (docs/specs/phase8_capture_browser.md).
  PreviewPlayer& previewPlayer() noexcept { return preview_; }
  // The DI take recorder (docs/PLUGIN.md "Record + Match"): taps the input before the rig, writes WAV + sidecar
  // off the audio thread. Its control surface is safe from any non-audio thread.
  // Record + Match (message thread): the settings (application properties, not part of the tone state), the
  // match / export job runner, and the audition / A-B of match candidates. MATCH and EXPORT NAM work in the
  // Standalone app and in a host alike; a job starts only when the tool is configured (jobs().checkTools()).
  MatchSettings& matchSettings() noexcept { return matchSettings_; }
  JobRunner& jobs() noexcept { return jobs_; }
  PresetAudition& audition() noexcept { return audition_; }
  // This instance's id: the owner written into every job folder it starts (JobRunner::setOwner), so a second
  // instance (or the Standalone app) sharing the per-user jobs folder never adopts or applies this one's job. It is
  // kept in the plugin state (only once the instance has a job to recover), so a reloaded project finds its own job.
  std::string instanceId() const { return jobs_.owner(); }
  // EXPORT NAM (phase 12) is available in plugin mode and in the Standalone app alike. The panel's last settings are
  // UI state (plugin state `export`, never the preset); any non-audio thread.
  ExportSettings exportSettings() const;
  void setExportSettings(const ExportSettings& s);
  std::uint64_t exportSettingsSerial() const noexcept { return exportSerial_.load(); }
  TakeRecorder& recorder() noexcept { return recorder_; }
  const TakeRecorder& recorder() const noexcept { return recorder_; }


  // --- undo / redo (v0.3 Task D; message thread) ------------------------------------------------------------------------------
  // The history of rig edits lives HERE, not in the editor: a closed and reopened editor (Logic users close the window all the time)
  // keeps it. It is never saved (the plugin state is the preset) and a host state restore leaves it alone (its steps are whole-preset
  // snapshots, so they stay applicable). 64 steps. Recorded: every edit a USER makes: through the rig controller (blocks, EQ, cab,
  // gate, comp, alignment, topology + BLEND fill ...), through a host-parameter widget (a knob drag, a wheel step or a typed value is
  // one parameter gesture: begin .. end), a capture swap, the mic page, a preset load, an applied match. NOT recorded: host
  // automation (no gesture), a state restore, and the asynchronous completions (the BLEND fill's amp arriving, a capture swap's make-up
  // arriving, a trim / ladder write-back): they add no step of their own (patchHistory() carries the ones that belong to the rig
  // into the stored snapshots so an undo never takes them away).
  using HistoryKind = EditHistory::Kind;
  bool canUndo() const;
  bool canRedo() const;
  bool undo();  // restores the preset as it was before the last step; false when there is none
  bool redo();
  std::size_t undoSteps() const;
  std::size_t redoSteps() const;
  // Records `before` (the preset as it was before an edit that made it `after`) as one step and ends the redo branch. Edit steps are
  // restored like a rig edit (the transient mute / solo state stays), Load steps like a preset load. Undo / redo restore only the host
  // parameters that differ between `before` and `after` (a parameter the host automated since keeps its value).
  void historyRecord(Preset before, const Preset& after, HistoryKind kind = HistoryKind::Edit);
  // A gesture (a drag) is one step: the preset at its start is compared with the rig at its end. Nested gestures are one gesture (the
  // outermost start counts); `before` is the preset a step should restore (default: the rig now).
  // begin returns the token its end must pass back; an end whose gesture was aborted meanwhile (historyAbortGestures / historyClear) is ignored.
  using GestureToken = std::uint64_t;
  GestureToken historyGestureBegin();
  GestureToken historyGestureBegin(Preset before);
  void historyGestureEnd(GestureToken token);
  void historyAbortGestures();  // the editor is going away mid-drag
  bool historyInGesture() const;
  void historyClear();
  // Applies an asynchronous completion that belongs to the rig to every stored snapshot (no step).
  void patchHistory(const std::function<void(Preset&)>& f);
  // The rig controller's pending (debounced) edits: flushed before a gesture starts and before an undo / redo, so they are steps of their own.
  // `owner` identifies the setter (a controller): clearHistoryFlusher() only clears the flusher if it is still that owner's (two editors / a
  // rebuilt controller must not drop each other's).
  void setHistoryFlusher(std::function<void()> f, const void* owner);
  void clearHistoryFlusher(const void* owner);
  // A user-driven load that is one step (loadPreset() itself records nothing: BodyFill, the ladder write-back, A/B compare and the audition use it).
  // Records only when the preset differs from the rig. `kind` is how an undo restores it.
  void loadPresetUndoable(Preset preset, HistoryKind kind, bool keepMonitor = false, std::optional<double> provisionalTrimDb = std::nullopt);

  // --- rig editor hooks (message thread; docs/PLUGIN.md "Rig editor") -----------------------------------
  // The preset a structural edit starts from: the pending user load if one is in flight, else the
  // current preset (with the parameter values).
  Preset editBasePreset() const;
  // Edits the preset's live fields (EQ freq / gain / Q, block gains) in place and publishes them to
  // the audio thread without a rebuild. Must not touch parameter-mapped fields (those go through the
  // parameters) or structure. Does not touch the pending load: an edit made while a structural load is
  // in flight can be overwritten when that load commits (EQ drags are absolute and self-heal).
  void applyLiveEdit(const std::function<void(Preset&)>& edit);
  // Live dynamics policy (docs/PRESET_SCHEMA.md "Live dynamics"): selects which dynamics set (gate + bus comp) the rig plays.
  // The new mode is published to the audio thread inside one LiveSnapshot, so the whole set (gate and comp together) is adopted
  // by the engine between two blocks: no block runs half old, half new (Chain::setLiveParams applies LiveParams::dynamics as one
  // object). Also moves the GATE THRESHOLD parameter to the new active gate's value. Message thread; no rebuild.
  void setDynamicsMode(DynamicsMode m);
  DynamicsMode dynamicsMode() const;
  struct Monitor {
    bool muteA = false, muteB = false;
  };
  // Transient monitoring mutes (never saved; cleared by user loads and state restores).
  void setMonitor(bool muteA, bool muteB);
  Monitor monitor() const;
  // Re-measures the alignment with an auto-mode build and writes the result back as manual values.
  void remeasureAlignment();
  // Phase 10.1: re-measures the level trims with an auto-mode build and writes them back as
  // levelMatch.mode = manual with the measured trims.
  void matchLevels();
  const rig::InputMeter& inputMeter() const noexcept { return inputMeter_; }
  // The loader request id the current preset was committed for (== the running engine's generation
  // once it has been published).
  std::uint64_t presetGeneration() const;
  // Counts user preset loads and state restores (not the rig editor's structural edits): the editor
  // resets its transient UI state (solo, remembered blend, ...) when it changes.
  std::uint64_t userLoadSerial() const noexcept { return userLoadSerial_.load(); }

  // Test hook (call with the audio thread idle): the current engine's applied live parameters and
  // the baseline it was built with. Parameter updates must never move the first off the second.
  struct EngineParamState {
    LiveParams live, baseline;
    bool valid = false;
  };
  EngineParamState engineParamState() const;
  // Number of engines the loader has published (parameter changes must not increase it).
  std::uint64_t engineBuilds() const noexcept { return loader_->engineBuilds(); }
  // --- level matching (v0.3 Task B; docs/PRESET_SCHEMA.md "Level matching") ---------------------------------------------------
  // The LEVEL MATCH setting (Settings store, default on) is read by levelTick(). The trim of the current rig is computed on the
  // LevelWorker thread: at load when the preset's stored trim is missing or stale, and (debounced) whenever the level-affecting
  // parts of the rig change. The audio thread applies the target through an atomic; until a trim is known a user load plays at 0.
  // Called by the 10 Hz timer; tests call it directly. Message thread.
  void levelTick();
  // The rig the trim is measured for: the current preset with the parameter values. The OUTPUT knob is measured at 0 dB and is not
  // in the staleness hash: it is a persistent user offset from the target.
  Preset levelMeasurementPreset() const;
  // Blocks until the level worker is idle and no trim is waiting for its debounce (tests).
  bool waitForLevelWork(std::chrono::milliseconds timeout = std::chrono::milliseconds(60000));
  std::uint64_t levelHashComputes() const noexcept { return hashComputes_; }  // autoTrimHash evaluations by levelTick (tests)
  void setLevelDebounceMs(int ms) noexcept { levelDebounceMs_.store(ms); }
  static constexpr int kLevelDebounceMs = 400;
  // The LEVEL MATCH setting right now. Re-reads the Settings store (levelMatchOn_ is otherwise only refreshed by the 10 Hz
  // levelTick(), so a caller that acts within 100 ms of a settings change, or of construction, would see a stale default of on).
  // Message thread only; never call from the audio thread (it reads levelMatchOn_ directly).
  bool levelMatchEnabled();
  // The capture-swap make-up (core auto_trim.h slotMakeupDb) on the level worker. `done` runs on the worker thread.
  void computeSlotMakeup(Preset before, Preset after, int path, LevelWorker::MakeupDone done);
  LevelWorker& levelWorker() noexcept { return *levelWorker_; }

  // --- gain ladders (v0.2 Task B; docs/PRESET_SCHEMA.md "Gain ladder") ---------------------------------------
  // What the UI (Task D) shows for path 0 = a / 1 = b: any non-audio thread. `has` is false for a path with no ladder.
  struct LadderInfo {
    bool has = false;
    int rungCount = 0;
    int activeIndex = -1;      // the rung sounding (or being faded to)
    int targetIndex = -1;      // the rung the GAIN knob asks for
    bool pending = false;      // the target rung's model is not loaded yet: GAIN is drive-only ("rung pending")
    double activeGain = 0.0;   // the amp's gain setting of the active rung, as the pack names it
    std::string activeName, activeModelId;  // e.g. "Gain 6"
    std::string targetName, targetModelId;
    int missingRungs = 0;      // wanted rungs that are not in the capture cache (they would have to be resolved)
  };
  LadderInfo ladderInfo(int path) const;
  // The message-thread work of the ladders: applies fetched ladders, starts the next `sawblade-t3k ladder` run, writes the
  // active rung back as `gainStep` once GAIN has moved it, and keeps the rung models loaded. Called by the 10 Hz timer;
  // tests call it directly.
  void ladderTick();
  // `sawblade-t3k ladder` is run for a TONE3000 amp capture without a ladder (once per tone per session). Default on.
  void setLadderFetchEnabled(bool on) noexcept { ladderFetch_.store(on); }
  // Blocks until no ladder fetch is running and the rung loader is idle (tests); a fetched result still waits for ladderTick().
  bool waitForLadderWork(std::chrono::milliseconds timeout = std::chrono::milliseconds(20000));
  // Why a ladder is not in use (rejected rungs, an own model that is not in the fetched ladder, ...): any non-audio thread.
  std::vector<std::string> ladderMessages() const;
  std::uint64_t ladderFetches() const noexcept { return ladderFetches_.load(); }  // `ladder` tool runs started
  // v0.3 Task E: what this session has learned about the gain ladder of TONE3000 tone `toneId` (the `ladder` tool's answer): -1 = not
  // checked (or the tool failed: unknown, never guessed), 0 = checked and none, n >= 2 = n steps. Any non-audio thread. Not saved in the preset:
  // a new session asks again (once per tone).
  int ladderSteps(const std::string& toneId) const;
  // Checked and an amp of this tone has no usable ladder: the tool said none, or its ladder does not contain the capture's own model (another
  // model size). The amp head's "STEPS -" tag.
  bool ladderCheckedNone(const std::string& toneId) const;
  // The capture browser asks about the tone it shows (selected / previewed): queued for the next ladderTick(), run through the same tool and
  // the same once-per-tone-per-session rule as the preset's own fetch (so the answer is also applied to a rig that uses that capture). Message
  // thread; a no-op when the tone is known, was already asked, ladder fetching is off or network tools are disabled.
  void requestLadderLookup(const std::string& toneId);  // jumps to the front of the queue
  // The capture browser's visible rows, in display order (the caller caps them): the lookup queue becomes `toneIds` (minus the known / already
  // asked), with `priorityId` (the selected tone, "" = none) first; queued tones that are no longer wanted are dropped, a run in flight is never
  // cancelled. Message thread. A no-op once a tool failure stopped the lookups for the session.
  void setLadderLookups(const std::vector<std::string>& toneIds, const std::string& priorityId = {});
  std::vector<std::string> ladderLookupQueue() const { return {ladderLookups_.begin(), ladderLookups_.end()}; }  // tests
  // A `ladder` run failed (not logged in, no network, garbage): the browser's lookups stay off for the rest of the session (no UI text).
  bool ladderLookupsStopped() const noexcept { return ladderLookupsStopped_.load(); }
  std::uint64_t rungFetches() const noexcept { return rungFetches_.load(); }      // `fetch` runs started for missing rung models

  // Test hook: a CIRCUIT edit from another thread (or during a commit) is waiting for the timer. Commit's own
  // writes of the parameters never set it.
  bool circuitEditPending() const noexcept { return circuitDirty_.load(); }

 private:
  void syncLevelMatchSetting();  // levelMatchOn_ <- Settings (message thread); a toggle retries what could not be measured
  Preset presetWithParams() const;
  ParamValues readParams() const noexcept;
  void writeParams(const ParamValues& v);
  void submit(bool fallbackToInit);
  void commit(const Preset& p, std::uint64_t generation, bool clearMonitor);
  void publishLive();  // mutex_ held
  void dropReplacedRemeasure();  // mutex_ held
  void remeasure(bool levels);
  void onOutcome(const EngineLoader::Outcome& o);
  // CIRCUIT switch: parameterChanged() (APVTS listener, any thread) handles the change at once on the
  // message thread and otherwise flags it for the message-thread timer, so a host automating the
  // switch from the audio thread never allocates or locks here.
  void parameterChanged(const juce::String& id, float value) override;
  void timerCallback() override;
  void circuitChanged();

  void levelOnLoad(const Preset& p);                   // commit() of a user load: the new preset's trim is known, or 0
  void onTrimResult(const LevelWorker::TrimResult&);   // level worker thread
  void rememberTrim(const std::string& hash, double db);  // levelMutex_ held
  void ladderWriteBack(const std::shared_ptr<Engine>& e);
  void fetchMissingRung(const Engine& e);

  // Parameter gestures (a user's drag / wheel / typed value; never host automation) are history gestures.
  class HistoryParamListener : public juce::AudioProcessorParameter::Listener {
   public:
    explicit HistoryParamListener(SawbladeProcessor& p) : p_(p) {}
    void parameterValueChanged(int, float) override {}
    void parameterGestureChanged(int, bool starting) override;

   private:
    SawbladeProcessor& p_;
  };
  bool stepHistory(bool undo);
  void finishGesture(Preset before);

  juce::AudioProcessorValueTreeState apvts_;
  HistoryParamListener historyListener_{*this};
  mutable std::mutex historyMutex_;  // history_, historyFlusher_; never taken on the audio thread
  EditHistory history_;
  std::function<void()> historyFlusher_;
  const void* historyFlusherOwner_ = nullptr;
  std::array<GestureToken, kNumParams> paramGestureToken_{};  // message thread: the open history gesture of each host parameter
  std::array<std::atomic<float>*, kNumParams> paramAtomic_{};
  std::array<juce::RangedAudioParameter*, kNumParams> paramObj_{};

  mutable std::mutex mutex_;  // guards preset_, status_, lastSubmitted_; never taken on the audio thread
  Preset preset_;
  Status status_;
  std::uint64_t lastSubmitted_ = 0;
  std::shared_ptr<const Preset> wanted_;  // latest user-requested preset not yet committed
  bool wantedKeepsMonitor_ = false;
  std::shared_ptr<const Preset> remeasureBase_;    // the preset a pending re-measure started from
  std::uint64_t presetSerial_ = 0;                 // bumped by every commit()
  std::uint64_t presetRev_ = 0;                    // bumped by every change of preset_ (commit, live edit, write-backs): the level hash cache key
  bool remeasureLevels_ = false;                   // the pending re-measure is MATCH LEVELS (else RE-MEASURE)
  std::shared_ptr<const Preset> remeasureWanted_;  // the pending re-measure build, if any
  std::uint64_t presetGeneration_ = 0;     // loader request id of the committed preset (kNoGeneration: none yet)
  Monitor monitor_;
  std::weak_ptr<Engine> published_;  // the latest published engine, for message-thread readers (mutex_)
  std::atomic<bool> ladderFetch_{true};
  std::atomic<std::uint64_t> ladderFetches_{0}, rungFetches_{0};
  std::atomic<bool> rungArrived_{false};  // a rung `fetch` finished: ask the rung loader at once
  std::set<std::string> rungTried_;       // "tone:model" already fetched (or failed) this session (message thread only)
  std::set<std::string> ladderTried_;  // tone ids already asked about (message thread only)
  std::atomic<bool> ladderLookupsStopped_{false};
  std::deque<std::string> ladderLookups_;  // tone ids the capture browser wants checked (message thread only)
  std::map<std::string, int> ladderSteps_;  // fetchMutex_: tone id -> steps (0 = none), from the tool's answers
  std::set<std::string> ladderUnusable_;    // fetchMutex_: a ladder exists but no amp block could take it
  std::map<std::string, std::vector<LadderRung>> ladderRungs_;  // fetchMutex_: the ladders found this session, by tone id
  std::set<std::string> ladderNoted_;       // message thread: "tone:model" captures already reported as not in their tone's ladder
  mutable std::mutex fetchMutex_;
  std::vector<std::string> ladderNotes_;  // fetchMutex_
  std::vector<LadderFetchResult> fetched_;  // results waiting for the message thread
  std::atomic<bool> fetchRunning_{false};
  std::uint64_t lastRungKey_ = ~0ull;  // message thread: what the rung loader was last asked for
  int rungTicks_ = 0;
  std::atomic<std::uint64_t> userLoadSerial_{0};
  rig::InputMeter inputMeter_;
  std::atomic<bool> circuitDirty_{false};
  std::atomic<int> commitCircuit_{0};  // the sawCircuit value commit() / the write-back is writing
  std::atomic<int> committing_{0};  // >0 while commit() writes the parameters: those writes are not user edits

  double hostRate_ = 0.0;
  int maxBlock_ = 0;
  std::vector<float> mono_, backL_, backR_;  // rig mono, backing L / R (audio-thread scratch)
  PlayAlong playAlong_;
  mutable std::mutex exportMutex_;  // exportSettings_; never taken on the audio thread
  ExportSettings exportSettings_;
  bool instanceRestored_ = false;  // the id came from a saved state: keep saving it
  std::atomic<std::uint64_t> exportSerial_{0};
  PreviewPlayer preview_;
  TakeRecorder recorder_;
  MatchSettings matchSettings_;
  JobRunner jobs_;
  PresetAudition audition_;

  // Audio-thread state: the engine in use and, for kFadeSeconds after a swap, the outgoing one.
  // Neither reference is ever the last one (the loader keeps its own), so dropping them on the
  // audio thread frees nothing.
  std::shared_ptr<Engine> cur_, fading_;
  int fadePos_ = 0, fadeLen_ = 0;
  std::vector<float> fadeBuf_;

  // Live values of the preset for the engine of the same generation. Producers: the message thread
  // (live edits, monitor) and the loader thread (onOutcome); both publish only with mutex_ held, which
  // serialises them and so satisfies SwapSlot's single-producer contract. Consumer: the audio thread.
  // Level matching. trimTargetDb_ / levelMatchOn_ are what the audio thread reads; the rest is guarded by levelMutex_ (never taken
  // on the audio thread).
  std::atomic<double> trimTargetDb_{0.0};
  std::atomic<bool> levelMatchOn_{true};
  std::atomic<int> levelDebounceMs_{kLevelDebounceMs};
  mutable std::mutex levelMutex_;
  std::vector<std::pair<std::string, double>> knownTrims_;  // hash -> trim, newest last, at most 32
  std::set<std::string> failedTrims_;  // cleared on a user load and on a LEVEL MATCH toggle: a failure is not remembered for ever
  std::optional<double> provisionalTrim_;  // levelMutex_: see loadPreset(); consumed by the next levelOnLoad
  // levelTick()'s autoTrimHash cache (message thread): valid for (presetRev_, the parameter values).
  bool hashCached_ = false;
  std::uint64_t hashRev_ = 0;
  ParamValues hashParams_{};
  std::string hashValue_;
  std::uint64_t hashComputes_ = 0;
  std::string levelWantedHash_, levelPendingHash_;
  std::chrono::steady_clock::time_point levelChangedAt_{};
  bool levelPending_ = false, levelFailed_ = false;
  std::unique_ptr<LevelWorker> levelWorker_;  // joined first in the destructor: its callbacks use this object

  SwapSlot<LiveSnapshot> liveSlot_;
  SwapSlot<EngineRef> slot_;                  // declared before loader_: the loader is destroyed first
  std::unique_ptr<EngineLoader> loader_;
  RungPreloader rungs_;
  T3kTool ladderTool_;
};

}  // namespace sawblade::plugin
