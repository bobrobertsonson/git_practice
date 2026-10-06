#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "Engine.h"
#include "ExportSettings.h"
#include "EngineLoader.h"
#include "JobRunner.h"
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
  bool loadPresetFile(const std::filesystem::path& file, std::string* error = nullptr);
  bool loadPresetJson(const std::string& json, const std::filesystem::path& baseDir, std::string* error = nullptr,
                      bool restore = false);
  // `keepMonitor`: the rig editor's structural edits keep the transient mute / solo state; user
  // loads clear it.
  void loadPreset(Preset preset, bool keepMonitor = false);
  // Host-driven state restore: the preset and its parameter values are applied immediately (hosts
  // and validators read the parameters right after setStateInformation); the engine follows.
  void restorePreset(Preset preset);

  // The preset with the current parameter values written into it (what getStateInformation saves).
  Preset currentPreset() const;
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


  // --- rig editor hooks (message thread; docs/PLUGIN.md "Rig editor") -----------------------------------
  // The preset a structural edit starts from: the pending user load if one is in flight, else the
  // current preset (with the parameter values).
  Preset editBasePreset() const;
  // Edits the preset's live fields (EQ freq / gain / Q, block gains) in place and publishes them to
  // the audio thread without a rebuild. Must not touch parameter-mapped fields (those go through the
  // parameters) or structure. Does not touch the pending load: an edit made while a structural load is
  // in flight can be overwritten when that load commits (EQ drags are absolute and self-heal).
  void applyLiveEdit(const std::function<void(Preset&)>& edit);
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
  // Test hook: a CIRCUIT edit from another thread (or during a commit) is waiting for the timer. Commit's own
  // writes of the parameters never set it.
  bool circuitEditPending() const noexcept { return circuitDirty_.load(); }

 private:
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

  juce::AudioProcessorValueTreeState apvts_;
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
  bool remeasureLevels_ = false;                   // the pending re-measure is MATCH LEVELS (else RE-MEASURE)
  std::shared_ptr<const Preset> remeasureWanted_;  // the pending re-measure build, if any
  std::uint64_t presetGeneration_ = 0;     // loader request id of the committed preset (kNoGeneration: none yet)
  Monitor monitor_;
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
  SwapSlot<LiveSnapshot> liveSlot_;
  SwapSlot<EngineRef> slot_;                  // declared before loader_: the loader is destroyed first
  std::unique_ptr<EngineLoader> loader_;
};

}  // namespace sawblade::plugin
