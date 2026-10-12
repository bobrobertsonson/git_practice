// RASP plugin: wraps the pedal's DSP core (dsp/rasp.h) as an AU/VST3 effect.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <vector>
#include "rasp.h"

// One host parameter, mapped onto a field of rasp::Params.
struct ParamSpec {
    enum Kind { Unit, Db, Bool, Choice, Bpm };
    const char* id;
    const char* name;
    const char* section;       // "Control", "Thicken", "Flavour", "Bus", "Live"
    Kind kind;
    float lo, hi;              // Db and Bpm ranges (Unit is 0..1)
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
        std::atomic<bool> hostTempo{false};
    } meters;
    void requestClearNotches() { clearNotches = true; }

private:
    void parameterChanged(const juce::String&, float) override { dirty = true; }
    rasp::Params buildParams() const;

    std::unique_ptr<rasp::Processor> engine;
    std::vector<std::atomic<float>*> raw;        // paramSpecs() order
    std::atomic<float>* syncParam = nullptr;
    std::atomic<bool> dirty{true}, clearNotches{false};
    int currentProgram = 0;
    float lastBpm = 0.f;
    bool lastMonoOut = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RaspProcessor)
};
