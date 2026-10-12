#include "PluginEditor.h"

namespace
{
const juce::Colour background { 0xff15171c };
const juce::Colour panel { 0xff22252c };
const juce::Colour accent { 0xffff7a2f };
const juce::Colour textColour { 0xffe8e8ec };
const juce::Colour dimText { 0xff8a8f9a };

constexpr int headerHeight = 56;
constexpr int rowHeight = 150;
constexpr int cellWidth = 90;
constexpr int sectionTitleHeight = 22;
constexpr int margin = 12;
} // namespace

juce::Component& SubForgeEditor::Control::widget()
{
    if (slider != nullptr) return *slider;
    if (combo != nullptr) return *combo;
    return *toggle;
}

SubForgeEditor::SubForgeEditor (SubForgeProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    namespace id = subforge::params;

    lookAndFeel.setColour (juce::Slider::rotarySliderFillColourId, accent);
    lookAndFeel.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3a3e48));
    lookAndFeel.setColour (juce::Slider::thumbColourId, textColour);
    lookAndFeel.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    lookAndFeel.setColour (juce::Slider::textBoxTextColourId, textColour);
    lookAndFeel.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff2e323b));
    lookAndFeel.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff3a3e48));
    lookAndFeel.setColour (juce::ToggleButton::tickColourId, accent);
    lookAndFeel.setColour (juce::Label::textColourId, textColour);
    setLookAndFeel (&lookAndFeel);

    sound.title = "SOUND";
    for (auto* paramId : { id::mode, id::tune, id::punch, id::punchTime, id::decay, id::oneShot, id::release,
                           id::glide, id::click, id::velocity })
        addControl (sound, paramId);

    drop.title = "DROP";
    for (auto* paramId : { id::dropAmount, id::dropLength, id::dropCurve })
        addControl (drop, paramId);

    lowEnd.title = "LOW END";
    for (auto* paramId : { id::lowCut, id::crossover, id::harmonics, id::autoHarmonics, id::drive, id::ceiling })
        addControl (lowEnd, paramId);

    mix.title = "KICK DUCK + OUTPUT";
    for (auto* paramId : { id::duckDepth, id::duckThreshold, id::duckRelease, id::output, id::phoneCheck })
        addControl (mix, paramId);

    punchControl = sound.controls[2];
    punchTimeControl = sound.controls[3];
    releaseControl = sound.controls[6];

    for (auto* meter : { &outputMeter, &subMeter, &duckMeter })
        addAndMakeVisible (meter);

    setSize (margin * 2 + cellWidth * 10 + 24, headerHeight + rowHeight * 3 + margin * 4);
    startTimerHz (30);
}

SubForgeEditor::~SubForgeEditor()
{
    setLookAndFeel (nullptr);
}

SubForgeEditor::Control& SubForgeEditor::addControl (Section& section, const char* paramId)
{
    auto control = std::make_unique<Control>();
    auto* param = processor.apvts.getParameter (paramId);

    control->label.setText (param->getName (32), juce::dontSendNotification);
    control->label.setJustificationType (juce::Justification::centred);
    control->label.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (control->label);

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
    {
        control->combo = std::make_unique<juce::ComboBox>();
        control->combo->addItemList (choice->choices, 1);
        control->comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            processor.apvts, paramId, *control->combo);
    }
    else if (dynamic_cast<juce::AudioParameterBool*> (param) != nullptr)
    {
        control->toggle = std::make_unique<juce::ToggleButton> ("On");
        control->buttonAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            processor.apvts, paramId, *control->toggle);
    }
    else
    {
        control->slider = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
        control->slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, cellWidth - 6, 18);
        control->sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.apvts, paramId, *control->slider);
    }

    addAndMakeVisible (control->widget());
    section.controls.push_back (control.get());
    controls.push_back (std::move (control));
    return *controls.back();
}

void SubForgeEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    g.setColour (accent);
    g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    g.drawText ("SUBFORGE", margin, 10, 200, 30, juce::Justification::centredLeft);

    g.setColour (dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("808 booms + sub drops that sit in the mix", margin + 150, 10, 400, 30, juce::Justification::centredLeft);

    for (auto* section : { &sound, &drop, &lowEnd, &mix })
    {
        g.setColour (panel);
        g.fillRoundedRectangle (section->bounds.toFloat(), 8.0f);
        g.setColour (dimText);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (section->title, section->bounds.reduced (10, 4).removeFromTop (sectionTitleHeight - 4),
                    juce::Justification::centredLeft);
    }
}

