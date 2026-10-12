#include "PluginEditor.h"

namespace {
const juce::Colour kBg{0xff121214}, kPanel{0xff1d1d21}, kEdge{0xff2c2c33}, kText{0xffe8e6e3},
    kDim{0xff8d8a86}, kAccent{0xffe2462c};
constexpr int kW = 1050, kH = 590, kHeader = 64, kCellW = 92, kCellH = 112, kTitle = 34, kPad = 10;

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
    panels.resize(4);
    auto& control = panels[0]; control.title = "CONTROL"; control.cols = 3;
    addPower(control, "controlOn");
    addKnob(control, "drive", "Drive"); addKnob(control, "mix", "Drive mix"); addChoice(control, "brightness", "Tone");
    addKnob(control, "chop", "Chop"); addKnob(control, "release", "Release"); addKnob(control, "deEss", "De-ess");
    addKnob(control, "loCut", "Lo cut"); addKnob(control, "hiCut", "Hi cut"); addKnob(control, "bleed", "Bleed reducer");
    addKnob(control, "controlGainDb", "Level");

    auto& thicken = panels[1]; thicken.title = "THICKEN"; thicken.cols = 2;
    addPower(thicken, "thickenOn");
    addKnob(thicken, "spread", "Spread"); addKnob(thicken, "diffuse", "Diffuse");
    addKnob(thicken, "darken", "Darken"); addKnob(thicken, "width", "Width");
    addKnob(thicken, "thickenGainDb", "Level"); addToggle(thicken, "solo", "Solo");

    auto& flavour = panels[2]; flavour.title = "FLAVOUR"; flavour.cols = 3;
    addPower(flavour, "flavourOn");
    addKnob(flavour, "tail", "Tail"); addKnob(flavour, "tailBlend", "Tail blend"); addKnob(flavour, "tilt", "Tilt");
    addKnob(flavour, "repeats", "Repeats"); addKnob(flavour, "delayBlend", "Delay blend"); addChoice(flavour, "delayTime", "Time");
    addKnob(flavour, "duck", "Duck"); addKnob(flavour, "flavourGainDb", "Level"); addKnob(flavour, "bpm", "Tempo");
    addToggle(flavour, "sync", "Sync to host");

    auto& bus = panels[3]; bus.title = "BUS / LIVE"; bus.cols = 2;
    addKnob(bus, "inDb", "Input"); addKnob(bus, "glue", "Glue"); addKnob(bus, "outDb", "Output");
    addToggle(bus, "mono", "Mono"); addToggle(bus, "bypassMatch", "Match bypass"); addToggle(bus, "howl", "Feedback guard");

    for (int i = 0; i < proc.getNumPrograms(); i++) preset.addItem(proc.getProgramName(i), i + 1);
    preset.setSelectedId(proc.getCurrentProgram() + 1, juce::dontSendNotification);
    preset.onChange = [this] { proc.setCurrentProgram(preset.getSelectedId() - 1); };
    addAndMakeVisible(preset);

    bypass.setClickingTogglesState(true);
    bypassAttach = std::make_unique<APVTS::ButtonAttachment>(s, "bypass", bypass);
    addAndMakeVisible(bypass);

    holdButton(megaBtn, "mega");
    holdButton(throwBtn, "throwOn");
    megaBtn.setTooltip("Hold: hard-clipped mid burst (or automate the Mega parameter)");
    throwBtn.setTooltip("Hold: send the voice to a long delay; repeats ring out after release");

    clearNotches.onClick = [this] { proc.requestClearNotches(); };
    addAndMakeVisible(clearNotches);

    status.setColour(juce::Label::textColourId, kDim);
    status.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(status);

    setSize(kW, kH);
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
    addAndMakeVisible(b);
}

void RaspEditor::addKnob(Panel& pn, const char* id, const juce::String& label) {
    pn.cells.push_back(std::make_unique<KnobCell>(proc.apvts, id, label));
    addAndMakeVisible(*pn.cells.back());
}
void RaspEditor::addChoice(Panel& pn, const char* id, const juce::String& label) {
    pn.cells.push_back(std::make_unique<ChoiceCell>(proc.apvts, id, label));
    addAndMakeVisible(*pn.cells.back());
}
void RaspEditor::addToggle(Panel& pn, const char* id, const juce::String& label) {
    pn.cells.push_back(std::make_unique<ToggleCell>(proc.apvts, id, label));
    addAndMakeVisible(*pn.cells.back());
}
void RaspEditor::addPower(Panel& pn, const char* id) {
    pn.power = std::make_unique<juce::ToggleButton>("On");
    powerAttach.push_back(std::make_unique<APVTS::ButtonAttachment>(proc.apvts, id, *pn.power));
    addAndMakeVisible(*pn.power);
}

void RaspEditor::paint(juce::Graphics& g) {
    g.fillAll(kBg);
    g.setColour(kText);
    g.setFont(juce::FontOptions(26.f, juce::Font::bold));
    g.drawText("RASP", kPad + 4, 0, 100, kHeader, juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(12.f));
    g.drawText("extreme vocal processor", kPad + 92, 0, 160, kHeader, juce::Justification::centredLeft);

    for (auto& pn : panels) {
        g.setColour(kPanel);
        g.fillRoundedRectangle(pn.bounds.toFloat(), 8.f);
        g.setColour(kEdge);
        g.drawRoundedRectangle(pn.bounds.toFloat().reduced(0.5f), 8.f, 1.f);
        g.setColour(kAccent);
        g.setFont(juce::FontOptions(14.f, juce::Font::bold));
        g.drawText(pn.title, pn.bounds.getX() + 12, pn.bounds.getY(), 140, kTitle, juce::Justification::centredLeft);
    }
}

void RaspEditor::resized() {
    auto top = getLocalBounds().removeFromTop(kHeader).reduced(kPad, 14);
    bypass.setBounds(top.removeFromRight(110));
    top.removeFromRight(8);
    clearNotches.setBounds(top.removeFromRight(120));
    top.removeFromRight(8);
    preset.setBounds(top.removeFromRight(150));
    top.removeFromRight(16);
    throwBtn.setBounds(top.removeFromRight(80));
    top.removeFromRight(8);
    megaBtn.setBounds(top.removeFromRight(80));

    status.setBounds(getLocalBounds().removeFromBottom(30).reduced(kPad + 4, 0));

    int x = kPad;
    for (auto& pn : panels) {
        const int rows = ((int)pn.cells.size() + pn.cols - 1) / pn.cols;
        pn.bounds = {x, kHeader, pn.cols * kCellW + 2 * kPad, kTitle + rows * kCellH + kPad};
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
    status.setText(t, juce::dontSendNotification);
    // Light the hold buttons when automation (not the mouse) engages them.
    megaBtn.setToggleState(proc.apvts.getRawParameterValue("mega")->load() > 0.5f, juce::dontSendNotification);
    throwBtn.setToggleState(proc.apvts.getRawParameterValue("throwOn")->load() > 0.5f, juce::dontSendNotification);
    if (preset.getSelectedId() != proc.getCurrentProgram() + 1)
        preset.setSelectedId(proc.getCurrentProgram() + 1, juce::dontSendNotification);
}
