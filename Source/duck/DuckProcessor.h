#pragma once

#include "BassDuck.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

namespace subforge::duckparams
{
inline constexpr auto split = "split";
inline constexpr auto fullBand = "fullBand";
inline constexpr auto depth = "depth";
inline constexpr auto threshold = "threshold";
inline constexpr auto release = "release";
inline constexpr auto rollHold = "rollHold";
inline constexpr auto rollWindow = "rollWindow";
inline constexpr auto rollDepth = "rollDepth";
inline constexpr auto lookAhead = "lookAhead";
inline constexpr auto gap = "gap";

// Look-ahead choices, as note lengths in beats (index 0 = off).
inline constexpr double lookAheadBeats[] = { 0.0, 0.125, 0.25, 0.5, 1.0 };
} // namespace subforge::duckparams

// SubForge Duck: an effect for the bass. Route SubForge (or the kick) into its
// side chain and it ducks only the bass below the split frequency.
class SubForgeDuckProcessor : public juce::AudioProcessor,
                              private juce::AsyncUpdater
{
public:
    SubForgeDuckProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float> meterReductionDb { 0.0f };

private:
    subforge::BassDuckSettings readSettings() const;
    void handleAsyncUpdate() override;

    subforge::BassDuck duck;
    juce::AudioBuffer<float> keyCopy;
    double bpm = 120.0;
    std::atomic<int> wantedLatency { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubForgeDuckProcessor)
};
