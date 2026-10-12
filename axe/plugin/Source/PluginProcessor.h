// AxeElevator plugin: wraps the DSP core (dsp/axe.h) as an AU / VST3 / LV2 effect.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include "axe.h"

// One host parameter, in the units it shows (dB, Hz, ms, %): the Anagram
// can't ask a plugin for a parameter's text, so the LV2 ports carry real
// ranges. apply() converts into axe::Params.
struct ParamSpec {
    enum Kind { Float, Choice, Bool };
    const char* id;
    const char* name;
    const char* page;          // groups of up to 6 (the Anagram shows 6 per page)
    Kind kind;
    float lo, hi, def;
    const char* label;         // "dB", "Hz", "ms", "%" (become LV2 units)
    const char* choices;       // Choice: "a|b|c"
    float centre;              // Float: value at the knob's centre (0 = linear)
    void (*apply)(axe::Params&, float);
};

const std::vector<ParamSpec>& paramSpecs();

class AxeProcessor : public juce::AudioProcessor, private juce::Timer {
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
    double getTailLengthSeconds() const override { return 0.5; }

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
        std::atomic<float> gr[3] = {0.f, 0.f, 0.f}, gate{0.f};
        std::atomic<int> learning{0};          // 0 idle, 1 fresh, 2 now
        std::atomic<float> learnProgress{0.f};
        std::atomic<bool> hasFresh{false};
    } meters;

    // Editor buttons (message thread). They pulse the learn parameters, so a
    // host (or the Anagram, as a toggle) can start Learn the same way.
    void pulseLearn(const char* id);

private:
    void timerCallback() override;          // message thread: learned results -> parameters / state
    axe::Params buildParams() const;
    void applyLearnedToParams();

    std::unique_ptr<axe::Processor> engine[2];
    std::vector<std::atomic<float>*> raw;    // paramSpecs() order
    juce::AudioParameterBool* bypassParam = nullptr;
    std::atomic<float>* bypassRaw = nullptr;
    std::atomic<float>* learnFreshRaw = nullptr;
    std::atomic<float>* learnNowRaw = nullptr;
    struct Listener : juce::AudioProcessorValueTreeState::Listener {
        AxeProcessor& p;
        explicit Listener(AxeProcessor& o) : p(o) {}
        void parameterChanged(const juce::String& id, float) override;
    } listener{*this};
    std::atomic<bool> dirty{true};
    bool lastLearnFresh = false, lastLearnNow = false;
    bool isLv2 = false;

    // Learn results cross from the audio thread with these.
    std::array<float, axe::kMatchBands> learned{}, freshFromAudio{}, freshSaved{};
    std::atomic<bool> learnedReady{false}, freshReady{false}, freshToEngine{false}, haveFreshSaved{false};
    // LV2: the plugin must not move its own control ports, so a learned
    // match is applied inside the plugin until a Match port changes.
    std::array<float, axe::kMatchBands> lv2Override{};
    std::atomic<bool> useLv2Override{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AxeProcessor)
};
