#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <cstring>
#include <optional>

namespace {
float pct(float v) { return v * 0.01f; }
float invExp(float v, float lo, float hi) { return std::log(v / lo) / std::log(hi / lo); }
}  // namespace

#define AXE_SET(expr) [](axe::Params& p, float v) { expr; }

// Parameter order = LV2 port order = the Anagram's pages of 6.
const std::vector<ParamSpec>& paramSpecs() {
    using K = ParamSpec;
    static const std::vector<ParamSpec> specs = {
        // page 1: global
        {"mode", "Mode", "Global", K::Choice, 0, 3, 0, "", "Tight|Sub|New Strings|Buzzsaw", 0, AXE_SET(p.mode = (int)std::lround(v))},
        {"instrument", "Instrument", "Global", K::Choice, 0, 1, 0, "", "Guitar|Bass", 0, AXE_SET(p.instrument = (int)std::lround(v))},
        {"input", "Input", "Global", K::Float, -12, 12, 0, "dB", nullptr, 0, AXE_SET(p.inDb = v)},
        {"gate", "Gate", "Global", K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.gate = pct(v))},
        {"mix", "Mix", "Global", K::Float, 0, 100, 100, "%", nullptr, 0, AXE_SET(p.mix = pct(v))},
        {"output", "Output", "Global", K::Float, -24, 12, 0, "dB", nullptr, 0, AXE_SET(p.outDb = v)},
        // page 2: Tight
        {"tight", "Tight", "Tight", K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.tight = pct(v))},
        {"punch", "Punch", "Tight", K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.punch = pct(v))},
        {"release", "Release", "Tight", K::Float, 30, 300, 75, "ms", nullptr, 95, AXE_SET(p.release = invExp(v, 30.f, 300.f))},
        {"low", "Low", "Tight", K::Float, -9, 9, 0, "dB", nullptr, 0, AXE_SET(p.lowDb = v)},
        {"mid", "Mid", "Tight", K::Float, -9, 9, 0, "dB", nullptr, 0, AXE_SET(p.midDb = v)},
        {"high", "High", "Tight", K::Float, -9, 9, 0, "dB", nullptr, 0, AXE_SET(p.highDb = v)},
        // page 3: Sub
        {"subType", "Sub Type", "Sub", K::Choice, 0, 1, 0, "", "Poly (chords)|Mono (single notes)", 0, AXE_SET(p.subType = (int)std::lround(v))},
        {"sub", "Sub", "Sub", K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.sub = pct(v))},
        {"dry", "Dry", "Sub", K::Float, 0, 100, 100, "%", nullptr, 0, AXE_SET(p.dry = pct(v))},
        {"subTone", "Sub Tone", "Sub", K::Float, 80, 1600, 358, "Hz", nullptr, 358, AXE_SET(p.subTone = invExp(v, 80.f, 1600.f))},
        {"range", "Range", "Sub", K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.range = pct(v))},
        {"grit", "Grit", "Sub", K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.grit = pct(v))},
        // page 4: New Strings
        {"bright", "Bright", "New Strings", K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.bright = pct(v))},
        {"clarity", "Clarity", "New Strings", K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.clarity = pct(v))},
        {"air", "Air", "New Strings", K::Float, 0, 9, 2, "dB", nullptr, 0, AXE_SET(p.airDb = v)},
        {"matchAmount", "Match", "New Strings", K::Float, 0, 100, 100, "%", nullptr, 0, AXE_SET(p.matchAmount = pct(v))},
        {"learnFresh", "Learn Fresh", "New Strings", K::Bool, 0, 1, 0, "", nullptr, 0, AXE_SET((void)p; (void)v)},
        {"learnNow", "Learn Now", "New Strings", K::Bool, 0, 1, 0, "", nullptr, 0, AXE_SET((void)p; (void)v)},
        // page 5: Buzzsaw
        {"gain", "Gain", "Buzzsaw", K::Float, 0, 100, 65, "%", nullptr, 0, AXE_SET(p.gain = pct(v))},
        {"lows", "Lows", "Buzzsaw", K::Float, 0, 15, 10.5f, "dB", nullptr, 0, AXE_SET(p.lows = v / 15.f)},
        {"highs", "Highs", "Buzzsaw", K::Float, 0, 15, 10.5f, "dB", nullptr, 0, AXE_SET(p.highs = v / 15.f)},
        {"scoop", "Scoop", "Buzzsaw", K::Float, 0, 12, 6, "dB", nullptr, 0, AXE_SET(p.scoop = v / 12.f)},
        {"level", "Level", "Buzzsaw", K::Float, -18, 12, 0, "dB", nullptr, 0, AXE_SET(p.level = v)},
        {"blend", "Blend", "Buzzsaw", K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.blend = pct(v))},
        // pages 6-7: the learned match EQ (Learn fills these; also usable by hand)
        {"match0", "Match 250 Hz", "Match", K::Float, -12, 12, 0, "dB", nullptr, 0, AXE_SET(p.match[0] = v)},
        {"match1", "Match 500 Hz", "Match", K::Float, -12, 12, 0, "dB", nullptr, 0, AXE_SET(p.match[1] = v)},
        {"match2", "Match 1 kHz", "Match", K::Float, -12, 12, 0, "dB", nullptr, 0, AXE_SET(p.match[2] = v)},
        {"match3", "Match 1.6 kHz", "Match", K::Float, -12, 12, 0, "dB", nullptr, 0, AXE_SET(p.match[3] = v)},
        {"match4", "Match 2.5 kHz", "Match", K::Float, -12, 12, 0, "dB", nullptr, 0, AXE_SET(p.match[4] = v)},
        {"match5", "Match 4 kHz", "Match", K::Float, -12, 12, 0, "dB", nullptr, 0, AXE_SET(p.match[5] = v)},
        {"match6", "Match 6.3 kHz", "Match 2", K::Float, -12, 12, 0, "dB", nullptr, 0, AXE_SET(p.match[6] = v)},
        {"match7", "Match 10 kHz", "Match 2", K::Float, -12, 12, 0, "dB", nullptr, 0, AXE_SET(p.match[7] = v)},
    };
    return specs;
}

