#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "presets.h"

// Field accessors for the parameter table.
#define RASP_F(m) [](const rasp::Params& p) { return (float)p.m; }, [](rasp::Params& p, float v) { p.m = v; }
#define RASP_B(m) [](const rasp::Params& p) { return p.m ? 1.f : 0.f; }, [](rasp::Params& p, float v) { p.m = v > 0.5f; }
#define RASP_I(m) [](const rasp::Params& p) { return (float)p.m; }, [](rasp::Params& p, float v) { p.m = (int)std::lround(v); }

// Interval menu: index 0..24 = -12..+12 semitones.
static const char* kSemis =
    "-12 oct|-11 st|-10 st|-9 st|-8 st|-7 5th|-6 st|-5 4th|-4 st|-3 st|-2 st|-1 st|0 unison|"
    "+1 st|+2 st|+3 st|+4 st|+5 4th|+6 st|+7 5th|+8 st|+9 st|+10 st|+11 st|+12 oct";

static juce::String hz(float h) { return h >= 1000.f ? juce::String(h / 1000.f, 1) + " kHz" : juce::String(juce::roundToInt(h)) + " Hz"; }

const std::vector<ParamSpec>& paramSpecs() {
    using K = ParamSpec;
    static const std::vector<ParamSpec> specs = {
        // CONTROL
        {"controlOn", "Control On", "Control", K::Bool, 0, 1, nullptr, RASP_B(controlOn), nullptr, false},
        {"drive", "Drive", "Control", K::Unit, 0, 1, nullptr, RASP_F(drive), nullptr, false},
        {"mix", "Drive Mix", "Control", K::Unit, 0, 1, nullptr, RASP_F(mix), nullptr, false},
        {"brightness", "Tone", "Control", K::Choice, 0, 2, "Dark|Natural|Bright", RASP_I(brightness), nullptr, false},
        {"loCut", "Lo Cut", "Control", K::Unit, 0, 1, nullptr, RASP_F(loCut),
         [](float v) { return hz(rasp::expMap(v, 20.f, 600.f)); }, false},
        {"hiCut", "Hi Cut", "Control", K::Unit, 0, 1, nullptr, RASP_F(hiCut),
         [](float v) { return hz(rasp::expMap(v, 2000.f, 20000.f)); }, false},
        {"chop", "Chop", "Control", K::Unit, 0, 1, nullptr, RASP_F(chop), nullptr, false},
        {"release", "Release", "Control", K::Unit, 0, 1, nullptr, RASP_F(release),
         [](float v) { return juce::String(juce::roundToInt(rasp::expMap(v, 60.f, 800.f))) + " ms"; }, false},
        {"deEss", "De-ess", "Control", K::Unit, 0, 1, nullptr, RASP_F(deEss), nullptr, false},
        {"airDb", "Air", "Control", K::Db, 0, 12, nullptr, RASP_F(airDb), nullptr, false},
        {"bodyDb", "Body", "Control", K::Db, 0, 9, nullptr, RASP_F(bodyDb), nullptr, false},
        {"weight", "Weight", "Control", K::Unit, 0, 1, nullptr, RASP_F(weight), nullptr, false},
        {"fill", "Fill", "Control", K::Unit, 0, 1, nullptr, RASP_F(fill), nullptr, false},
        {"smooth", "Smooth", "Control", K::Unit, 0, 1, nullptr, RASP_F(smooth), nullptr, false},
        {"bleed", "Bleed Reducer", "Control", K::Unit, 0, 1, nullptr, RASP_F(bleed), nullptr, false},
        {"controlGainDb", "Control Level", "Control", K::Db, -18, 12, nullptr, RASP_F(controlGainDb), nullptr, false},
        // THICKEN
        {"thickenOn", "Thicken On", "Thicken", K::Bool, 0, 1, nullptr, RASP_B(thickenOn), nullptr, false},
        {"spread", "Spread", "Thicken", K::Unit, 0, 1, nullptr, RASP_F(spread), nullptr, false},
        {"diffuse", "Diffuse", "Thicken", K::Unit, 0, 1, nullptr, RASP_F(diffuse), nullptr, false},
        {"darken", "Darken", "Thicken", K::Unit, 0, 1, nullptr, RASP_F(darken), nullptr, false},
        {"width", "Width", "Thicken", K::Unit, 0, 1, nullptr, RASP_F(width), nullptr, false},
        {"solo", "Thicken Solo", "Thicken", K::Bool, 0, 1, nullptr, RASP_B(solo), nullptr, false},
        {"doubleTrack", "Double-Track", "Thicken", K::Bool, 0, 1, nullptr, RASP_B(doubleTrack), nullptr, false},
        {"thickenGainDb", "Thicken Level", "Thicken", K::Db, -24, 6, nullptr, RASP_F(thickenGainDb), nullptr, false},
        // HARMONY pairs (ids octDown/octUp/pitch1Semi/pitch2Semi kept so saved sessions still load)
        {"pitch1Semi", "Harmony 1 Interval", "Harmony", K::Choice, 0, 24, kSemis,
         [](const rasp::Params& p) { return (float)(p.pitch1Semi + 12); },
         [](rasp::Params& p, float v) { p.pitch1Semi = (int)std::lround(v) - 12; }, nullptr, false},
        {"octDown", "Harmony 1 Level", "Harmony", K::Unit, 0, 1, nullptr, RASP_F(octDown), nullptr, false},
        {"pitch2Semi", "Harmony 2 Interval", "Harmony", K::Choice, 0, 24, kSemis,
         [](const rasp::Params& p) { return (float)(p.pitch2Semi + 12); },
         [](rasp::Params& p, float v) { p.pitch2Semi = (int)std::lround(v) - 12; }, nullptr, false},
        {"octUp", "Harmony 2 Level", "Harmony", K::Unit, 0, 1, nullptr, RASP_F(octUp), nullptr, false},
        {"harm3Semi", "Harmony 3 Interval", "Harmony", K::Choice, 0, 24, kSemis,
         [](const rasp::Params& p) { return (float)(p.harm3Semi + 12); },
         [](rasp::Params& p, float v) { p.harm3Semi = (int)std::lround(v) - 12; }, nullptr, false},
        {"harm3Level", "Harmony 3 Level", "Harmony", K::Unit, 0, 1, nullptr, RASP_F(harm3Level), nullptr, false},
        {"harm4Semi", "Harmony 4 Interval", "Harmony", K::Choice, 0, 24, kSemis,
         [](const rasp::Params& p) { return (float)(p.harm4Semi + 12); },
         [](rasp::Params& p, float v) { p.harm4Semi = (int)std::lround(v) - 12; }, nullptr, false},
        {"harm4Level", "Harmony 4 Level", "Harmony", K::Unit, 0, 1, nullptr, RASP_F(harm4Level), nullptr, false},
        {"harm1Width", "Harmony 1 Width", "Harmony", K::Unit, 0, 1, nullptr, RASP_F(harm1Width), nullptr, false},
        {"harm2Width", "Harmony 2 Width", "Harmony", K::Unit, 0, 1, nullptr, RASP_F(harm2Width), nullptr, false},
        {"harm3Width", "Harmony 3 Width", "Harmony", K::Unit, 0, 1, nullptr, RASP_F(harm3Width), nullptr, false},
        {"harm4Width", "Harmony 4 Width", "Harmony", K::Unit, 0, 1, nullptr, RASP_F(harm4Width), nullptr, false},
        {"harmRandPitch", "Harmony Randomize Pitch", "Harmony", K::Unit, 0, 1, nullptr, RASP_F(harmRandPitch), nullptr, false},
        {"harmRandTime", "Harmony Randomize Time", "Harmony", K::Unit, 0, 1, nullptr, RASP_F(harmRandTime), nullptr, false},
        {"harmLoCut", "Harmony Low Cut", "Harmony", K::Hz, 20, 1000, nullptr, RASP_F(harmLoCut), nullptr, false},
        {"harmHiCut", "Harmony High Cut", "Harmony", K::Hz, 1000, 20000, nullptr, RASP_F(harmHiCut), nullptr, false},
        {"harmWidth", "Harmony Width", "Harmony", K::Unit, 0, 1, nullptr, RASP_F(harmWidth), nullptr, false},
        // FLAVOUR
        {"flavourOn", "Flavour On", "Flavour", K::Bool, 0, 1, nullptr, RASP_B(flavourOn), nullptr, false},
        {"tail", "Tail", "Flavour", K::Unit, 0, 1, nullptr, RASP_F(tail),
         [](float v) { return juce::String(rasp::expMap(v, 0.5f, 6.f), 1) + " s"; }, false},
        {"tailBlend", "Tail Blend", "Flavour", K::Unit, 0, 1, nullptr, RASP_F(tailBlend), nullptr, false},
        {"repeats", "Repeats", "Flavour", K::Unit, 0, 1, nullptr, RASP_F(repeats), nullptr, false},
        {"delayBlend", "Delay Blend", "Flavour", K::Unit, 0, 1, nullptr, RASP_F(delayBlend), nullptr, false},
        {"delayTime", "Delay Time", "Flavour", K::Choice, 0, 2, "1/8|Dotted 1/8|1/4", RASP_I(delayTime), nullptr, false},
        {"tilt", "Tilt", "Flavour", K::Unit, 0, 1, nullptr, RASP_F(tilt),
         [](float v) { float d = (v - 0.5f) * 12.f; return (d > 0 ? "+" : "") + juce::String(d, 1) + " dB"; }, false},
        {"duck", "Duck", "Flavour", K::Unit, 0, 1, nullptr, RASP_F(duck), nullptr, false},
        {"flavourGainDb", "Flavour Level", "Flavour", K::Db, -24, 6, nullptr, RASP_F(flavourGainDb), nullptr, false},
        {"bpm", "Tempo", "Flavour", K::Bpm, 40, 240, nullptr, RASP_F(bpm), nullptr, true},
        // BUS
        {"inDb", "Input Gain", "Bus", K::Db, -12, 36, nullptr, RASP_F(inDb), nullptr, true},
        {"glue", "Glue", "Bus", K::Unit, 0, 1, nullptr, RASP_F(glue), nullptr, false},
        {"outDb", "Output", "Bus", K::Db, -24, 12, nullptr, RASP_F(outDb), nullptr, false},
        // LIVE
        {"bypass", "Bypass", "Live", K::Bool, 0, 1, nullptr, RASP_B(bypass), nullptr, true},
        {"bypassMatch", "Match Bypass Level", "Live", K::Bool, 0, 1, nullptr, RASP_B(bypassMatch), nullptr, true},
        {"mono", "Mono", "Live", K::Bool, 0, 1, nullptr, RASP_B(mono), nullptr, true},
        {"howl", "Feedback Guard", "Live", K::Bool, 0, 1, nullptr, RASP_B(howl), nullptr, true},
        // MATCH (Learn): set by the LEARN button, kept across presets
        {"matchAmount", "Match Amount", "Live", K::Unit, 0, 1, nullptr, RASP_F(matchAmount), nullptr, true},
        {"matchOn", "Match On", "Live", K::Bool, 0, 1, nullptr, RASP_B(matchOn), nullptr, true},
        {"match0", "Match 100 Hz", "Match", K::Db, -12, 12, nullptr, RASP_F(match[0]), nullptr, true},
        {"match1", "Match 160 Hz", "Match", K::Db, -12, 12, nullptr, RASP_F(match[1]), nullptr, true},
        {"match2", "Match 250 Hz", "Match", K::Db, -12, 12, nullptr, RASP_F(match[2]), nullptr, true},
        {"match3", "Match 400 Hz", "Match", K::Db, -12, 12, nullptr, RASP_F(match[3]), nullptr, true},
        {"match4", "Match 630 Hz", "Match", K::Db, -12, 12, nullptr, RASP_F(match[4]), nullptr, true},
        {"match5", "Match 1.6 kHz", "Match", K::Db, -12, 12, nullptr, RASP_F(match[5]), nullptr, true},
        {"match6", "Match 2.5 kHz", "Match", K::Db, -12, 12, nullptr, RASP_F(match[6]), nullptr, true},
        {"match7", "Match 4 kHz", "Match", K::Db, -12, 12, nullptr, RASP_F(match[7]), nullptr, true},
        {"match8", "Match 6.3 kHz", "Match", K::Db, -12, 12, nullptr, RASP_F(match[8]), nullptr, true},
        {"match9", "Match 10 kHz", "Match", K::Db, -12, 12, nullptr, RASP_F(match[9]), nullptr, true},
        // MOMENTARY: on while held (buttons in the window, or automation lanes)
        {"mega", "Mega", "Momentary", K::Bool, 0, 1, nullptr, RASP_B(mega), nullptr, true},
        {"throwOn", "Throw", "Momentary", K::Bool, 0, 1, nullptr, RASP_B(throwOn), nullptr, true},
        {"stutter", "Stutter", "Momentary", K::Bool, 0, 1, nullptr, RASP_B(stutter), nullptr, true},
        {"glitch", "Glitch", "Momentary", K::Bool, 0, 1, nullptr, RASP_B(glitch), nullptr, true},
        {"stutterDiv", "Stutter Slice", "Momentary", K::Choice, 0, 3, "1/4|1/8|1/16|1/32", RASP_I(stutterDiv), nullptr, true},
        {"glitchAmount", "Glitch Amount", "Momentary", K::Unit, 0, 1, nullptr, RASP_F(glitchAmount), nullptr, true},
        {"glitchPattern", "Glitch Pattern", "Momentary", K::Choice, 0, 4, "Random|Pattern A|Pattern B|Pattern C|Pattern D", RASP_I(glitchPattern), nullptr, true},
        {"tapeStop", "Tape Stop", "Momentary", K::Bool, 0, 1, nullptr, RASP_B(tapeStop), nullptr, true},
        {"tapeTime", "Tape Stop Time", "Momentary", K::Choice, 0, 3, "1/8|1/4|1/2|1 bar", RASP_I(tapeTime), nullptr, true},
        {"freeze", "Freeze", "Momentary", K::Bool, 0, 1, nullptr, RASP_B(freeze), nullptr, true},
        {"gate", "Gate", "Momentary", K::Bool, 0, 1, nullptr, RASP_B(gate), nullptr, true},
        {"gatePattern", "Gate Pattern", "Momentary", K::Choice, 0, 3, "1/16 chop|1/8 chop|Syncopated|Trance", RASP_I(gatePattern), nullptr, true},
        {"throwStyle", "Throw Style", "Momentary", K::Choice, 0, 1, "Clean|Glitch", RASP_I(throwStyle), nullptr, true},
        // DEGRADE: part of the sound (saved with presets)
        {"crush", "Crush", "Degrade", K::Unit, 0, 1, nullptr, RASP_F(crush), nullptr, false},
        {"crushMix", "Crush Mix", "Degrade", K::Unit, 0, 1, nullptr, RASP_F(crushMix), nullptr, false},
        {"lofi", "Lo-fi", "Degrade", K::Unit, 0, 1, nullptr, RASP_F(lofi), nullptr, false},
    };
    return specs;
}

