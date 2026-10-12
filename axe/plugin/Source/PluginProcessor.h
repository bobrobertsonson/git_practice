// AxeElevator plugin: wraps the DSP core (dsp/axe.h) as an AU / VST3 / LV2 effect.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <vector>
#include "axe.h"

// One host parameter, in the units it shows (dB, Hz, ms, %, beats): the
// Anagram can't ask a plugin for a parameter's text, so the LV2 ports carry
// real ranges. apply() converts into axe::Params.
struct ParamSpec {
    enum Kind { Float, Choice, Bool };
    const char* id;
    const char* name;
    const char* page;          // groups of exactly 6 (the Anagram shows 6 per page)
    int mode;                  // the mode this page belongs to, -1 = global
    Kind kind;
    float lo, hi, def;
    const char* label;         // "dB", "Hz", "ms", "%" become LV2 units; others are shown as text
    const char* choices;       // Choice: "a|b|c"
    float centre;              // Float: value at the knob's centre (0 = linear)
    void (*apply)(axe::Params&, float);
};

const std::vector<ParamSpec>& paramSpecs();

class AxeProcessor : public juce::AudioProcessor {
public:
    AxeProcessor();
    ~AxeProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;     // LV2 kx:Reset lands here
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "AxeElevator"; }
    juce::StringArray getAlternateDisplayNames() const override { return {"AXE", "AxeElevator"}; }   // first: Anagram dg:abbreviation
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }   // freeze / feedback releases

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    juce::AudioProcessorValueTreeState apvts;

    struct Meters {
        std::atomic<float> gr[3] = {0.f, 0.f, 0.f}, gate{0.f}, bpm{120.f}, pitch{0.f};
        std::atomic<int> step{0};
        std::atomic<bool> triggered{false}, hostTempo{false};
    } meters;

private:
    axe::Params buildParams() const;

    std::unique_ptr<axe::Processor> engine[2];
    std::vector<std::atomic<float>*> raw;    // paramSpecs() order
    juce::AudioParameterBool* bypassParam = nullptr;
    std::atomic<float>* bypassRaw = nullptr;
    struct Listener : juce::AudioProcessorValueTreeState::Listener {
        AxeProcessor& p;
        explicit Listener(AxeProcessor& o) : p(o) {}
        void parameterChanged(const juce::String&, float) override { p.dirty = true; }
    } listener{*this};
    std::atomic<bool> dirty{true};
    bool isLv2 = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AxeProcessor)
};