static juce::String fmtValue(float v, const char* label) {
    juce::String l(label);
    if (l == "dB") return (v > 0.f ? "+" : "") + juce::String(v, 1) + " dB";
    if (l == "Hz") return v >= 1000.f ? juce::String(v / 1000.f, 2) + " kHz" : juce::String(juce::roundToInt(v)) + " Hz";
    if (l == "ms") return juce::String(juce::roundToInt(v)) + " ms";
    if (l == "%") return juce::String(juce::roundToInt(v)) + "%";
    return juce::String(v, 2);
}

static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& s : paramSpecs()) {
        const juce::ParameterID pid{s.id, 1};
        switch (s.kind) {
            case ParamSpec::Bool:
                layout.add(std::make_unique<juce::AudioParameterBool>(pid, s.name, s.def > 0.5f));
                break;
            case ParamSpec::Choice:
                layout.add(std::make_unique<juce::AudioParameterChoice>(
                    pid, s.name, juce::StringArray::fromTokens(s.choices, "|", ""), (int)s.def));
                break;
            case ParamSpec::Float: {
                juce::NormalisableRange<float> r(s.lo, s.hi);
                if (s.centre > 0.f) r.setSkewForCentre(s.centre);
                const char* lab = s.label;
                layout.add(std::make_unique<juce::AudioParameterFloat>(
                    pid, s.name, r, s.def,
                    juce::AudioParameterFloatAttributes().withLabel(lab).withStringFromValueFunction(
                        [lab](float v, int) { return fmtValue(v, lab); })));
                break;
            }
        }
    }
    // Host bypass. In LV2 it becomes the lv2:enabled port (inverted, default on).
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"bypass", 1}, "Bypass", false));
    return layout;
}

AxeProcessor::AxeProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::mono(), true)
                         .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "AXE", makeLayout()) {
    isLv2 = wrapperType == wrapperType_LV2;
    for (const auto& s : paramSpecs()) {
        raw.push_back(apvts.getRawParameterValue(s.id));
        apvts.addParameterListener(s.id, &listener);
    }
    bypassParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("bypass"));
    bypassRaw = apvts.getRawParameterValue("bypass");
    learnFreshRaw = apvts.getRawParameterValue("learnFresh");
    learnNowRaw = apvts.getRawParameterValue("learnNow");
    apvts.addParameterListener("bypass", &listener);
    // Learn results reach the parameters through the message thread. LV2 has
    // no message loop to rely on (and must not move its own ports).
    if (!isLv2) startTimerHz(10);
}

AxeProcessor::~AxeProcessor() {
    stopTimer();
    for (const auto& s : paramSpecs()) apvts.removeParameterListener(s.id, &listener);
    apvts.removeParameterListener("bypass", &listener);
}

void AxeProcessor::Listener::parameterChanged(const juce::String& id, float) {
    if (id.startsWith("match") && id != "matchAmount") p.useLv2Override = false;   // a hand-set Match band wins
    p.dirty = true;
}

bool AxeProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
    if (in == mono && (out == mono || out == stereo)) return true;   // a DI: mono, optionally copied to both sides
    return in == stereo && out == stereo;                            // dual mono
}

void AxeProcessor::prepareToPlay(double sampleRate, int) {
    // A fresh engine per prepare (allocation is fine here: message thread).
    for (auto& e : engine) {
        e = std::make_unique<axe::Processor>();
        e->init((float)sampleRate);
        e->setParams(buildParams());
        e->reset();
    }
    if (haveFreshSaved) freshToEngine = true;
    dirty = true;
}

void AxeProcessor::reset() {
    for (auto& e : engine) if (e) e->reset();
}

axe::Params AxeProcessor::buildParams() const {
    axe::Params p;
    const auto& specs = paramSpecs();
    for (size_t i = 0; i < specs.size(); i++) specs[i].apply(p, raw[i]->load());
    p.bypass = bypassRaw->load() > 0.5f;
    if (useLv2Override) for (int b = 0; b < axe::kMatchBands; b++) p.match[b] = lv2Override[(size_t)b];
    return p;
}

void AxeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    // The Anagram host owns the CPU flags (Darkglass REQUIREMENTS.md); in
    // the other hosts JUCE's usual guard keeps denormals away.
    std::optional<juce::ScopedNoDenormals> noDenormals;
    if (!isLv2) noDenormals.emplace();
    const int n = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels(), numOut = getTotalNumOutputChannels();
    if (!engine[0] || numOut < 1) { buffer.clear(); return; }

    // Learn: rising edges of the two learn parameters (buttons in the editor,
    // toggles on the Anagram).
    const bool lf = learnFreshRaw->load() > 0.5f;
    const bool ln = learnNowRaw->load() > 0.5f;
    if (lf && !lastLearnFresh) engine[0]->learnFresh();
    if (ln && !lastLearnNow) engine[0]->learnNow();
    lastLearnFresh = lf; lastLearnNow = ln;
    if (freshToEngine.exchange(false)) engine[0]->setFreshProfile(freshSaved.data());

    const bool wasFresh = engine[0]->hasFresh();
    float g[axe::kMatchBands];
    if (engine[0]->learnedMatch(g)) {
        for (int b = 0; b < axe::kMatchBands; b++) learned[(size_t)b] = g[b];
        if (isLv2) {
            for (int b = 0; b < axe::kMatchBands; b++) lv2Override[(size_t)b] = g[b];
            useLv2Override = true;
            dirty = true;
        } else {
            learnedReady = true;     // the timer writes the Match parameters
        }
    }

    if (dirty.exchange(false)) {
        const axe::Params p = buildParams();
        for (auto& e : engine) e->setParams(p);
    }

    const float* inL = buffer.getReadPointer(0);
    float* outL = buffer.getWritePointer(0);
    if (numIn >= 2 && numOut >= 2) {
        engine[0]->process(inL, outL, n);
        engine[1]->process(buffer.getReadPointer(1), buffer.getWritePointer(1), n);
    } else {
        engine[0]->process(inL, outL, n);
        if (numOut >= 2) buffer.copyFrom(1, 0, outL, n);
    }
    for (int ch = std::max(numIn, numOut); ch < buffer.getNumChannels(); ch++) buffer.clear(ch, 0, n);

    if (!wasFresh && engine[0]->hasFresh()) {
        const float* f = engine[0]->freshProfile();
        for (int b = 0; b < axe::kMatchBands; b++) freshFromAudio[(size_t)b] = f[b];
        freshReady = true;
    }
    for (int b = 0; b < 3; b++) meters.gr[b] = engine[0]->gainReductionDb(b);
    meters.gate = engine[0]->gateReductionDb();
    meters.learning = engine[0]->learningWhat();
    meters.learnProgress = engine[0]->learnProgress();
    meters.hasFresh = engine[0]->hasFresh();
}

void AxeProcessor::timerCallback() {
    if (freshReady.exchange(false)) { freshSaved = freshFromAudio; haveFreshSaved = true; }
    if (learnedReady.exchange(false)) applyLearnedToParams();
}

void AxeProcessor::applyLearnedToParams() {
    for (int b = 0; b < axe::kMatchBands; b++)
        if (auto* prm = apvts.getParameter("match" + juce::String(b))) {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost(prm->convertTo0to1(learned[(size_t)b]));
            prm->endChangeGesture();
        }
    if (auto* amt = apvts.getParameter("matchAmount"))
        if (amt->getValue() < 0.01f) { amt->beginChangeGesture(); amt->setValueNotifyingHost(1.f); amt->endChangeGesture(); }
}

void AxeProcessor::pulseLearn(const char* id) {
    if (auto* prm = apvts.getParameter(id)) {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost(1.f);
        prm->endChangeGesture();
        // back to off after a moment, so the next press is a new rising edge
        juce::Timer::callAfterDelay(150, [this, s = juce::String(id)] {
            if (auto* q = apvts.getParameter(s)) { q->beginChangeGesture(); q->setValueNotifyingHost(0.f); q->endChangeGesture(); }
        });
    }
}

void AxeProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = apvts.copyState();
    if (haveFreshSaved) {
        juce::String f;
        for (float v : freshSaved) f << juce::String(v, 3) << " ";
        state.setProperty("freshProfile", f.trim(), nullptr);
    }
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void AxeProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts.state.getType())) {
            auto state = juce::ValueTree::fromXml(*xml);
            auto f = juce::StringArray::fromTokens(state.getProperty("freshProfile").toString(), " ", "");
            if (f.size() == axe::kMatchBands) {
                for (int b = 0; b < axe::kMatchBands; b++) freshSaved[(size_t)b] = f[b].getFloatValue();
                haveFreshSaved = true;
                freshToEngine = true;
            }
            state.removeProperty("freshProfile", nullptr);
            apvts.replaceState(state);
            dirty = true;
        }
}

juce::AudioProcessorEditor* AxeProcessor::createEditor() { return new AxeEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AxeProcessor(); }
