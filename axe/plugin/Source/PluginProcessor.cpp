#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <cstring>
#include <optional>

namespace {
float pct(float v) { return v * 0.01f; }
float invExp(float v, float lo, float hi) { return std::log(v / lo) / std::log(hi / lo); }
float invLin(float v, float lo, float hi) { return (v - lo) / (hi - lo); }
int toInt(float v) { return (int)std::lround(v); }
}  // namespace

#define AXE_SET(expr) [](axe::Params& p, float v) { (void)v; expr; }

// Parameter order = LV2 port order = the Anagram's pages of 6.
const std::vector<ParamSpec>& paramSpecs() {
    using K = ParamSpec;
    static const std::vector<ParamSpec> specs = {
        // page 1: global
        {"mode", "Mode", "Global", -1, K::Choice, 0, 12, 0, "",
         "Tight|Sub|Buzzsaw|Glitch|Laser|Dive Bomb|Chiptune|Stutter Gate|Ring Shredder|Freeze|Feedback|Swarm|Radio", 0, AXE_SET(p.mode = toInt(v))},
        {"instrument", "Instrument", "Global", -1, K::Choice, 0, 1, 0, "", "Guitar|Bass", 0, AXE_SET(p.instrument = toInt(v))},
        {"input", "Input", "Global", -1, K::Float, -12, 12, 0, "dB", nullptr, 0, AXE_SET(p.inDb = v)},
        {"gate", "Gate", "Global", -1, K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.gate = pct(v))},
        {"mix", "Mix", "Global", -1, K::Float, 0, 100, 100, "%", nullptr, 0, AXE_SET(p.mix = pct(v))},
        {"output", "Output", "Global", -1, K::Float, -24, 12, 0, "dB", nullptr, 0, AXE_SET(p.outDb = v)},
        // page 2: trigger and tempo
        {"trigger", "Trigger", "Trigger", -1, K::Bool, 0, 1, 0, "", nullptr, 0, AXE_SET(p.trigger = v > 0.5f)},
        {"engage", "Engage", "Trigger", -1, K::Choice, 0, 1, 0, "", "Always|While Triggered", 0, AXE_SET(p.engage = toInt(v))},
        {"sync", "Sync", "Trigger", -1, K::Choice, 0, 1, 1, "", "Tempo Knob|Host Tempo", 0, AXE_SET(p.sync = toInt(v))},
        {"tempo", "Tempo", "Trigger", -1, K::Float, 40, 300, 120, "bpm", nullptr, 120, AXE_SET(p.tempo = v)},
        {"rate", "Rate", "Trigger", -1, K::Choice, 0, 11, 4, "", "1/1|1/2|1/4|1/8|1/16|1/32|1/2T|1/4T|1/8T|1/16T|1/4D|1/8D", 0, AXE_SET(p.rate = toInt(v))},
        {"swing", "Swing", "Trigger", -1, K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.swing = pct(v))},
        // Tight
        {"tight", "Tight", "Tight", axe::kTight, K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.tight = pct(v))},
        {"punch", "Punch", "Tight", axe::kTight, K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.punch = pct(v))},
        {"release", "Release", "Tight", axe::kTight, K::Float, 30, 300, 75, "ms", nullptr, 95, AXE_SET(p.release = invExp(v, 30.f, 300.f))},
        {"low", "Low", "Tight", axe::kTight, K::Float, -9, 9, 0, "dB", nullptr, 0, AXE_SET(p.lowDb = v)},
        {"mid", "Mid", "Tight", axe::kTight, K::Float, -9, 9, 0, "dB", nullptr, 0, AXE_SET(p.midDb = v)},
        {"high", "High", "Tight", axe::kTight, K::Float, -9, 9, 0, "dB", nullptr, 0, AXE_SET(p.highDb = v)},
        // Sub
        {"subType", "Sub Type", "Sub", axe::kSub, K::Choice, 0, 1, 0, "", "Poly (chords)|Mono (single notes)", 0, AXE_SET(p.subType = toInt(v))},
        {"sub", "Sub", "Sub", axe::kSub, K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.sub = pct(v))},
        {"dry", "Dry", "Sub", axe::kSub, K::Float, 0, 100, 100, "%", nullptr, 0, AXE_SET(p.dry = pct(v))},
        {"subTone", "Sub Tone", "Sub", axe::kSub, K::Float, 80, 1600, 358, "Hz", nullptr, 358, AXE_SET(p.subTone = invExp(v, 80.f, 1600.f))},
        {"range", "Range", "Sub", axe::kSub, K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.range = pct(v))},
        {"grit", "Grit", "Sub", axe::kSub, K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.grit = pct(v))},
        // Buzzsaw
        {"gain", "Gain", "Buzzsaw", axe::kBuzzsaw, K::Float, 0, 100, 65, "%", nullptr, 0, AXE_SET(p.gain = pct(v))},
        {"lows", "Lows", "Buzzsaw", axe::kBuzzsaw, K::Float, 0, 15, 10.5f, "dB", nullptr, 0, AXE_SET(p.lows = v / 15.f)},
        {"highs", "Highs", "Buzzsaw", axe::kBuzzsaw, K::Float, 0, 15, 10.5f, "dB", nullptr, 0, AXE_SET(p.highs = v / 15.f)},
        {"scoop", "Scoop", "Buzzsaw", axe::kBuzzsaw, K::Float, 0, 12, 6, "dB", nullptr, 0, AXE_SET(p.scoop = v / 12.f)},
        {"level", "Level", "Buzzsaw", axe::kBuzzsaw, K::Float, -18, 12, 0, "dB", nullptr, 0, AXE_SET(p.level = v)},
        {"blend", "Blend", "Buzzsaw", axe::kBuzzsaw, K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.blend = pct(v))},
        // Glitch
        {"gChaos", "Chaos", "Glitch", axe::kGlitch, K::Float, 0, 100, 40, "%", nullptr, 0, AXE_SET(p.gChaos = pct(v))},
        {"gCrush", "Crush", "Glitch", axe::kGlitch, K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.gCrush = pct(v))},
        {"gJumps", "Pitch Jumps", "Glitch", axe::kGlitch, K::Float, 0, 100, 40, "%", nullptr, 0, AXE_SET(p.gJumps = pct(v))},
        {"gTapeStop", "Tape Stops", "Glitch", axe::kGlitch, K::Float, 0, 100, 30, "%", nullptr, 0, AXE_SET(p.gTapeStop = pct(v))},
        {"gReverse", "Reverse", "Glitch", axe::kGlitch, K::Float, 0, 100, 30, "%", nullptr, 0, AXE_SET(p.gReverse = pct(v))},
        {"gSmooth", "Smooth", "Glitch", axe::kGlitch, K::Float, 0.3f, 8, 1.6f, "ms", nullptr, 1.6f, AXE_SET(p.gSmooth = invExp(v, 0.3f, 8.f))},
        // Laser
        {"lPitch", "Pitch", "Laser", axe::kLaser, K::Float, 200, 6000, 1095, "Hz", nullptr, 1095, AXE_SET(p.lPitch = invExp(v, 200.f, 6000.f))},
        {"lTrack", "Track Note", "Laser", axe::kLaser, K::Bool, 0, 1, 0, "", nullptr, 0, AXE_SET(p.lTrack = v > 0.5f)},
        {"lDrop", "Drop", "Laser", axe::kLaser, K::Float, 12, 60, 36, "st", nullptr, 0, AXE_SET(p.lDrop = invLin(v, 12.f, 60.f))},
        {"lSweep", "Sweep", "Laser", axe::kLaser, K::Float, 20, 800, 88, "ms", nullptr, 126, AXE_SET(p.lSweep = invExp(v, 20.f, 800.f))},
        {"lTone", "Tone", "Laser", axe::kLaser, K::Float, 0, 100, 30, "%", nullptr, 0, AXE_SET(p.lTone = pct(v))},
        {"lLevel", "Level", "Laser", axe::kLaser, K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.lLevel = pct(v))},
        // Dive Bomb
        {"dDepth", "Depth", "Dive Bomb", axe::kDive, K::Float, 1, 36, 12, "st", nullptr, 0, AXE_SET(p.dDepth = v)},
        {"dTime", "Dive Time", "Dive Bomb", axe::kDive, K::Float, 0.0625f, 4, 0.5f, "beats", nullptr, 0.5f, AXE_SET(p.dTime = invExp(v, 0.0625f, 4.f))},
        {"dReturn", "Return", "Dive Bomb", axe::kDive, K::Float, 0.0625f, 4, 0.22f, "beats", nullptr, 0.5f, AXE_SET(p.dReturn = invExp(v, 0.0625f, 4.f))},
        {"dCurve", "Curve", "Dive Bomb", axe::kDive, K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.dCurve = pct(v))},
        {"dWobble", "Wobble", "Dive Bomb", axe::kDive, K::Float, 0, 100, 20, "%", nullptr, 0, AXE_SET(p.dWobble = pct(v))},
        {"dDry", "Dry", "Dive Bomb", axe::kDive, K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.dDry = pct(v))},
        // Chiptune
        {"cPulse", "Pulse Width", "Chiptune", axe::kChip, K::Float, 12.5f, 50, 31, "%", nullptr, 0, AXE_SET(p.cPulse = invLin(v, 12.5f, 50.f))},
        {"cCrush", "Crush", "Chiptune", axe::kChip, K::Float, 0, 100, 40, "%", nullptr, 0, AXE_SET(p.cCrush = pct(v))},
        {"cArp", "Arpeggio", "Chiptune", axe::kChip, K::Choice, 0, 5, 1, "", "Off|Octave|Power|Major|Minor|Random", 0, AXE_SET(p.cArp = toInt(v))},
        {"cOctave", "Octave", "Chiptune", axe::kChip, K::Choice, 0, 2, 1, "", "-1|0|+1", 0, AXE_SET(p.cOctave = toInt(v))},
        {"cGlide", "Glide", "Chiptune", axe::kChip, K::Float, 0, 300, 60, "ms", nullptr, 0, AXE_SET(p.cGlide = invLin(v, 0.f, 300.f))},
        {"cDry", "Dry", "Chiptune", axe::kChip, K::Float, 0, 100, 20, "%", nullptr, 0, AXE_SET(p.cDry = pct(v))},
        // Stutter Gate
        {"sPattern", "Pattern", "Stutter Gate", axe::kStutter, K::Choice, 0, 5, 0, "", "All|Offbeat|Trance|Gallop|Chug|Random", 0, AXE_SET(p.sPattern = toInt(v))},
        {"sDuty", "Duty", "Stutter Gate", axe::kStutter, K::Float, 10, 100, 55, "%", nullptr, 0, AXE_SET(p.sDuty = invLin(v, 10.f, 100.f))},
        {"sDepth", "Depth", "Stutter Gate", axe::kStutter, K::Float, 0, 100, 100, "%", nullptr, 0, AXE_SET(p.sDepth = pct(v))},
        {"sShape", "Shape", "Stutter Gate", axe::kStutter, K::Float, 0.5f, 20, 1.5f, "ms", nullptr, 3, AXE_SET(p.sShape = invExp(v, 0.5f, 20.f))},
        {"sRandom", "Random", "Stutter Gate", axe::kStutter, K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.sRandom = pct(v))},
        {"sTone", "Muffle", "Stutter Gate", axe::kStutter, K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.sTone = pct(v))},
        // Ring Shredder
        {"rType", "Type", "Ring Shredder", axe::kRing, K::Choice, 0, 1, 0, "", "Ring Mod|Freq Shift", 0, AXE_SET(p.rType = toInt(v))},
        {"rFreq", "Freq", "Ring Shredder", axe::kRing, K::Float, 20, 3000, 147, "Hz", nullptr, 245, AXE_SET(p.rFreq = invExp(v, 20.f, 3000.f))},
        {"rTrack", "Track Note", "Ring Shredder", axe::kRing, K::Bool, 0, 1, 0, "", nullptr, 0, AXE_SET(p.rTrack = v > 0.5f)},
        {"rJump", "Jumps", "Ring Shredder", axe::kRing, K::Float, 0, 100, 40, "%", nullptr, 0, AXE_SET(p.rJump = pct(v))},
        {"rGlide", "Glide", "Ring Shredder", axe::kRing, K::Float, 0.5f, 300, 60, "ms", nullptr, 0, AXE_SET(p.rGlide = invLin(v, 0.5f, 300.f))},
        {"rDrive", "Drive", "Ring Shredder", axe::kRing, K::Float, 0, 100, 20, "%", nullptr, 0, AXE_SET(p.rDrive = pct(v))},
        // Freeze
        {"fzSize", "Size", "Freeze", axe::kFreeze, K::Float, 80, 600, 180, "ms", nullptr, 220, AXE_SET(p.fzSize = invExp(v, 80.f, 600.f))},
        {"fzAttack", "Attack", "Freeze", axe::kFreeze, K::Float, 5, 2000, 17, "ms", nullptr, 100, AXE_SET(p.fzAttack = invExp(v, 5.f, 2000.f))},
        {"fzRelease", "Release", "Freeze", axe::kFreeze, K::Float, 20, 4000, 167, "ms", nullptr, 280, AXE_SET(p.fzRelease = invExp(v, 20.f, 4000.f))},
        {"fzDrift", "Drift", "Freeze", axe::kFreeze, K::Float, 0, 25, 7.5f, "cents", nullptr, 0, AXE_SET(p.fzDrift = v / 25.f)},
        {"fzShimmer", "Shimmer", "Freeze", axe::kFreeze, K::Float, 0, 100, 0, "%", nullptr, 0, AXE_SET(p.fzShimmer = pct(v))},
        {"fzLevel", "Level", "Freeze", axe::kFreeze, K::Float, 0, 100, 70, "%", nullptr, 0, AXE_SET(p.fzLevel = pct(v))},
        // Feedback
        {"fbHarmonic", "Harmonic", "Feedback", axe::kFeedback, K::Choice, 0, 3, 1, "", "Note|Octave|Octave + 5th|2 Octaves", 0, AXE_SET(p.fbHarmonic = toInt(v))},
        {"fbSwell", "Swell", "Feedback", axe::kFeedback, K::Float, 50, 3000, 257, "ms", nullptr, 390, AXE_SET(p.fbSwell = invExp(v, 50.f, 3000.f))},
        {"fbRelease", "Release", "Feedback", axe::kFeedback, K::Float, 20, 3000, 90, "ms", nullptr, 245, AXE_SET(p.fbRelease = invExp(v, 20.f, 3000.f))},
        {"fbLevel", "Level", "Feedback", axe::kFeedback, K::Float, 0, 100, 60, "%", nullptr, 0, AXE_SET(p.fbLevel = pct(v))},
        {"fbBloom", "Bloom", "Feedback", axe::kFeedback, K::Float, 0, 100, 30, "%", nullptr, 0, AXE_SET(p.fbBloom = pct(v))},
        {"fbTone", "Tone", "Feedback", axe::kFeedback, K::Float, 1000, 10000, 3981, "Hz", nullptr, 3162, AXE_SET(p.fbTone = invExp(v, 1000.f, 10000.f))},
        // Swarm
        {"swDensity", "Density", "Swarm", axe::kSwarm, K::Float, 2, 120, 15.5f, "grains/s", nullptr, 15.5f, AXE_SET(p.swDensity = invExp(v, 2.f, 120.f))},
        {"swSize", "Grain Size", "Swarm", axe::kSwarm, K::Float, 10, 250, 36, "ms", nullptr, 50, AXE_SET(p.swSize = invExp(v, 10.f, 250.f))},
        {"swSpray", "Spray", "Swarm", axe::kSwarm, K::Float, 0, 1500, 600, "ms", nullptr, 0, AXE_SET(p.swSpray = v / 1500.f)},
        {"swPitch", "Pitch Spread", "Swarm", axe::kSwarm, K::Float, 0, 100, 30, "%", nullptr, 0, AXE_SET(p.swPitch = pct(v))},
        {"swReverse", "Reverse", "Swarm", axe::kSwarm, K::Float, 0, 100, 20, "%", nullptr, 0, AXE_SET(p.swReverse = pct(v))},
        {"swLevel", "Level", "Swarm", axe::kSwarm, K::Float, 0, 100, 70, "%", nullptr, 0, AXE_SET(p.swLevel = pct(v))},
        // Radio
        {"rdBand", "Station", "Radio", axe::kRadio, K::Float, 0, 100, 50, "%", nullptr, 0, AXE_SET(p.rdBand = pct(v))},
        {"rdWidth", "Width", "Radio", axe::kRadio, K::Float, 0, 100, 40, "%", nullptr, 0, AXE_SET(p.rdWidth = pct(v))},
        {"rdStatic", "Static", "Radio", axe::kRadio, K::Float, 0, 100, 30, "%", nullptr, 0, AXE_SET(p.rdStatic = pct(v))},
        {"rdWarble", "Warble", "Radio", axe::kRadio, K::Float, 0, 100, 30, "%", nullptr, 0, AXE_SET(p.rdWarble = pct(v))},
        {"rdSpin", "Spin", "Radio", axe::kRadio, K::Float, 50, 2000, 218, "ms", nullptr, 316, AXE_SET(p.rdSpin = invExp(v, 50.f, 2000.f))},
        {"rdFade", "Fading", "Radio", axe::kRadio, K::Float, 0, 100, 30, "%", nullptr, 0, AXE_SET(p.rdFade = pct(v))},
    };
    return specs;
}

