#include "PluginEditor.h"
#include <cstring>

namespace {
const juce::Colour kBg(0xff141619), kPanel(0xff202329), kSteel(0xff3a3f47), kText(0xffe9e6e1),
    kDim(0xff8b8f96), kAmber(0xffffb020), kRed(0xffe5533d);
const char* kModeIds[axe::kNumModes][6] = {
    {"tight", "punch", "release", "low", "mid", "high"},
    {"sub", "dry", "subTone", "range", "grit", nullptr},
    {"bright", "clarity", "air", "matchAmount", nullptr, nullptr},
    {"gain", "lows", "highs", "scoop", "level", "blend"},
};
const char* kModeBlurb[axe::kNumModes] = {
    "3-band compressor: palm mutes stay tight, the lows don't flub",
    "Octave down under the DI. Poly holds chords; Mono is cleanest on single notes",
    "Fresh-strings zing: harmonics, pick clarity, air, and a learned match",
    "HM-2-style chainsaw: big lows and 1 kHz, scooped middle",
};
}  // namespace

AxeLook::AxeLook() {
    setColour(juce::Slider::textBoxTextColourId, kText);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::textColourId, kText);
    setColour(juce::TextButton::textColourOffId, kText);
    setColour(juce::TextButton::textColourOnId, kBg);
}

void AxeLook::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) {
    auto r = juce::Rectangle<float>((float)x, (float)y, (float)w, (float)h).reduced(6.f);
    const float rad = juce::jmin(r.getWidth(), r.getHeight()) * 0.5f;
    const auto c = r.getCentre();
    // brushed-steel cap
    g.setGradientFill(juce::ColourGradient(kSteel.brighter(0.35f), c.x, c.y - rad, kSteel.darker(0.6f), c.x, c.y + rad, false));
    g.fillEllipse(c.x - rad * 0.72f, c.y - rad * 0.72f, rad * 1.44f, rad * 1.44f);
    // track + value arc
    juce::Path track, value;
    track.addCentredArc(c.x, c.y, rad * 0.9f, rad * 0.9f, 0.f, a0, a1, true);
    g.setColour(kPanel.brighter(0.15f));
    g.strokePath(track, juce::PathStrokeType(3.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const float a = a0 + pos * (a1 - a0);
    value.addCentredArc(c.x, c.y, rad * 0.9f, rad * 0.9f, 0.f, a0, a, true);
    g.setColour(kAmber);
    g.strokePath(value, juce::PathStrokeType(3.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // pointer
    juce::Path p;
    p.addRoundedRectangle(-1.5f, -rad * 0.68f, 3.f, rad * 0.32f, 1.f);
    g.setColour(kText);
    g.fillPath(p, juce::AffineTransform::rotation(a).translated(c.x, c.y));
}

void AxeLook::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) {
    auto r = b.getLocalBounds().toFloat().reduced(2.f);
    const bool on = b.getToggleState();
    g.setColour(on ? kAmber : (down ? kSteel.brighter(0.2f) : (over ? kSteel.brighter(0.1f) : kSteel)));
    g.fillRoundedRectangle(r, r.getHeight() < 40.f ? 5.f : r.getHeight() * 0.5f);
    g.setColour(on ? kAmber.brighter(0.4f) : kSteel.brighter(0.3f));
    g.drawRoundedRectangle(r, r.getHeight() < 40.f ? 5.f : r.getHeight() * 0.5f, 1.2f);
}

void AxeLook::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle, juce::Slider& s) {
    auto r = juce::Rectangle<float>((float)x, (float)y, (float)w, (float)h);
    const float mid = s.valueToProportionOfLength(0.0) * r.getHeight();
    const float cx = r.getCentreX();
    g.setColour(kPanel.brighter(0.15f));
    g.fillRoundedRectangle(cx - 2.f, r.getY(), 4.f, r.getHeight(), 2.f);
    const float y0 = r.getBottom() - mid, y1 = pos;
    g.setColour(kAmber);
    g.fillRect(cx - 2.f, juce::jmin(y0, y1), 4.f, std::abs(y1 - y0));
    g.setColour(kText);
    g.fillRoundedRectangle(cx - 7.f, y1 - 2.f, 14.f, 4.f, 2.f);
}

Knob::Knob(juce::AudioProcessorValueTreeState& s, const juce::String& id, const juce::String& title)
    : att(s, id, slider) {
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 80, 18);
    addAndMakeVisible(slider);
    name.setText(title.toUpperCase(), juce::dontSendNotification);
    name.setJustificationType(juce::Justification::centred);
    name.setFont(juce::FontOptions(12.f, juce::Font::bold));
    name.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(name);
}

void Knob::resized() {
    auto r = getLocalBounds();
    name.setBounds(r.removeFromTop(16));
    slider.setBounds(r);
}

AxeEditor::AxeEditor(AxeProcessor& p)
    : AudioProcessorEditor(&p), proc(p), bypassAtt(p.apvts, "bypass", bypassBtn) {
    setLookAndFeel(&look);
    const auto& specs = paramSpecs();
    auto titleOf = [&](const char* id) {
        for (const auto& s : specs) if (!std::strcmp(s.id, id)) return juce::String(s.name);
        return juce::String(id);
    };
    for (int m = 0; m < axe::kNumModes; m++) {
        floors[m].setButtonText(juce::String(m + 1));
        floors[m].setTooltip(axe::modeName(m));
        floors[m].onClick = [this, m] { setChoice("mode", m); };
        addAndMakeVisible(floors[m]);
        for (const char* id : kModeIds[m]) {
            if (!id) continue;
            modeKnobs[m].push_back(std::make_unique<Knob>(p.apvts, id, titleOf(id)));
            addChildComponent(*modeKnobs[m].back());
        }
    }
    for (const char* id : {"input", "gate", "mix", "output"}) {
        globalKnobs.push_back(std::make_unique<Knob>(p.apvts, id, titleOf(id)));
        addAndMakeVisible(*globalKnobs.back());
    }
    guitarBtn.onClick = [this] { setChoice("instrument", 0); };
    bassBtn.onClick = [this] { setChoice("instrument", 1); };
    polyBtn.onClick = [this] { setChoice("subType", 0); };
    monoBtn.onClick = [this] { setChoice("subType", 1); };
    bypassBtn.setClickingTogglesState(true);
    for (auto* b : {&guitarBtn, &bassBtn, &bypassBtn}) addAndMakeVisible(*b);
    for (auto* b : {&polyBtn, &monoBtn}) addChildComponent(*b);

    learnFreshBtn.setTooltip("Play ~12 s on fresh strings: stores what they sound like");
    learnNowBtn.setTooltip("Play ~12 s on your current strings: matches them to the stored fresh sound");
    learnFreshBtn.onClick = [this] { proc.pulseLearn("learnFresh"); };
    learnNowBtn.onClick = [this] { proc.pulseLearn("learnNow"); };
    addChildComponent(learnFreshBtn);
    addChildComponent(learnNowBtn);
    learnStatus.setFont(juce::FontOptions(12.f));
    learnStatus.setColour(juce::Label::textColourId, kDim);
    addChildComponent(learnStatus);
    for (int b = 0; b < axe::kMatchBands; b++) {
        auto s = std::make_unique<juce::Slider>(juce::Slider::LinearVertical, juce::Slider::NoTextBox);
        s->setTooltip(titleOf(("match" + std::to_string(b)).c_str()));
        matchAtts.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.apvts, "match" + juce::String(b), *s));
        addChildComponent(*s);
        matchSliders.push_back(std::move(s));
    }
    setSize(760, 480);
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
        for (auto& k : modeKnobs[m]) k->setVisible(m == mode);
    }
    polyBtn.setVisible(mode == axe::kSub);
    monoBtn.setVisible(mode == axe::kSub);
    for (auto* c : std::initializer_list<juce::Component*>{&learnFreshBtn, &learnNowBtn, &learnStatus}) c->setVisible(mode == axe::kStrings);
    for (auto& s : matchSliders) s->setVisible(mode == axe::kStrings);
    resized();
    repaint();
}

