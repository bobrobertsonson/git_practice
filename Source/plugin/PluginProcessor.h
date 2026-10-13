#pragma once

#include "Parameters.h"
#include "SubEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

class SubForgeProcessor : public juce::AudioProcessor
{
public:
    SubForgeProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 12.0; }

    int getNumPrograms() override { return (int) subforge::presets::all.size(); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Peak-hold meters for the editor. The audio thread raises them, the editor
    // reads and clears them with exchange().
    std::atomic<float> meterOutputPeakDb { -180.0f };
    std::atomic<float> meterSubReductionDb { 0.0f };
    std::atomic<float> meterDuckReductionDb { 0.0f };

private:
    subforge::EngineSettings readSettings() const;
    void handleMidi (const juce::MidiMessage&);

    subforge::SubEngine engine;
    int currentProgram = 0;
    juce::AudioBuffer<float> sidechainCopy;

    struct
    {
        std::atomic<float>* mode;
        std::atomic<float>* tune;
        std::atomic<float>* fine;
        std::atomic<float>* punch;
        std::atomic<float>* punchTime;
        std::atomic<float>* decay;
        std::atomic<float>* oneShot;
        std::atomic<float>* release;
        std::atomic<float>* glide;
        std::atomic<float>* click;
        std::atomic<float>* velocity;
        std::atomic<float>* dropAmount;
        std::atomic<float>* dropLength;
        std::atomic<float>* dropCurve;
        std::atomic<float>* lowCut;
        std::atomic<float>* crossover;
        std::atomic<float>* harmonics;
        std::atomic<float>* autoHarmonics;
        std::atomic<float>* drive;
        std::atomic<float>* ceiling;
        std::atomic<float>* duckDepth;
        std::atomic<float>* duckThreshold;
        std::atomic<float>* duckRelease;
        std::atomic<float>* rollHold;
        std::atomic<float>* rollWindow;
        std::atomic<float>* rollDepth;
        std::atomic<float>* impactLevel;
        std::atomic<float>* impactTone;
        std::atomic<float>* impactLength;
        std::atomic<float>* impactSpace;
        std::atomic<float>* phoneCheck;
        std::atomic<float>* output;
    } p {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubForgeProcessor)
};