static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const rasp::Params d;
    for (const auto& s : paramSpecs()) {
        const juce::ParameterID pid{s.id, 1};
        float def = s.get(d);
        // In a DAW the feedback guard is opt-in: it's for a mic in a room
        // with speakers, not a recorded take.
        if (!std::strcmp(s.id, "howl")) def = 0.f;
        switch (s.kind) {
            case ParamSpec::Bool:
                layout.add(std::make_unique<juce::AudioParameterBool>(pid, s.name, def > 0.5f));
                break;
            case ParamSpec::Choice:
                layout.add(std::make_unique<juce::AudioParameterChoice>(
                    pid, s.name, juce::StringArray::fromTokens(s.choices, "|", ""), (int)def));
                break;
            case ParamSpec::Db:
                layout.add(std::make_unique<juce::AudioParameterFloat>(
                    pid, s.name, juce::NormalisableRange<float>(s.lo, s.hi, 0.1f), def,
                    juce::AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(
                        [](float v, int) { return (v > 0 ? "+" : "") + juce::String(v, 1) + " dB"; })));
                break;
            case ParamSpec::Bpm:
                layout.add(std::make_unique<juce::AudioParameterFloat>(
                    pid, s.name, juce::NormalisableRange<float>(s.lo, s.hi, 0.1f), def,
                    juce::AudioParameterFloatAttributes().withLabel("bpm").withStringFromValueFunction(
                        [](float v, int) { return juce::String(v, 1) + " bpm"; })));
                break;
            case ParamSpec::Hz: {
                juce::NormalisableRange<float> r(s.lo, s.hi);
                r.setSkewForCentre(std::sqrt(s.lo * s.hi));
                layout.add(std::make_unique<juce::AudioParameterFloat>(
                    pid, s.name, r, def,
                    juce::AudioParameterFloatAttributes().withLabel("Hz").withStringFromValueFunction(
                        [](float v, int) { return hz(v); })));
                break;
            }
            case ParamSpec::Unit: {
                auto fmt = s.display;
                layout.add(std::make_unique<juce::AudioParameterFloat>(
                    pid, s.name, juce::NormalisableRange<float>(0.f, 1.f), def,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction(
                        [fmt](float v, int) { return fmt ? fmt(v) : juce::String(juce::roundToInt(v * 100.f)) + "%"; })));
                break;
            }
        }
    }
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"sync", 1}, "Sync to Host Tempo", true));
    return layout;
}