void AxeEditor::paint(juce::Graphics& g) {
    g.fillAll(kBg);
    // panel plate
    auto plate = getLocalBounds().reduced(10).toFloat();
    g.setColour(kPanel);
    g.fillRoundedRectangle(plate, 10.f);
    // title
    g.setColour(kText);
    g.setFont(juce::FontOptions(20.f, juce::Font::bold));
    g.drawText("AXE ELEVATOR", 30, 18, 300, 26, juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(11.f));
    g.drawText("SNOTCH  -  DI ENHANCER", 30, 42, 300, 14, juce::Justification::centredLeft);
    // floor indicator
    const int mode = shownMode < 0 ? 0 : shownMode;
    g.setColour(juce::Colours::black);
    g.fillRoundedRectangle(indicatorArea.toFloat(), 6.f);
    g.setColour(kAmber);
    g.setFont(juce::FontOptions(34.f, juce::Font::bold));
    auto ind = indicatorArea;
    g.drawText(juce::String(mode + 1), ind.removeFromLeft(54), juce::Justification::centred);
    g.setFont(juce::FontOptions(17.f, juce::Font::bold));
    g.drawText(juce::String(axe::modeName(mode)).toUpperCase(), ind.reduced(4, 0), juce::Justification::centredLeft);
    // up-arrow beside the floor buttons
    g.setColour(kDim);
    g.setFont(juce::FontOptions(11.f));
    g.drawText(kModeBlurb[mode], pageArea.getX(), pageArea.getY() - 22, pageArea.getWidth(), 16, juce::Justification::centredLeft);
    // Tight: gain reduction per band
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
            g.fillRect(bar.withHeight((int)(bar.getHeight() * gr / 18.f)));
            g.setColour(kDim);
            g.drawText(juce::String(names[b]) + "  -" + juce::String(gr, 1) + " dB", cell.reduced(6, 0), juce::Justification::centredLeft);
        }
    }
    if (mode == axe::kStrings) {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(10.f));
        const char* hz[axe::kMatchBands] = {"250", "500", "1k", "1.6k", "2.5k", "4k", "6.3k", "10k"};
        const int w = matchArea.getWidth() / axe::kMatchBands;
        for (int b = 0; b < axe::kMatchBands; b++)
            g.drawText(hz[b], matchArea.getX() + b * w, matchArea.getBottom(), w, 14, juce::Justification::centred);
        g.drawText("LEARNED MATCH (dB)", matchArea.getX(), matchArea.getY() - 16, matchArea.getWidth(), 14, juce::Justification::centredLeft);
    }
    if (proc.meters.gate.load() > 6.f) {
        g.setColour(kRed);
        g.fillEllipse((float)globalKnobs[1]->getRight() - 14.f, (float)globalKnobs[1]->getY() + 2.f, 8.f, 8.f);
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
    indicatorArea = top.removeFromRight(210).reduced(0, 4);

    auto bottom = r.removeFromBottom(110);
    const int gw = bottom.getWidth() / 4;
    for (auto& k : globalKnobs) k->setBounds(bottom.removeFromLeft(gw).reduced(18, 0));

    r.removeFromTop(10);
    auto floorCol = r.removeFromLeft(86);
    const int fh = juce::jmin(56, floorCol.getHeight() / axe::kNumModes);
    for (int m = axe::kNumModes - 1; m >= 0; m--) {   // floor 4 at the top, like a lift panel
        auto cell = floorCol.removeFromTop(fh);
        floors[m].setBounds(cell.withSizeKeepingCentre(fh - 8, fh - 8));
    }
    r.removeFromLeft(14);
    r.removeFromTop(22);
    pageArea = r;

    const int mode = shownMode < 0 ? 0 : shownMode;
    auto page = r;
    if (mode == axe::kSub) {
        auto row = page.removeFromTop(30);
        polyBtn.setBounds(row.removeFromLeft(90).reduced(2));
        monoBtn.setBounds(row.removeFromLeft(90).reduced(2));
        page.removeFromTop(6);
    }
    if (mode == axe::kStrings) {
        auto right = page.removeFromRight(250);
        auto row = right.removeFromTop(32);
        learnFreshBtn.setBounds(row.removeFromLeft(124).reduced(2));
        learnNowBtn.setBounds(row.reduced(2));
        learnStatus.setBounds(right.removeFromTop(22));
        right.removeFromTop(22);
        matchArea = right.removeFromTop(right.getHeight() - 16);
        const int w = matchArea.getWidth() / axe::kMatchBands;
        for (int b = 0; b < axe::kMatchBands; b++) matchSliders[(size_t)b]->setBounds(matchArea.getX() + b * w, matchArea.getY(), w, matchArea.getHeight());
    }
    if (mode == axe::kTight) meterArea = page.removeFromBottom(22);
    auto& ks = modeKnobs[mode];
    const int perRow = 3, rows = ((int)ks.size() + perRow - 1) / perRow;
    const int rh = page.getHeight() / juce::jmax(1, rows);
    for (size_t i = 0; i < ks.size(); i++) {
        const int row = (int)i / perRow, col = (int)i % perRow;
        const int cw = page.getWidth() / perRow;
        ks[i]->setBounds(page.getX() + col * cw, page.getY() + row * rh, cw, rh);
        ks[i]->setBounds(ks[i]->getBounds().reduced(10, 2));
    }
}

