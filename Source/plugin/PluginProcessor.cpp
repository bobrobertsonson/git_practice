#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
void raiseMeter (std::atomic<float>& meter, float value)
{
    float current = meter.load();
    while (value > current && ! meter.compare_exchange_weak (current, value)) {}
}
} // namespace

SubForgeProcessor::SubForgeProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          // Logic shows this as the instrument's side chain menu: route the kick here.
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "SubForge", subforge::params::createLayout())
{
    namespace id = subforge::params;
    const auto get = [this] (const char* paramId) { return apvts.getRawParameterValue (paramId); };

    p.mode = get (id::mode);
    p.tune = get (id::tune);
    p.fine = get (id::fine);
    p.punch = get (id::punch);
    p.punchTime = get (id::punchTime);
    p.decay = get (id::decay);
    p.oneShot = get (id::oneShot);
    p.release = get (id::release);
    p.glide = get (id::glide);
    p.click = get (id::click);
    p.velocity = get (id::velocity);
    p.dropAmount = get (id::dropAmount);
    p.dropLength = get (id::dropLength);
    p.dropCurve = get (id::dropCurve);
    p.dropTime = get (id::dropTime);
    p.lowCut = get (id::lowCut);
    p.crossover = get (id::crossover);
    p.harmonics = get (id::harmonics);
    p.autoHarmonics = get (id::autoHarmonics);
    p.drive = get (id::drive);
    p.ceiling = get (id::ceiling);
    p.duckDepth = get (id::duckDepth);
    p.duckThreshold = get (id::duckThreshold);
    p.duckRelease = get (id::duckRelease);
    p.rollHold = get (id::rollHold);
    p.rollWindow = get (id::rollWindow);
    p.rollDepth = get (id::rollDepth);
    p.impactLevel = get (id::impactLevel);
    p.impactTone = get (id::impactTone);
    p.impactLength = get (id::impactLength);
    p.impactSpace = get (id::impactSpace);
    p.phoneCheck = get (id::phoneCheck);
    p.output = get (id::output);
}

bool SubForgeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    const auto in = layouts.getMainInputChannelSet();
    return in.isDisabled() || in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void SubForgeProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sidechainCopy.setSize (2, samplesPerBlock);
    engine.prepare (sampleRate);
    engine.setSettings (readSettings());
}

subforge::EngineSettings SubForgeProcessor::readSettings() const
{
    subforge::EngineSettings s;
    auto& v = s.voice;

    v.mode = p.mode->load() > 0.5f ? subforge::Mode::drop : subforge::Mode::boom;
    v.punchSemis = p.punch->load();
    v.punchMs = p.punchTime->load();
    v.decayMs = p.decay->load();
    v.oneShot = p.oneShot->load() > 0.5f;
    v.releaseMs = p.release->load();
    v.glideMs = p.glide->load();
    v.clickLevel = p.click->load();
    v.velocitySens = p.velocity->load();
    v.dropSemis = p.dropAmount->load();
    v.dropCurve = p.dropCurve->load();
    v.harmonics = p.harmonics->load();
    v.autoHarmonics = p.autoHarmonics->load();

    const int lengthIndex = juce::roundToInt (p.dropLength->load());
    if (lengthIndex == subforge::params::dropLengthFree)
        s.dropMs = p.dropTime->load();
    else
        s.dropBeats = subforge::params::dropLengthBeats[juce::jlimit (0, (int) std::size (subforge::params::dropLengthBeats) - 1, lengthIndex)];
    s.tuneSemis = p.tune->load() + p.fine->load() / 100.0f;
    s.lowCutHz = p.lowCut->load();
    s.crossoverHz = p.crossover->load();
    s.driveDb = p.drive->load();
    s.ceilingDb = p.ceiling->load();
    s.duckDepthDb = p.duckDepth->load();
    s.duckThresholdDb = p.duckThreshold->load();
    s.duckReleaseMs = p.duckRelease->load();
    s.rollHold = p.rollHold->load() > 0.5f;
    s.rollWindowMs = p.rollWindow->load();
    s.rollDepthDb = p.rollDepth->load();
    s.impact = { p.impactLevel->load(), p.impactTone->load(), p.impactLength->load(), p.impactSpace->load() };
    s.phoneCheck = p.phoneCheck->load() > 0.5f;
    s.outputDb = p.output->load();
    return s;
}