RaspProcessor::RaspProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::mono(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "RASP", makeLayout()) {
    for (const auto& s : paramSpecs()) {
        raw.push_back(apvts.getRawParameterValue(s.id));
        apvts.addParameterListener(s.id, this);
    }
    syncParam = apvts.getRawParameterValue("sync");
    apvts.addParameterListener("sync", this);
    setCurrentProgram(0);   // open on "Huge", the album sound
}

RaspProcessor::~RaspProcessor() {
    for (const auto& s : paramSpecs()) apvts.removeParameterListener(s.id, this);
    apvts.removeParameterListener("sync", this);
}

bool RaspProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    const bool inOk = in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
    const bool outOk = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    return inOk && outOk && !(in == juce::AudioChannelSet::stereo() && out == juce::AudioChannelSet::mono());
}

void RaspProcessor::prepareToPlay(double sampleRate, int) {
    // ~3 MB of delay lines: heap, and a fresh (silent) engine per prepare.
    engine = std::make_unique<rasp::Processor>();
    engine->init((float)sampleRate);
    dirty = true;
}

rasp::Params RaspProcessor::buildParams() const {
    rasp::Params p;
    const auto& specs = paramSpecs();
    for (size_t i = 0; i < specs.size(); i++) specs[i].set(p, raw[i]->load());
    return p;
}

void RaspProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels(), numOut = getTotalNumOutputChannels();
    if (!engine || numOut < 1) { buffer.clear(); return; }

    // Tempo for the delay: the session's, when the host reports one.
    float bpm = 0.f;
    if (syncParam->load() > 0.5f)
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto b = pos->getBpm()) bpm = (float)*b;
    meters.hostTempo = bpm > 0.f;
    const bool monoOut = numOut == 1;

    if (dirty.exchange(false) || std::fabs(bpm - lastBpm) > 1e-3f || monoOut != lastMonoOut) {
        rasp::Params p = buildParams();
        if (bpm > 0.f) p.bpm = bpm;
        if (monoOut) p.mono = true;     // a mono insert sums anyway: drop the comb taps
        engine->setParams(p);
        meters.bpm = p.bpm;
        lastBpm = bpm;
        lastMonoOut = monoOut;
    }
    if (clearNotches.exchange(false)) engine->clearHowl();
    if (learnCmd.exchange(0) == 1) engine->startLearn();
    meters.learning = engine->learning();
    meters.learnProgress = engine->learnProgress();
    {
        float g[rasp::kMatchBands];
        if (engine->learnedMatch(g)) {
            for (int i = 0; i < rasp::kMatchBands; i++) learned[i] = g[i];
            learnDone = true;                   // the editor's timer writes the parameters
        }
    }

    const float* inL = numIn > 0 ? buffer.getReadPointer(0) : nullptr;
    const float* inR = numIn > 1 ? buffer.getReadPointer(1) : nullptr;
    float* outL = buffer.getWritePointer(0);
    float* outR = numOut > 1 ? buffer.getWritePointer(1) : nullptr;
    for (int i = 0; i < n; i++) {
        float x = inL ? inL[i] : 0.f;
        if (inR) x = 0.5f * (x + inR[i]);
        float L, R;
        engine->process(x, L, R);
        if (outR) { outL[i] = L; outR[i] = R; }
        else outL[i] = 0.5f * (L + R);
    }
    for (int ch = 2; ch < buffer.getNumChannels(); ch++) buffer.clear(ch, 0, n);

    meters.chopGr = engine->gainReductionDb();
    meters.limiter = engine->limiterDb();
    meters.bleed = engine->bleedReductionDb();
    meters.bypassMatch = engine->bypassMatchDb();
    meters.notches = engine->howlNotches();
}

