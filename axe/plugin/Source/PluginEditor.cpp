#include "PluginEditor.h"
#include <cstring>

namespace {
const juce::Colour kBg(0xff141619), kPanel(0xff202329), kSteel(0xff3a3f47), kText(0xffe9e6e1),
    kDim(0xff8b8f96), kAmber(0xffffb020), kRed(0xffe5533d);
const char* kModeBlurb[axe::kNumModes] = {
    "3-band compressor: palm mutes stay tight, the lows don't flub",
    "Octave down under your playing. Poly holds chords; Mono is cleanest on single notes",
    "HM-2-style chainsaw: big lows and 1 kHz, scooped middle",
    "Beat repeat on the Rate grid: rolls, pitch jumps, tape stops, reverse. TRIGGER holds it",
    "Every pick fires a falling pew. TRIGGER: machine gun on the Rate",
    "Pitch dives Depth semitones over Dive Time, comes back over Return. TRIGGER holds the dive",
    "Pulse-wave game voice that follows your note. TRIGGER runs the arpeggiator",
    "Rhythmic chopping on the Rate grid. TRIGGER: double-time roll",
    "Ring mod or frequency shifter. TRIGGER: the carrier jumps on the Rate",
    "Holds what you just played as an endless pad. TRIGGER freezes; Always re-freezes on every pick",
    "Amp feedback on a harmonic of your note. TRIGGER swells it; Always swells on notes left ringing",
    "Granular cloud of what you just played. TRIGGER: 4x burst",
    "Shortwave broadcast. TRIGGER spins the dial (and re-tunes on the Rate while held)",
};
}  // namespace

AxeLook::AxeLook() {
    setColour(juce::Slider::textBoxTextColourId, kText);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::textColourId, kText);
    setColour(juce::TextButton::textColourOffId, kText);
    setColour(juce::TextButton::textColourOnId, kBg);
    setColour(juce::ComboBox::backgroundColourId, kSteel);
    setColour(juce::ComboBox::outlineColourId, kSteel.brighter(0.3f));
    setColour(juce::ComboBox::textColourId, kText);
    setColour(juce::PopupMenu::backgroundColourId, kPanel);
}