void AxeEditor::timerCallback() {
    const int m = currentMode();
    if (m != shownMode) showMode(m);
    const int inst = (int)std::lround(proc.apvts.getRawParameterValue("instrument")->load());
    guitarBtn.setToggleState(inst == 0, juce::dontSendNotification);
    bassBtn.setToggleState(inst == 1, juce::dontSendNotification);
    const int st = (int)std::lround(proc.apvts.getRawParameterValue("subType")->load());
    polyBtn.setToggleState(st == 0, juce::dontSendNotification);
    monoBtn.setToggleState(st == 1, juce::dontSendNotification);
    if (m == axe::kStrings) {
        const int what = proc.meters.learning.load();
        juce::String t;
        if (what == 1) t = "Learning fresh strings... " + juce::String(juce::roundToInt(proc.meters.learnProgress.load() * 100.f)) + "%";
        else if (what == 2) t = "Matching your strings... " + juce::String(juce::roundToInt(proc.meters.learnProgress.load() * 100.f)) + "%";
        else t = proc.meters.hasFresh.load() ? "Fresh strings stored. Learn Now to match." : "Learn Fresh on new strings first.";
        learnStatus.setText(t, juce::dontSendNotification);
        learnFreshBtn.setToggleState(what == 1, juce::dontSendNotification);
        learnNowBtn.setToggleState(what == 2, juce::dontSendNotification);
        learnNowBtn.setEnabled(proc.meters.hasFresh.load());
    }
    repaint(meterArea);
    repaint(globalKnobs[1]->getBounds());
}