void SubForgeProcessor::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
        engine.noteOn (m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff())
        engine.noteOff (m.getNoteNumber());
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        engine.allNotesOff();
}

void SubForgeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    if (auto* head = getPlayHead())
        if (auto position = head->getPosition())
            if (auto bpm = position->getBpm())
                engine.setTempo (*bpm);

    engine.setSettings (readSettings());

    // The sidechain arrives in the same buffer the instrument writes to, so take
    // a copy before the output overwrites it.
    const float* scL = nullptr;
    const float* scR = nullptr;
    const auto input = getBusBuffer (buffer, true, 0);

    if (input.getNumChannels() > 0 && numSamples <= sidechainCopy.getNumSamples())
    {
        for (int ch = 0; ch < juce::jmin (2, input.getNumChannels()); ++ch)
            sidechainCopy.copyFrom (ch, 0, input, ch, 0, numSamples);

        scL = sidechainCopy.getReadPointer (0);
        scR = input.getNumChannels() > 1 ? sidechainCopy.getReadPointer (1) : nullptr;
    }

    auto output = getBusBuffer (buffer, false, 0);
    float* outL = output.getWritePointer (0);
    float* outR = output.getNumChannels() > 1 ? output.getWritePointer (1) : nullptr;

    // Render between MIDI events so notes land sample-accurately.
    int pos = 0;
    for (const auto meta : midi)
    {
        const int eventPos = juce::jlimit (0, numSamples, meta.samplePosition);
        if (eventPos > pos)
        {
            engine.process (outL + pos, outR != nullptr ? outR + pos : nullptr,
                            scL != nullptr ? scL + pos : nullptr, scR != nullptr ? scR + pos : nullptr, eventPos - pos);
            pos = eventPos;
        }
        handleMidi (meta.getMessage());
    }

    if (pos < numSamples)
        engine.process (outL + pos, outR != nullptr ? outR + pos : nullptr,
                        scL != nullptr ? scL + pos : nullptr, scR != nullptr ? scR + pos : nullptr, numSamples - pos);

    for (int ch = output.getNumChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    const auto meters = engine.takeMeters();
    raiseMeter (meterOutputPeakDb, (float) meters.outputPeakDb);
    raiseMeter (meterSubReductionDb, (float) meters.subReductionDb);
    raiseMeter (meterDuckReductionDb, (float) meters.duckReductionDb);
}

void SubForgeProcessor::setCurrentProgram (int index)
{
    const auto& presets = subforge::presets::all;
    if (index < 0 || index >= (int) presets.size())
        return;

    currentProgram = index;

    for (auto* param : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            ranged->setValueNotifyingHost (ranged->getDefaultValue());

    for (const auto& [paramId, value] : presets[(size_t) index].values)
        if (auto* param = apvts.getParameter (paramId))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
}

const juce::String SubForgeProcessor::getProgramName (int index)
{
    const auto& presets = subforge::presets::all;
    return index >= 0 && index < (int) presets.size() ? presets[(size_t) index].name : juce::String();
}

juce::AudioProcessorEditor* SubForgeProcessor::createEditor()
{
    return new SubForgeEditor (*this);
}

void SubForgeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void SubForgeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    apvts.replaceState (juce::ValueTree::fromXml (*xml));

    // replaceState skips parameters whose stored value already maps to the same
    // switch or menu position, which can leave the host holding an in-between
    // value (e.g. 0.76 for a switch that's on). Push every value back so the
    // host sees exactly what was saved.
    for (auto* param : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            if (auto* raw = apvts.getRawParameterValue (ranged->getParameterID()))
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (raw->load()));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SubForgeProcessor();
}
