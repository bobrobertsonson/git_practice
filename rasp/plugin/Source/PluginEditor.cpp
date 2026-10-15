#include "PluginEditor.h"

#ifndef RASP_BUILD_NUMBER
#define RASP_BUILD_NUMBER 0
#endif
#ifndef RASP_BUILD_ID
#define RASP_BUILD_ID "local"
#endif

namespace {
const juce::Colour kBg{0xff121214}, kPanel{0xff1d1d21}, kEdge{0xff2c2c33}, kText{0xffe8e6e3},
    kDim{0xff8d8a86}, kAccent{0xffe2462c};
constexpr int kW = 1180, kH = 1040, kHeader = 64, kCellW = 92, kCellH = 112, kTitle = 34, kPad = 10;

using APVTS = juce::AudioProcessorValueTreeState;

struct KnobCell : juce::Component {
    juce::Slider knob{juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow};
    juce::Label label;
    std::unique_ptr<APVTS::SliderAttachment> attach;
    KnobCell(APVTS& s, const char* id, const juce::String& name) {
        knob.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 80, 18);
        knob.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        label.setText(name, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setColour(juce::Label::textColourId, kDim);
        addAndMakeVisible(knob); addAndMakeVisible(label);
        attach = std::make_unique<APVTS::SliderAttachment>(s, id, knob);
        knob.setDoubleClickReturnValue(true, s.getParameter(id)->convertFrom0to1(s.getParameter(id)->getDefaultValue()));
    }
    void resized() override {
        auto r = getLocalBounds();
        label.setBounds(r.removeFromTop(18));
        knob.setBounds(r.withTrimmedBottom(8));
    }
};

struct ChoiceCell : juce::Component {
    juce::ComboBox box;
    juce::Label label;
    std::unique_ptr<APVTS::ComboBoxAttachment> attach;
    ChoiceCell(APVTS& s, const char* id, const juce::String& name) {
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*>(s.getParameter(id))) box.addItemList(c->choices, 1);
        label.setText(name, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setColour(juce::Label::textColourId, kDim);
        addAndMakeVisible(box); addAndMakeVisible(label);
        attach = std::make_unique<APVTS::ComboBoxAttachment>(s, id, box);
    }
    void resized() override {
        auto r = getLocalBounds();
        label.setBounds(r.removeFromTop(18));
        box.setBounds(r.withSizeKeepingCentre(r.getWidth() - 8, 24).translated(0, -10));
    }
};

// A momentary button in a panel cell: on while the mouse holds it.
struct HoldCell : juce::Component {
    juce::TextButton button;
    juce::RangedAudioParameter* prm;
    HoldCell(APVTS& s, const char* id, const juce::String& name, const juce::String& tip) : prm(s.getParameter(id)) {
        button.setButtonText(name);
        button.setTooltip(tip);
        button.onStateChange = [this] {
            const bool down = button.isDown();
            if (down == (prm->getValue() > 0.5f)) return;
            prm->beginChangeGesture(); prm->setValueNotifyingHost(down ? 1.f : 0.f); prm->endChangeGesture();
        };
        addAndMakeVisible(button);
    }
    void sync() { button.setToggleState(prm->getValue() > 0.5f, juce::dontSendNotification); }
    void resized() override { button.setBounds(getLocalBounds().withSizeKeepingCentre(getWidth() - 10, 40)); }
};

struct ToggleCell : juce::Component {
    juce::ToggleButton button;
    std::unique_ptr<APVTS::ButtonAttachment> attach;
    ToggleCell(APVTS& s, const char* id, const juce::String& name) {
        button.setButtonText(name);
        addAndMakeVisible(button);
        attach = std::make_unique<APVTS::ButtonAttachment>(s, id, button);
    }
    void resized() override { button.setBounds(getLocalBounds().withSizeKeepingCentre(getWidth(), 26)); }
};
} // namespace

