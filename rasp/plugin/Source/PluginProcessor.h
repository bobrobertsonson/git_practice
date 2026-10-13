// RASP plugin: wraps the pedal's DSP core (dsp/rasp.h) as an AU/VST3 effect.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <vector>
#include "rasp.h"

// One host parameter, mapped onto a field of rasp::Params.
struct ParamSpec {
    enum Kind { Unit, Db, Bool, Choice, Bpm, Hz };
    const char* id;
    const char* name;
    const char* section;       // "Control", "Thicken", "Harmony", "Flavour", "Bus", "Live", "Momentary"
    Kind kind;
    float lo, hi;              // Db, Bpm and Hz ranges (Unit is 0..1)
    const char* choices;       // Choice: "a|b|c"
    float (*get)(const rasp::Params&);
    void (*set)(rasp::Params&, float);
    juce::String (*display)(float);   // Unit display; nullptr = percent
    bool keepOnPreset;         // live/setup settings survive a preset change
};

const std::vector<ParamSpec>& paramSpecs();

class RaspProcessor : public juce::AudioProcessor,
                      private juce::AudioProcessorValueTreeState::Listener {
public:
    RaspProcessor();
    ~RaspProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "RASP"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }   // 6 s plate + delay repeats

    // Factory presets appear in Logic's preset menu.
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Logic's bypass button drives the engine's level-matched, click-free bypass.
    juce::AudioProcessorParameter* getBypassParameter() const override;

    juce::AudioProcessorValueTreeState apvts;

    // Read by the editor.
    struct Meters {
        std::atomic<float> chopGr{0}, limiter{0}, bleed{0}, bypassMatch{0}, bpm{120};
        std::atomic<int> notches{0};
        std::atomic<bool> hostTempo{false}, learning{false};
        std::atomic<float> learnProgress{0};
        std::atomic<float> inPeak{-100.f}, inRms{-100.f}, plosive{0.f};
        std::atomic<int> character{0};
    } meters;
    void requestClearNotches() { clearNotches = true; }
    std::atomic<float> uiScale{0.8f};   // editor size, saved with the session
    // Learn: the audio thread starts it and hands back the gains; applyLearned()
    // (message thread) writes them into the match parameters so they save.
    void requestLearn() { learnCmd = 1; }
    bool applyLearned() {
        if (!learnDone.exchange(false)) return false;
        for (int i = 0; i < rasp::kMatchBands; i++)
            if (auto* prm = apvts.getParameter("match" + juce::String(i))) {
                prm->beginChangeGesture(); prm->setValueNotifyingHost(prm->convertTo0to1(learned[i])); prm->endChangeGesture();
            }
        if (auto* amt = apvts.getParameter("matchAmount")) {
            amt->beginChangeGesture(); amt->setValueNotifyingHost(1.f); amt->endChangeGesture();
        }
        if (auto* on = apvts.getParameter("matchOn")) {
            on->beginChangeGesture(); on->setValueNotifyingHost(1.f); on->endChangeGesture();
        }
        return true;
    }

private:
    void parameterChanged(const juce::String&, float) override { dirty = true; }
    rasp::Params buildParams() const;

    std::unique_ptr<rasp::Processor> engine;
    std::vector<std::atomic<float>*> raw;        // paramSpecs() order
    std::atomic<float>* syncParam = nullptr;
    std::atomic<bool> dirty{true}, clearNotches{false}, learnDone{false};
    std::atomic<int> learnCmd{0};
    float learned[rasp::kMatchBands] = {};
    float inMs = 0.f;
    int currentProgram = 0;
    float lastBpm = 0.f;
    bool lastMonoOut = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RaspProcessor)
};