void AxeLook::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) {
    auto r = juce::Rectangle<float>((float)x, (float)y, (float)w, (float)h).reduced(4.f);
    const float rad = juce::jmin(r.getWidth(), r.getHeight()) * 0.5f;
    const auto c = r.getCentre();
    g.setGradientFill(juce::ColourGradient(kSteel.brighter(0.35f), c.x, c.y - rad, kSteel.darker(0.6f), c.x, c.y + rad, false));
    g.fillEllipse(c.x - rad * 0.72f, c.y - rad * 0.72f, rad * 1.44f, rad * 1.44f);
    juce::Path track, value;
    track.addCentredArc(c.x, c.y, rad * 0.9f, rad * 0.9f, 0.f, a0, a1, true);
    g.setColour(kPanel.brighter(0.15f));
    g.strokePath(track, juce::PathStrokeType(3.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const float a = a0 + pos * (a1 - a0);
    value.addCentredArc(c.x, c.y, rad * 0.9f, rad * 0.9f, 0.f, a0, a, true);
    g.setColour(kAmber);
    g.strokePath(value, juce::PathStrokeType(3.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    juce::Path p;
    p.addRoundedRectangle(-1.5f, -rad * 0.68f, 3.f, rad * 0.32f, 1.f);
    g.setColour(kText);
    g.fillPath(p, juce::AffineTransform::rotation(a).translated(c.x, c.y));
}

void AxeLook::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) {
    auto r = b.getLocalBounds().toFloat().reduced(2.f);
    const bool on = b.getToggleState();
    const bool trig = b.getButtonText() == "TRIGGER";
    const juce::Colour hot = trig ? kRed : kAmber;
    g.setColour(on ? hot : (down ? kSteel.brighter(0.2f) : (over ? kSteel.brighter(0.1f) : kSteel)));
    const float rr = r.getHeight() < 40.f ? 5.f : r.getHeight() * 0.5f;
    g.fillRoundedRectangle(r, rr);
    g.setColour(on ? hot.brighter(0.4f) : kSteel.brighter(0.3f));
    g.drawRoundedRectangle(r, rr, 1.2f);
}

ParamControl::ParamControl(juce::AudioProcessorValueTreeState& s, const ParamSpec& spec) {
    name.setText(juce::String(spec.name).toUpperCase(), juce::dontSendNotification);
    name.setJustificationType(juce::Justification::centred);
    name.setFont(juce::FontOptions(11.f, juce::Font::bold));
    name.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(name);
    switch (spec.kind) {
        case ParamSpec::Float:
            knob = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 18);
            knobAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(s, spec.id, *knob);
            addAndMakeVisible(*knob);
            break;
        case ParamSpec::Choice:
            menu = std::make_unique<juce::ComboBox>();
            menu->addItemList(juce::StringArray::fromTokens(spec.choices, "|", ""), 1);
            menuAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(s, spec.id, *menu);
            addAndMakeVisible(*menu);
            break;
        case ParamSpec::Bool:
            toggle = std::make_unique<juce::TextButton>(juce::String(spec.name).toUpperCase());
            toggle->setClickingTogglesState(true);
            toggleAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(s, spec.id, *toggle);
            addAndMakeVisible(*toggle);
            name.setVisible(false);
            break;
    }
}

void ParamControl::resized() {
    auto r = getLocalBounds();
    if (toggle) { toggle->setBounds(r.withSizeKeepingCentre(juce::jmin(r.getWidth(), 120), 30)); return; }
    name.setBounds(r.removeFromTop(16));
    if (knob) knob->setBounds(r);
    if (menu) menu->setBounds(r.withSizeKeepingCentre(juce::jmin(r.getWidth() - 8, 150), 26));
}

void TriggerButton::set(bool on) {
    param.beginChangeGesture();
    param.setValueNotifyingHost(on ? 1.f : 0.f);
    param.endChangeGesture();
}
void TriggerButton::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isShiftDown() || e.mods.isPopupMenu()) { latched = !latched; set(latched); return; }
    latched = false;
    set(true);
}
void TriggerButton::mouseUp(const juce::MouseEvent& e) {
    if (e.mods.isShiftDown() || e.mods.isPopupMenu() || latched) return;
    set(false);
}

AxeEditor::AxeEditor(AxeProcessor& p)
    : AudioProcessorEditor(&p), proc(p), bypassAtt(p.apvts, "bypass", bypassBtn),
      triggerBtn(*p.apvts.getParameter("trigger")) {
    setLookAndFeel(&look);
    for (int m = 0; m < axe::kNumModes; m++) {
        floors[m].setButtonText(juce::String(m + 1));
        floors[m].setTooltip(axe::modeName(m));
        floors[m].onClick = [this, m] { setChoice("mode", m); };
        addAndMakeVisible(floors[m]);
    }
    for (const auto& s : paramSpecs()) {
        if (s.mode >= 0) {
            modeControls[s.mode].push_back(std::make_unique<ParamControl>(p.apvts, s));
            addChildComponent(*modeControls[s.mode].back());
        } else if (!std::strcmp(s.page, "Global")) {
            if (!std::strcmp(s.id, "mode") || !std::strcmp(s.id, "instrument")) continue;   // floor buttons / top switches
            globalControls.push_back(std::make_unique<ParamControl>(p.apvts, s));
            addAndMakeVisible(*globalControls.back());
        } else {
            if (!std::strcmp(s.id, "trigger")) continue;   // the big button
            triggerControls.push_back(std::make_unique<ParamControl>(p.apvts, s));
            addAndMakeVisible(*triggerControls.back());
        }
    }
    guitarBtn.onClick = [this] { setChoice("instrument", 0); };
    bassBtn.onClick = [this] { setChoice("instrument", 1); };
    bypassBtn.setClickingTogglesState(true);
    triggerBtn.setTooltip("Hold to fire. Shift-click (or right-click) to latch.");
    for (auto* b : {&guitarBtn, &bassBtn, &bypassBtn}) addAndMakeVisible(*b);
    addAndMakeVisible(triggerBtn);
    setSize(980, 620);
    showMode(currentMode());
    startTimerHz(20);
}