RaspEditor::RaspEditor(RaspProcessor& p) : AudioProcessorEditor(p), proc(p) {
    laf.setColourScheme({kBg, kPanel, kPanel, kEdge, kText, kAccent, kText, kAccent, kText});
    laf.setColour(juce::Slider::rotarySliderFillColourId, kAccent);
    laf.setColour(juce::Slider::rotarySliderOutlineColourId, kEdge);
    laf.setColour(juce::Slider::thumbColourId, kText);
    laf.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    laf.setColour(juce::Slider::textBoxTextColourId, kText);
    laf.setColour(juce::ToggleButton::tickColourId, kAccent);
    laf.setColour(juce::TextButton::buttonOnColourId, kAccent);
    setLookAndFeel(&laf);

    auto& s = proc.apvts;
    panels.resize(7);
    auto& control = panels[0]; control.title = "CONTROL"; control.cols = 4;
    addPower(control, "controlOn");
    addKnob(control, "drive", "Drive"); addKnob(control, "mix", "Drive mix"); addChoice(control, "brightness", "Tone"); addKnob(control, "weight", "Weight");
    addKnob(control, "chop", "Chop"); addKnob(control, "release", "Release"); addKnob(control, "deEss", "De-ess"); addKnob(control, "fill", "Fill");
    addKnob(control, "loCut", "Lo cut"); addKnob(control, "hiCut", "Hi cut"); addKnob(control, "bleed", "Bleed reducer"); addKnob(control, "smooth", "Smooth");
    addKnob(control, "bodyDb", "Body"); addKnob(control, "airDb", "Air"); addKnob(control, "controlGainDb", "Level"); addKnob(control, "plosive", "Plosive");

    auto& thicken = panels[1]; thicken.title = "THICKEN"; thicken.cols = 2;
    addPower(thicken, "thickenOn");
    addKnob(thicken, "spread", "Spread"); addKnob(thicken, "diffuse", "Diffuse");
    addKnob(thicken, "darken", "Darken"); addKnob(thicken, "width", "Width");
    addKnob(thicken, "thickenGainDb", "Level"); addToggle(thicken, "solo", "Solo"); addToggle(thicken, "doubleTrack", "Double-track");


    auto& flavour = panels[2]; flavour.title = "FLAVOUR"; flavour.cols = 3;
    addPower(flavour, "flavourOn");
    addKnob(flavour, "tail", "Tail"); addKnob(flavour, "tailBlend", "Tail blend"); addKnob(flavour, "tilt", "Tilt");
    addKnob(flavour, "repeats", "Repeats"); addKnob(flavour, "delayBlend", "Delay blend"); addChoice(flavour, "delayTime", "Time");
    addKnob(flavour, "duck", "Duck"); addKnob(flavour, "flavourGainDb", "Level"); addKnob(flavour, "bpm", "Tempo");
    addToggle(flavour, "sync", "Sync to host"); addKnob(flavour, "delayDuck", "Delay duck"); addKnob(flavour, "duckRelease", "Swell");

    auto& harm = panels[4]; harm.title = "HARMONY"; harm.cols = 8; harm.newRow = true;
    addChoice(harm, "pitch1Semi", "Pair 1"); addKnob(harm, "octDown", "Level 1");
    addChoice(harm, "pitch2Semi", "Pair 2"); addKnob(harm, "octUp", "Level 2");
    addChoice(harm, "harm3Semi", "Pair 3"); addKnob(harm, "harm3Level", "Level 3");
    addChoice(harm, "harm4Semi", "Pair 4"); addKnob(harm, "harm4Level", "Level 4");
    addKnob(harm, "harm1Width", "Width 1"); addKnob(harm, "harm2Width", "Width 2");
    addKnob(harm, "harm3Width", "Width 3"); addKnob(harm, "harm4Width", "Width 4");
    addKnob(harm, "harmRandPitch", "Random pitch"); addKnob(harm, "harmRandTime", "Random time");
    addKnob(harm, "harmLoCut", "Low cut"); addKnob(harm, "harmHiCut", "High cut");

    auto& gl = panels[5]; gl.title = "GLITCH / DEGRADE"; gl.cols = 4;
    addChoice(gl, "stutterDiv", "Slice"); addKnob(gl, "glitchAmount", "Glitch"); addChoice(gl, "glitchPattern", "Pattern");
    addChoice(gl, "throwStyle", "Throw");
    addHold(gl, "tapeStop", "TAPE STOP", "Hold: the voice slows to a stop like a tape deck losing power");
    addChoice(gl, "tapeTime", "Stop time");
    addHold(gl, "freeze", "FREEZE", "Hold: the moment before the press sustains as a drifting drone under your voice");
    addHold(gl, "gate", "GATE", "Hold: the voice chops on and off in the Gate pattern, on the tempo grid");
    addChoice(gl, "gatePattern", "Gate pattern"); addKnob(gl, "crush", "Crush"); addKnob(gl, "crushMix", "Crush mix");
    addKnob(gl, "lofi", "Lo-fi");

    auto& vox = panels[6]; vox.title = "VOX"; vox.cols = 8; vox.under = 4;
    addChoice(vox, "voxMode", "Mode"); addKnob(vox, "voxMix", "Mix"); addChoice(vox, "voxNote", "Note");
    addChoice(vox, "voxChord", "Chord"); addToggle(vox, "voxFollow", "Follow pitch"); addKnob(vox, "voxFormant", "Formant");
    addKnob(vox, "voxRing", "Ring mod");

    auto& bus = panels[3]; bus.title = "BUS / LIVE"; bus.cols = 2;
    addKnob(bus, "inDb", "Input"); addKnob(bus, "glue", "Glue"); addKnob(bus, "outDb", "Output");
    addKnob(bus, "matchAmount", "Match"); addToggle(bus, "mono", "Mono"); addToggle(bus, "bypassMatch", "Match bypass"); addToggle(bus, "howl", "Feedback guard"); addChoice(bus, "character", "Character");

    for (int i = 0; i < proc.getNumPrograms(); i++) preset.addItem(proc.getProgramName(i), i + 1);
    preset.setSelectedId(proc.getCurrentProgram() + 1, juce::dontSendNotification);
    preset.onChange = [this] { proc.setCurrentProgram(preset.getSelectedId() - 1); };
    content.addAndMakeVisible(preset);

    bypass.setClickingTogglesState(true);
    bypassAttach = std::make_unique<APVTS::ButtonAttachment>(s, "bypass", bypass);
    content.addAndMakeVisible(bypass);

    holdButton(megaBtn, "mega");
    holdButton(throwBtn, "throwOn");
    holdButton(stutterBtn, "stutter");
    holdButton(glitchBtn, "glitch");
    holdButton(compareBtn, "compare");
    compareBtn.setTooltip("Hold: hear your dry voice at the processed loudness, for a fair before/after");
    stutterBtn.setTooltip("Hold: repeat the slice that starts now, on the tempo grid (Slice sets its length)");
    glitchBtn.setTooltip("Hold: like stutter, but repeats roll, reverse, drop an octave or gate at random (Glitch sets how often)");
    megaBtn.setTooltip("Hold: hard-clipped mid burst (or automate the Mega parameter)");
    throwBtn.setTooltip("Hold: send the voice to a long delay; repeats ring out after release");

    clearNotches.onClick = [this] { proc.requestClearNotches(); };
    learnBtn.setTooltip("Sing (or play a DI) for about 15 seconds: RASP matches your voice and mic to the tone the presets were tuned on");
    learnBtn.onClick = [this] { proc.requestLearn(); };
    content.addAndMakeVisible(learnBtn);
    matchBtn.setClickingTogglesState(true);
    matchBtn.setTooltip("Switch the learned match off and on to hear what Learn changed");
    matchAttach = std::make_unique<APVTS::ButtonAttachment>(s, "matchOn", matchBtn);
    matchBtn.onStateChange = [this] { matchBtn.setButtonText(matchBtn.getToggleState() ? "MATCH ON" : "MATCH OFF"); };
    content.addAndMakeVisible(matchBtn);
    content.addAndMakeVisible(clearNotches);

    status.setColour(juce::Label::textColourId, kDim);
    status.setJustificationType(juce::Justification::centredLeft);
    content.addAndMakeVisible(status);

    content.painter = [this](juce::Graphics& g) { paintContent(g); };
    addAndMakeVisible(content);
    const float s0 = juce::jlimit(0.5f, 1.5f, proc.uiScale.load());   // read before the limits resize us
    setResizable(true, true);
    setResizeLimits(kW / 2, kH / 2, kW * 3 / 2, kH * 3 / 2);
    getConstrainer()->setFixedAspectRatio((double)kW / (double)kH);
    setSize(juce::roundToInt(kW * s0), juce::roundToInt(kH * s0));
    startTimerHz(15);
}

