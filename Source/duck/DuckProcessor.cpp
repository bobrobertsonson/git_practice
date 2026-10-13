#include "DuckProcessor.h"
#include "DuckEditor.h"

namespace
{
juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    namespace id = subforge::duckparams;
    AudioProcessorValueTreeState::ParameterLayout layout;

    const auto range = [] (float lo, float hi, float centre)
    {
        NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    };

    const auto unit = [] (const char* suffix, int decimals)
    {
        return AudioParameterFloatAttributes().withLabel (suffix).withStringFromValueFunction (
            [suffix, decimals] (float v, int) { return String (v, decimals) + " " + suffix; });
    };

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id::split, 1 }, "Split", range (40.0f, 250.0f, 100.0f), 100.0f, unit ("Hz", 0)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { id::fullBand, 1 }, "Full Band", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id::depth, 1 }, "Depth", NormalisableRange<float> (0.0f, 24.0f), 8.0f, unit ("dB", 1)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id::threshold, 1 }, "Threshold", NormalisableRange<float> (-48.0f, 0.0f), -30.0f, unit ("dB", 1)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id::release, 1 }, "Release", range (20.0f, 500.0f, 120.0f), 120.0f, unit ("ms", 0)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { id::rollHold, 1 }, "Roll Hold", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id::rollWindow, 1 }, "Roll Window", range (60.0f, 250.0f, 140.0f), 150.0f, unit ("ms", 0)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id::rollDepth, 1 }, "Roll Depth", NormalisableRange<float> (0.0f, 12.0f), 2.0f, unit ("dB", 1)));
    return layout;
}

void raiseMeter (std::atomic<float>& meter, float value)
{
    float current = meter.load();
    while (value > current && ! meter.compare_exchange_weak (current, value)) {}
}
} // namespace

SubForgeDuckProcessor::SubForgeDuckProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          // Logic shows this as the Side Chain menu: pick the SubForge track.
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SubForgeDuck", createLayout())
{
}

bool SubForgeDuckProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto main = layouts.getMainOutputChannelSet();
    if (main != juce::AudioChannelSet::mono() && main != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != main)
        return false;

    const auto key = layouts.getChannelSet (true, 1);
    return key.isDisabled() || key == juce::AudioChannelSet::mono() || key == juce::AudioChannelSet::stereo();
}

subforge::BassDuckSettings SubForgeDuckProcessor::readSettings() const
{
    namespace id = subforge::duckparams;
    const auto get = [this] (const char* paramId) { return (double) apvts.getRawParameterValue (paramId)->load(); };

    subforge::BassDuckSettings s;
    s.splitHz = get (id::split);
    s.fullBand = get (id::fullBand) > 0.5;
    s.duck.depthDb = get (id::depth);
    s.duck.thresholdDb = get (id::threshold);
    s.duck.releaseMs = get (id::release);
    s.duck.rollHold = get (id::rollHold) > 0.5;
    s.duck.rollWindowMs = get (id::rollWindow);
    s.duck.rollDepthDb = get (id::rollDepth);
    return s;
}

void SubForgeDuckProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    keyCopy.setSize (2, samplesPerBlock);
    duck.prepare (sampleRate);
    duck.setSettings (readSettings());
}

void SubForgeDuckProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    duck.setSettings (readSettings());

    // The key arrives in the same buffer the output is written to, so copy it first.
    const float* keyL = nullptr;
    const float* keyR = nullptr;

    if (getBusCount (true) > 1)
    {
        const auto key = getBusBuffer (buffer, true, 1);
        if (key.getNumChannels() > 0 && numSamples <= keyCopy.getNumSamples())
        {
            for (int ch = 0; ch < juce::jmin (2, key.getNumChannels()); ++ch)
                keyCopy.copyFrom (ch, 0, key, ch, 0, numSamples);

            keyL = keyCopy.getReadPointer (0);
            keyR = key.getNumChannels() > 1 ? keyCopy.getReadPointer (1) : nullptr;
        }
    }

    auto main = getBusBuffer (buffer, false, 0);
    float* left = main.getWritePointer (0);
    float* right = main.getNumChannels() > 1 ? main.getWritePointer (1) : nullptr;
    duck.process (left, right, keyL, keyR, numSamples);

    raiseMeter (meterReductionDb, (float) duck.takeReductionDb());
}

juce::AudioProcessorEditor* SubForgeDuckProcessor::createEditor()
{
    return new SubForgeDuckEditor (*this);
}

void SubForgeDuckProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void SubForgeDuckProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    apvts.replaceState (juce::ValueTree::fromXml (*xml));

    // Push every value back so switches restore to exactly on/off (see SubForge).
    for (auto* param : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            if (auto* raw = apvts.getRawParameterValue (ranged->getParameterID()))
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (raw->load()));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SubForgeDuckProcessor();
}