AxeEditor::~AxeEditor() { setLookAndFeel(nullptr); }

int AxeEditor::currentMode() const { return (int)std::lround(proc.apvts.getRawParameterValue("mode")->load()); }

void AxeEditor::setChoice(const char* id, int index) {
    if (auto* prm = proc.apvts.getParameter(id)) {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost(prm->convertTo0to1((float)index));
        prm->endChangeGesture();
    }
}

void AxeEditor::showMode(int mode) {
    shownMode = mode;
    for (int m = 0; m < axe::kNumModes; m++) {
        floors[m].setToggleState(m == mode, juce::dontSendNotification);
        for (auto& c : modeControls[m]) c->setVisible(m == mode);
    }
    resized();
    repaint();
}

void AxeEditor::paint(juce::Graphics& g) {
    g.fillAll(kBg);
    g.setColour(kPanel);
    g.fillRoundedRectangle(getLocalBounds().reduced(10).toFloat(), 10.f);
    g.setColour(kText);
    g.setFont(juce::FontOptions(20.f, juce::Font::bold));
    g.drawText("AXE ELEVATOR", 30, 18, 300, 26, juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(11.f));
    g.drawText("SNOTCH  -  PRE-AMP MANGLER", 30, 42, 300, 14, juce::Justification::centredLeft);
    const int mode = shownMode < 0 ? 0 : shownMode;
    // floor indicator
    g.setColour(juce::Colours::black);
    g.fillRoundedRectangle(indicatorArea.toFloat(), 6.f);
    g.setColour(kAmber);
    g.setFont(juce::FontOptions(34.f, juce::Font::bold));
    auto ind = indicatorArea;
    g.drawText(juce::String(mode + 1), ind.removeFromLeft(60), juce::Justification::centred);
    g.setFont(juce::FontOptions(17.f, juce::Font::bold));
    g.drawText(juce::String(axe::modeName(mode)).toUpperCase(), ind.reduced(4, 0), juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(12.f));
    g.drawText(kModeBlurb[mode], pageArea.getX(), pageArea.getY() - 24, pageArea.getWidth(), 18, juce::Justification::centredLeft);
    // the step lights: 8 steps of the Rate grid, the current one lit
    {
        const int st = proc.meters.step.load();
        const int w = stepArea.getWidth() / 8;
        for (int i = 0; i < 8; i++) {
            const bool lit = ((st % 8) + 8) % 8 == i;
            g.setColour(lit ? (proc.meters.triggered.load() ? kRed : kAmber) : kSteel);
            g.fillRoundedRectangle((float)(stepArea.getX() + i * w + 2), (float)stepArea.getY(), (float)(w - 4), (float)stepArea.getHeight(), 3.f);
        }
        g.setColour(kDim);
        g.setFont(juce::FontOptions(11.f));
        const juce::String bpm = juce::String(proc.meters.bpm.load(), 1) + " BPM" + (proc.meters.hostTempo.load() ? " (host)" : "");
        g.drawText(bpm, stepArea.getX(), stepArea.getBottom() + 2, stepArea.getWidth(), 14, juce::Justification::centred);
    }
    if (mode == axe::kTight) {
        const char* names[3] = {"LOW", "MID", "HIGH"};
        auto m = meterArea;
        const int bw = m.getWidth() / 3;
        for (int b = 0; b < 3; b++) {
            auto cell = m.removeFromLeft(bw).reduced(8, 0);
            auto bar = cell.removeFromLeft(12);
            g.setColour(kBg);
            g.fillRect(bar);
            const float gr = juce::jlimit(0.f, 18.f, proc.meters.gr[b].load());
            g.setColour(kAmber);
            g.fillRect(bar.withHeight((int)((float)bar.getHeight() * gr / 18.f)));
            g.setColour(kDim);
            g.drawText(juce::String(names[b]) + "  -" + juce::String(gr, 1) + " dB", cell.reduced(6, 0), juce::Justification::centredLeft);
        }
    }
    const float hz = proc.meters.pitch.load();
    if (hz > 0.f && (mode == axe::kChip || mode == axe::kFeedback || mode == axe::kLaser || mode == axe::kRing)) {
        static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const int midi = (int)std::lround(69.f + 12.f * std::log2(hz / 440.f));
        g.setColour(kDim);
        g.drawText("NOTE " + juce::String(names[((midi % 12) + 12) % 12]) + juce::String(midi / 12 - 1), meterArea, juce::Justification::centredLeft);
    }
}