RaspEditor::~RaspEditor() { setLookAndFeel(nullptr); }

void RaspEditor::holdButton(juce::TextButton& b, const char* id) {
    auto* prm = proc.apvts.getParameter(id);
    b.setColour(juce::TextButton::buttonOnColourId, kAccent);
    b.onStateChange = [&b, prm] {
        const bool down = b.isDown();
        if (down == (prm->getValue() > 0.5f)) return;
        prm->beginChangeGesture();
        prm->setValueNotifyingHost(down ? 1.f : 0.f);
        prm->endChangeGesture();
    };
    content.addAndMakeVisible(b);
}

void RaspEditor::addKnob(Panel& pn, const char* id, const juce::String& label) {
    pn.cells.push_back(std::make_unique<KnobCell>(proc.apvts, id, label));
    content.addAndMakeVisible(*pn.cells.back());
}
void RaspEditor::addChoice(Panel& pn, const char* id, const juce::String& label) {
    pn.cells.push_back(std::make_unique<ChoiceCell>(proc.apvts, id, label));
    content.addAndMakeVisible(*pn.cells.back());
}
void RaspEditor::addHold(Panel& pn, const char* id, const juce::String& label, const juce::String& tip) {
    auto cell = std::make_unique<HoldCell>(proc.apvts, id, label, tip);
    holds.push_back(cell.get());
    pn.cells.push_back(std::move(cell));
    content.addAndMakeVisible(*pn.cells.back());
}
void RaspEditor::addToggle(Panel& pn, const char* id, const juce::String& label) {
    pn.cells.push_back(std::make_unique<ToggleCell>(proc.apvts, id, label));
    content.addAndMakeVisible(*pn.cells.back());
}
void RaspEditor::addPower(Panel& pn, const char* id) {
    pn.power = std::make_unique<juce::ToggleButton>("On");
    powerAttach.push_back(std::make_unique<APVTS::ButtonAttachment>(proc.apvts, id, *pn.power));
    content.addAndMakeVisible(*pn.power);
}