int RaspProcessor::getNumPrograms() { return rasp::kNumPresets; }
const juce::String RaspProcessor::getProgramName(int index) {
    return juce::String(rasp::presetName(index)).toUpperCase().substring(0, 1) + juce::String(rasp::presetName(index)).substring(1);
}

void RaspProcessor::setCurrentProgram(int index) {
    if (index < 0 || index >= rasp::kNumPresets) return;
    currentProgram = index;
    const rasp::Params p = rasp::presetParams(index);
    for (const auto& s : paramSpecs()) {
        if (s.keepOnPreset) continue;
        if (auto* prm = apvts.getParameter(s.id))
            prm->setValueNotifyingHost(prm->convertTo0to1(s.get(p)));
    }
}

juce::AudioProcessorParameter* RaspProcessor::getBypassParameter() const {
    return apvts.getParameter("bypass");
}

void RaspProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = apvts.copyState();
    state.setProperty("program", currentProgram, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void RaspProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts.state.getType())) {
            auto state = juce::ValueTree::fromXml(*xml);
            currentProgram = state.getProperty("program", 0);
            apvts.replaceState(state);
            dirty = true;
        }
}

juce::AudioProcessorEditor* RaspProcessor::createEditor() { return new RaspEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RaspProcessor(); }
