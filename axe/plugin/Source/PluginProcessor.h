// AxeElevator plugin: wraps the DSP core (dsp/axe.h) as an AU / VST3 / LV2 effect.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
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
        std::atomic<bool> triggered{false}, hostTempo{false}, hasTarget{false}, hasMyTone{false};
        std::atomic<int> learning{0};          // axe::LearnWhat
        std::atomic<float> learnProgress{0.f};
    } meters;

    // Editor buttons: pulse a learn parameter (a rising edge starts it; the
    // Anagram sees the same parameters as toggles).
    void pulse(const char* id);

private:
    void timerCallback() override;
    axe::Params buildParams() const;

    std::unique_ptr<axe::Processor> engine[2];
    std::vector<std::atomic<float>*> raw;    // paramSpecs() order
    juce::AudioParameterBool* bypassParam = nullptr;
    std::atomic<float>* bypassRaw = nullptr;
    struct Listener : juce::AudioProcessorValueTreeState::Listener {
        AxeProcessor& p;
        explicit Listener(AxeProcessor& o) : p(o) {}
        void parameterChanged(const juce::String& id, float) override {
            // LV2: a hand-set Match band or Learn value wins over a learned one
            if (id.startsWith("match") && id != "matchAmount") p.lv2MatchOn = false;
            if (id == "pickSense" || id == "pickHold" || id == "noiseFloor" || id == "lowNote" || id == "highNote") p.lv2PlayingOn = false;
            p.dirty = true;
        }
    } listener{*this};
    std::atomic<float>* learnPlayingRaw = nullptr;
    std::atomic<float>* learnTargetRaw = nullptr;
    std::atomic<float>* targetRaw = nullptr;
    bool lastLearnPlaying = false, lastLearnTarget = false;
    // learn results crossing from the audio thread
    std::array<float, axe::kMatchBands> learnedMatch{}, lv2Match{};
    axe::PlayingProfile learnedPlay, lv2Playing;
    std::atomic<bool> matchReady{false}, playingReady{false}, targetSync{false};
    std::atomic<bool> lv2MatchOn{false}, lv2PlayingOn{false};
    // A learned profile (the Tone Match target, your tone): copied out of the
    // engine once per learn, saved with the session, put back on load.
    struct SavedProfile {
        std::array<float, axe::kMatchBands> saved{}, fromAudio{};
        std::atomic<bool> ready{false}, toEngine{false}, haveSaved{false};
        bool copied = false;   // audio thread
        void fromEngine(const float* p) { if (copied) return; for (int b = 0; b < axe::kMatchBands; b++) fromAudio[(size_t)b] = p[b]; copied = true; ready = true; }
        void collect() { if (ready.exchange(false)) { saved = fromAudio; haveSaved = true; } }
        void save(juce::ValueTree& st, const char* key) const {
            if (!haveSaved) return;
            juce::String t; for (float v : saved) t << juce::String(v, 3) << " ";
            st.setProperty(key, t.trim(), nullptr);
        }
        void load(juce::ValueTree& st, const char* key) {
            auto t = juce::StringArray::fromTokens(st.getProperty(key).toString(), " ", "");
            if (t.size() == axe::kMatchBands) {
                for (int b = 0; b < axe::kMatchBands; b++) saved[(size_t)b] = t[b].getFloatValue();
                haveSaved = true; toEngine = true; copied = true;   // (don't copy the old engine's back over it)
            }
            st.removeProperty(key, nullptr);
        }
    } tgt, mine;
    std::atomic<bool> dirty{true};
    bool isLv2 = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AxeProcessor)
};