void RaspEditor::paint(juce::Graphics& g) { g.fillAll(kBg); }

void RaspEditor::paintContent(juce::Graphics& g) {
    g.fillAll(kBg);
    g.setColour(kText);
    g.setFont(juce::FontOptions(26.f, juce::Font::bold));
    g.drawText("RASP", kPad + 4, 0, 100, kHeader, juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(12.f));
    g.drawText("extreme vocal processor", kPad + 92, 0, 160, kHeader, juce::Justification::centredLeft);
    g.setFont(juce::FontOptions(10.f));
    g.drawText(juce::String("build ") + juce::String(RASP_BUILD_NUMBER) + " \u00b7 " + RASP_BUILD_ID,
               kPad + 92, kHeader / 2 + 8, 200, 14, juce::Justification::centredLeft);

    for (auto& pn : panels) {
        g.setColour(kPanel);
        g.fillRoundedRectangle(pn.bounds.toFloat(), 8.f);
        g.setColour(kEdge);
        g.drawRoundedRectangle(pn.bounds.toFloat().reduced(0.5f), 8.f, 1.f);
        g.setColour(kAccent);
        g.setFont(juce::FontOptions(14.f, juce::Font::bold));
        g.drawText(pn.title, pn.bounds.getX() + 12, pn.bounds.getY(), 140, kTitle, juce::Justification::centredLeft);
    }

    // Input meter (bottom left). Green zone: singing RMS -30..-12 dBFS, where
    // the drive, chop and Learn were tuned; red: peaks within 3 dB of clipping.
    const int mx = kPad + 4, my = kH - 22, mw = 220, mh = 12;
    auto xOf = [&](float db) { return mx + 30 + (int)((juce::jlimit(-60.f, 0.f, db) + 60.f) / 60.f * (float)(mw - 30)); };
    g.setColour(kDim);
    g.setFont(juce::FontOptions(11.f, juce::Font::bold));
    g.drawText("IN", mx, my - 1, 26, mh + 2, juce::Justification::centredLeft);
    g.setColour(kEdge);
    g.fillRect(mx + 30, my, mw - 30, mh);
    g.setColour(juce::Colour(0xff254a2e));
    g.fillRect(xOf(-30.f), my, xOf(-12.f) - xOf(-30.f), mh);
    g.setColour(juce::Colour(0xff5a2020));
    g.fillRect(xOf(-3.f), my, xOf(0.f) - xOf(-3.f), mh);
    const bool good = meterRms > -30.f && meterRms < -12.f, hot = meterPeak > -3.f;
    g.setColour(hot ? juce::Colour(0xffe2462c) : (good ? juce::Colour(0xff4cc26a) : kText));
    g.fillRect(mx + 30, my + 3, xOf(meterRms) - (mx + 30), mh - 6);
    g.fillRect(xOf(meterPeak) - 1, my, 2, mh);
}