static juce::String fmtValue(float v, const char* label) {
    juce::String l(label);
    if (l == "dB") return (v > 0.f ? "+" : "") + juce::String(v, 1) + " dB";
    if (l == "Hz") return v >= 1000.f ? juce::String(v / 1000.f, 2) + " kHz" : juce::String(juce::roundToInt(v)) + " Hz";
    if (l == "ms") return v < 10.f ? juce::String(v, 1) + " ms" : juce::String(juce::roundToInt(v)) + " ms";
    if (l == "%") return juce::String(juce::roundToInt(v)) + "%";
    if (l == "bpm") return juce::String(v, 1) + " bpm";
    if (l == "beats") return juce::String(v, v < 1.f ? 3 : 2) + " beats";
    if (l == "st") return juce::String(juce::roundToInt(v)) + " st";
    if (l == "cents") return juce::String(v, 1) + " ct";
    return juce::String(v, 1) + (l.isEmpty() ? "" : " " + l);
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
    apvts.addParameterListener("bypass", &listener);
}

AxeProcessor::~AxeProcessor() {
    for (const auto& s : paramSpecs()) apvts.removeParameterListener(s.id, &listener);
    apvts.removeParameterListener("bypass", &listener);
}

bool AxeProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
    if (in == mono && (out == mono || out == stereo)) return true;   // a guitar: mono, optionally copied to both sides
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

    if (dirty.exchange(false)) {
        const axe::Params p = buildParams();
        for (auto& e : engine) e->setParams(p);
    }
    // Host tempo and position (Logic, most DAWs; the Anagram has none: Tempo knob)
    float bpm = 0.f; double ppq = 0.0; bool playing = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition()) {
            if (auto b = pos->getBpm()) bpm = (float)*b;
            if (auto q = pos->getPpqPosition()) ppq = *q;
            playing = pos->getIsPlaying();
        }
    for (auto& e : engine) e->setHostTransport(bpm, ppq, playing);
    meters.hostTempo = bpm > 0.f;

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

    for (int b = 0; b < 3; b++) meters.gr[b] = engine[0]->gainReductionDb(b);
    meters.gate = engine[0]->gateReductionDb();
    meters.bpm = engine[0]->bpm();
    meters.step = engine[0]->step();
    meters.triggered = engine[0]->triggered();
    meters.pitch = engine[0]->detectedPitch();
}

void AxeProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = apvts.copyState();
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void AxeProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts.state.getType())) {
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
            dirty = true;
        }
}

juce::AudioProcessorEditor* AxeProcessor::createEditor() { return new AxeEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AxeProcessor(); }