void AxeEditor::resized() {
    auto r = getLocalBounds().reduced(20);
    auto top = r.removeFromTop(48);
    bypassBtn.setBounds(top.removeFromRight(90).reduced(0, 8));
    top.removeFromRight(16);
    bassBtn.setBounds(top.removeFromRight(80).reduced(0, 8));
    guitarBtn.setBounds(top.removeFromRight(80).reduced(0, 8));
    top.removeFromRight(16);
    indicatorArea = top.removeFromRight(260).reduced(0, 4);

    // bottom: trigger strip, then the global controls
    auto bottom = r.removeFromBottom(220);
    auto trig = bottom.removeFromTop(110);
    triggerBtn.setBounds(trig.removeFromLeft(150).reduced(8, 14));
    stepArea = trig.removeFromRight(170).reduced(8, 0).withHeight(14).translated(0, 40);
    const int tw = trig.getWidth() / juce::jmax(1, (int)triggerControls.size());
    for (auto& c : triggerControls) c->setBounds(trig.removeFromLeft(tw).reduced(6, 0));
    bottom.removeFromTop(6);
    const int gw = bottom.getWidth() / juce::jmax(1, (int)globalControls.size());
    for (auto& c : globalControls) c->setBounds(bottom.removeFromLeft(gw).reduced(20, 0));

    // floors: two columns, 1 at the bottom left like a lift panel
    r.removeFromTop(8);
    auto floorCol = r.removeFromLeft(120);
    const int rows = (axe::kNumModes + 1) / 2;
    const int fh = juce::jmin(46, floorCol.getHeight() / rows);
    for (int m = 0; m < axe::kNumModes; m++) {
        const int row = rows - 1 - m / 2, col = m % 2;
        floors[m].setBounds(floorCol.getX() + col * 60 + 4, floorCol.getY() + row * fh + 2, 52, fh - 4);
    }
    r.removeFromLeft(20);
    r.removeFromTop(26);
    pageArea = r;

    const int mode = shownMode < 0 ? 0 : shownMode;
    auto page = r;
    if (mode == axe::kTight) meterArea = page.removeFromBottom(22);
    else meterArea = page.removeFromBottom(22);
    auto& cs = modeControls[mode];
    const int perRow = 3, nRows = ((int)cs.size() + perRow - 1) / perRow;
    const int rh = page.getHeight() / juce::jmax(1, nRows), cw = page.getWidth() / perRow;
    for (size_t i = 0; i < cs.size(); i++) {
        const int row = (int)i / perRow, col = (int)i % perRow;
        cs[i]->setBounds(juce::Rectangle<int>(page.getX() + col * cw, page.getY() + row * rh, cw, rh).reduced(14, 2));
    }
}

void AxeEditor::timerCallback() {
    const int m = currentMode();
    if (m != shownMode) showMode(m);
    const int inst = (int)std::lround(proc.apvts.getRawParameterValue("instrument")->load());
    guitarBtn.setToggleState(inst == 0, juce::dontSendNotification);
    bassBtn.setToggleState(inst == 1, juce::dontSendNotification);
    triggerBtn.setToggleState(proc.apvts.getRawParameterValue("trigger")->load() > 0.5f, juce::dontSendNotification);
    repaint(stepArea.expanded(0, 18));
    repaint(meterArea);
}