void SubForgeEditor::resized()
{
    auto area = getLocalBounds().reduced (margin);
    area.removeFromTop (headerHeight - margin);

    const auto layoutSection = [] (Section& section, juce::Rectangle<int> bounds)
    {
        section.bounds = bounds;
        auto inner = bounds.reduced (6, 4);
        inner.removeFromTop (sectionTitleHeight);

        for (auto* control : section.controls)
        {
            auto cell = inner.removeFromLeft (cellWidth);
            control->label.setBounds (cell.removeFromTop (18));

            if (control->slider != nullptr)
                control->widget().setBounds (cell.reduced (2));
            else
                control->widget().setBounds (cell.withSizeKeepingCentre (cellWidth - 10, 26));
        }
    };

    const auto sectionWidth = [] (const Section& s) { return (int) s.controls.size() * cellWidth + 12; };

    auto row1 = area.removeFromTop (rowHeight);
    layoutSection (sound, row1.removeFromLeft (sectionWidth (sound)));

    area.removeFromTop (margin);
    auto row2 = area.removeFromTop (rowHeight);
    layoutSection (drop, row2.removeFromLeft (sectionWidth (drop)));
    row2.removeFromLeft (margin);
    layoutSection (lowEnd, row2.removeFromLeft (sectionWidth (lowEnd)));

    area.removeFromTop (margin);
    auto row3 = area.removeFromTop (rowHeight);
    layoutSection (mix, row3.removeFromLeft (sectionWidth (mix)));
    row3.removeFromLeft (margin * 2);

    const int meterWidth = 70;
    for (auto* meter : { &outputMeter, &subMeter, &duckMeter })
    {
        meter->setBounds (row3.removeFromLeft (meterWidth));
        row3.removeFromLeft (margin);
    }
}

void SubForgeEditor::timerCallback()
{
    // Meters: jump up instantly, fall back smoothly.
    const float out = processor.meterOutputPeakDb.exchange (-180.0f);
    const float sub = processor.meterSubReductionDb.exchange (0.0f);
    const float duck = processor.meterDuckReductionDb.exchange (0.0f);

    shownOutput = juce::jmax (out, shownOutput - 1.5f);
    shownSub = juce::jmax (sub, shownSub - 0.6f);
    shownDuck = juce::jmax (duck, shownDuck - 0.6f);

    outputMeter.setValue (shownOutput);
    subMeter.setValue (shownSub);
    duckMeter.setValue (shownDuck);
    outputMeter.setMarker (processor.apvts.getRawParameterValue (subforge::params::ceiling)->load()
                           + processor.apvts.getRawParameterValue (subforge::params::output)->load());

    // Fade out the controls that don't apply to the current mode.
    const bool dropMode = processor.apvts.getRawParameterValue (subforge::params::mode)->load() > 0.5f;
    const bool oneShot = processor.apvts.getRawParameterValue (subforge::params::oneShot)->load() > 0.5f;

    const auto setDim = [] (Control* c, bool dim)
    {
        const float alpha = dim ? 0.35f : 1.0f;
        c->widget().setAlpha (alpha);
        c->label.setAlpha (alpha);
    };

    setDim (punchControl, dropMode);
    setDim (punchTimeControl, dropMode);
    setDim (releaseControl, oneShot);

    for (auto* c : drop.controls)
        setDim (c, ! dropMode);
}

//==============================================================================
SubForgeEditor::Meter::Meter (juce::String n, float lo, float hi, bool isReduction)
    : name (std::move (n)), minDb (lo), maxDb (hi), value (isReduction ? 0.0f : lo), marker (lo), reduction (isReduction)
{
}

void SubForgeEditor::Meter::setValue (float db)
{
    if (std::abs (db - value) > 0.05f)
    {
        value = db;
        repaint();
    }
}

void SubForgeEditor::Meter::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();

    g.setColour (dimText);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText (name, area.removeFromTop (18), juce::Justification::centred);

    auto readout = area.removeFromBottom (18);
    auto bar = area.reduced (18, 4);

    g.setColour (panel);
    g.fillRoundedRectangle (bar.toFloat(), 4.0f);

    const auto toY = [this, &bar] (float db)
    {
        const float norm = juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
        return reduction ? bar.getY() + norm * bar.getHeight() : bar.getBottom() - norm * bar.getHeight();
    };

    const float y = toY (value);
    juce::Rectangle<float> fill = reduction
        ? juce::Rectangle<float> ((float) bar.getX(), (float) bar.getY(), (float) bar.getWidth(), y - bar.getY())
        : juce::Rectangle<float> ((float) bar.getX(), y, (float) bar.getWidth(), bar.getBottom() - y);

    g.setColour (reduction ? juce::Colour (0xff4fb3ff) : accent);
    g.fillRoundedRectangle (fill, 4.0f);

    if (! reduction)
    {
        g.setColour (textColour.withAlpha (0.8f));
        const float markerY = toY (marker);
        g.drawHorizontalLine (juce::roundToInt (markerY), (float) bar.getX() - 4.0f, (float) bar.getRight() + 4.0f);
    }

    g.setColour (textColour);
    g.setFont (juce::FontOptions (12.0f));
    const juce::String text = reduction ? (value > 0.05f ? "-" + juce::String (value, 1) + " dB" : "0.0 dB")
                                        : (value > -100.0f ? juce::String (value, 1) + " dB" : "-inf");
    g.drawText (text, readout, juce::Justification::centred);
}