void RaspEditor::resized() {
    const float sc = (float)getWidth() / (float)kW;
    content.setBounds(0, 0, kW, kH);
    content.setTransform(juce::AffineTransform::scale(sc));
    if (getWidth() >= kW / 2) proc.uiScale = sc;   // not the 0 x 0 pass before setSize
    const juce::Rectangle<int> all(0, 0, kW, kH);
    auto top = all.withHeight(kHeader).reduced(kPad, 14);
    bypass.setBounds(top.removeFromRight(90));
    top.removeFromRight(6);
    clearNotches.setBounds(top.removeFromRight(110));
    top.removeFromRight(6);
    preset.setBounds(top.removeFromRight(120));
    top.removeFromRight(12);
    for (auto* b : {&glitchBtn, &stutterBtn, &throwBtn, &megaBtn, &compareBtn}) {
        b->setBounds(top.removeFromRight(b == &compareBtn ? 92 : 78));
        top.removeFromRight(6);
    }
    top.removeFromRight(6);
    matchBtn.setBounds(top.removeFromRight(96));
    top.removeFromRight(6);
    learnBtn.setBounds(top.removeFromRight(76));

    status.setBounds(all.withTop(kH - 30).reduced(kPad + 4, 0).withTrimmedLeft(236));

    int x = kPad, y = kHeader, rowBottom = kHeader;
    for (auto& pn : panels) {
        const int rows = ((int)pn.cells.size() + pn.cols - 1) / pn.cols;
        if (pn.newRow) { x = kPad; y = rowBottom + kPad; }
        if (pn.under >= 0) { x = panels[(size_t)pn.under].bounds.getX(); y = panels[(size_t)pn.under].bounds.getBottom() + kPad; }
        pn.bounds = {x, y, pn.cols * kCellW + 2 * kPad, kTitle + rows * kCellH + kPad};
        rowBottom = std::max(rowBottom, pn.bounds.getBottom());
        if (pn.power) pn.power->setBounds(pn.bounds.getRight() - 60, pn.bounds.getY() + 4, 52, 26);
        for (size_t i = 0; i < pn.cells.size(); i++)
            pn.cells[i]->setBounds(pn.bounds.getX() + kPad + (int)(i % (size_t)pn.cols) * kCellW,
                                   pn.bounds.getY() + kTitle + (int)(i / (size_t)pn.cols) * kCellH, kCellW, kCellH);
        x = pn.bounds.getRight() + kPad;
    }
}

void RaspEditor::timerCallback() {
    auto& m = proc.meters;
    juce::String t;
    t << "Chop " << juce::String(m.chopGr.load(), 1) << " dB   Limiter " << juce::String(m.limiter.load(), 1)
      << " dB   Bleed " << juce::String(m.bleed.load(), 0) << " dB   Notches " << m.notches.load() << "   "
      << juce::roundToInt(m.bpm.load()) << " bpm" << (m.hostTempo.load() ? " (host)" : "");
    if (const int c = m.character.load(); c > 0) t << "   Voice: " << (c == 3 ? "clean" : (c == 2 ? "shout" : "scream"));
    if (m.plosive.load() < -3.f) t << "   Pop cut";
    status.setText(t, juce::dontSendNotification);
    if (proc.applyLearned()) learnBtn.setButtonText("LEARNED");
    else if (m.learning.load()) learnBtn.setButtonText("SING " + juce::String(juce::roundToInt(m.learnProgress.load() * 100.f)) + "%");
    else if (learnBtn.getButtonText().startsWith("SING")) learnBtn.setButtonText("LEARN");
    // Light the hold buttons when automation (not the mouse) engages them.
    megaBtn.setToggleState(proc.apvts.getRawParameterValue("mega")->load() > 0.5f, juce::dontSendNotification);
    throwBtn.setToggleState(proc.apvts.getRawParameterValue("throwOn")->load() > 0.5f, juce::dontSendNotification);
    stutterBtn.setToggleState(proc.apvts.getRawParameterValue("stutter")->load() > 0.5f, juce::dontSendNotification);
    glitchBtn.setToggleState(proc.apvts.getRawParameterValue("glitch")->load() > 0.5f, juce::dontSendNotification);
    compareBtn.setToggleState(proc.apvts.getRawParameterValue("compare")->load() > 0.5f, juce::dontSendNotification);
    meterPeak = m.inPeak.load(); meterRms = m.inRms.load();
    content.repaint(kPad, kH - 26, 240, 22);
    for (auto* h : holds) static_cast<HoldCell*>(h)->sync();
    matchBtn.setEnabled(proc.apvts.getRawParameterValue("matchAmount")->load() > 0.001f);   // nothing to A/B before a Learn
    if (preset.getSelectedId() != proc.getCurrentProgram() + 1)
        preset.setSelectedId(proc.getCurrentProgram() + 1, juce::dontSendNotification);
}
