#include "DuckEditor.h"

namespace
{
const juce::Colour background { 0xff15171c };
const juce::Colour panel { 0xff22252c };
const juce::Colour accent { 0xffff7a2f };
const juce::Colour duckBlue { 0xff4fb3ff };
const juce::Colour textColour { 0xffe8e8ec };
const juce::Colour dimText { 0xff8a8f9a };

constexpr int margin = 12;
constexpr int headerHeight = 56;
constexpr int cellWidth = 90;
constexpr int rowHeight = 140;
constexpr int meterWidth = 90;
} // namespace

SubForgeDuckEditor::SubForgeDuckEditor (SubForgeDuckProcessor& p)
    : AudioProcessorEditor (&p), duckProcessor (p)
{
    namespace id = subforge::duckparams;

    lookAndFeel.setColour (juce::Slider::rotarySliderFillColourId, duckBlue);
    lookAndFeel.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3a3e48));
    lookAndFeel.setColour (juce::Slider::thumbColourId, textColour);
    lookAndFeel.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    lookAndFeel.setColour (juce::Slider::textBoxTextColourId, textColour);
    lookAndFeel.setColour (juce::ToggleButton::tickColourId, duckBlue);
    lookAndFeel.setColour (juce::Label::textColourId, textColour);
    lookAndFeel.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff2c3038));
    lookAndFeel.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff3a3e48));
    lookAndFeel.setColour (juce::ComboBox::textColourId, textColour);
    lookAndFeel.setColour (juce::ComboBox::arrowColourId, duckBlue);
    lookAndFeel.setColour (juce::PopupMenu::backgroundColourId, panel);
    lookAndFeel.setColour (juce::PopupMenu::highlightedBackgroundColourId, duckBlue.withAlpha (0.4f));
    setLookAndFeel (&lookAndFeel);

    for (auto* paramId : { id::split, id::fullBand, id::depth, id::threshold, id::release, id::rollHold, id::rollWindow, id::rollDepth,
                           id::lookAhead, id::gap })
    {
        auto c = std::make_unique<Control>();
        auto* param = duckProcessor.apvts.getParameter (paramId);
        c->label.setText (param->getName (32), juce::dontSendNotification);
        c->label.setJustificationType (juce::Justification::centred);
        c->label.setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (c->label);

        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
        {
            c->combo = std::make_unique<juce::ComboBox>();
            c->combo->addItemList (choice->choices, 1);
            c->comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
                duckProcessor.apvts, paramId, *c->combo);
        }
        else if (dynamic_cast<juce::AudioParameterBool*> (param) != nullptr)
        {
            c->toggle = std::make_unique<juce::ToggleButton> ("On");
            c->buttonAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                duckProcessor.apvts, paramId, *c->toggle);
        }
        else
        {
            c->slider = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            c->slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, cellWidth - 6, 18);
            c->sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                duckProcessor.apvts, paramId, *c->slider);
        }

        addAndMakeVisible (c->widget());
        controls.push_back (std::move (c));
    }

    setSize (margin * 3 + cellWidth * (int) controls.size() + 12 + meterWidth, headerHeight + rowHeight + margin * 2);
    startTimerHz (30);
}

SubForgeDuckEditor::~SubForgeDuckEditor()
{
    setLookAndFeel (nullptr);
}

void SubForgeDuckEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    g.setColour (accent);
    g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    g.drawText ("SUBFORGE DUCK", margin, 10, 260, 30, juce::Justification::centredLeft);

    g.setColour (dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("ducks this track's low end (or all of it) when the side chain plays", margin + 220, 10, 460, 30,
                juce::Justification::centredLeft);

    auto area = getLocalBounds().reduced (margin);
    area.removeFromTop (headerHeight - margin);
    g.setColour (panel);
    g.fillRoundedRectangle (area.removeFromLeft (cellWidth * (int) controls.size() + 12).toFloat(), 8.0f);

    // Gain reduction meter
    area.removeFromLeft (margin);
    auto meter = area.removeFromLeft (meterWidth);
    g.setColour (dimText);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("Duck", meter.removeFromTop (18), juce::Justification::centred);
    auto readout = meter.removeFromBottom (18);
    auto bar = meter.reduced (28, 4);
    g.setColour (panel);
    g.fillRoundedRectangle (bar.toFloat(), 4.0f);

    const float norm = juce::jlimit (0.0f, 1.0f, shownReduction / 24.0f);
    g.setColour (duckBlue);
    g.fillRoundedRectangle (bar.toFloat().withHeight ((float) bar.getHeight() * norm), 4.0f);

    g.setColour (textColour);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (shownReduction > 0.05f ? "-" + juce::String (shownReduction, 1) + " dB" : "0.0 dB", readout,
                juce::Justification::centred);
}

void SubForgeDuckEditor::resized()
{
    auto area = getLocalBounds().reduced (margin);
    area.removeFromTop (headerHeight - margin);
    auto row = area.removeFromLeft (cellWidth * (int) controls.size() + 12).reduced (6, 8);

    for (auto& c : controls)
    {
        auto cell = row.removeFromLeft (cellWidth);
        c->label.setBounds (cell.removeFromTop (18));
        if (c->slider != nullptr)
            c->widget().setBounds (cell.reduced (2));
        else
            c->widget().setBounds (cell.withSizeKeepingCentre (cellWidth - 10, 26));
    }
}

void SubForgeDuckEditor::timerCallback()
{
    const float r = duckProcessor.meterReductionDb.exchange (0.0f);
    const float next = juce::jmax (r, shownReduction - 0.6f);
    if (std::abs (next - shownReduction) > 0.05f)
    {
        shownReduction = next;
        repaint();
    }

    const bool rollHold = duckProcessor.apvts.getRawParameterValue (subforge::duckparams::rollHold)->load() > 0.5f;
    const bool fullBand = duckProcessor.apvts.getRawParameterValue (subforge::duckparams::fullBand)->load() > 0.5f;
    const auto setDim = [] (Control& c, bool dim)
    {
        c.widget().setAlpha (dim ? 0.35f : 1.0f);
        c.label.setAlpha (dim ? 0.35f : 1.0f);
    };

    const bool lookAhead = duckProcessor.apvts.getRawParameterValue (subforge::duckparams::lookAhead)->load() > 0.5f;

    setDim (*controls[0], fullBand);  // Split does nothing in full-band mode
    setDim (*controls[6], ! rollHold);
    setDim (*controls[7], ! rollHold);
    setDim (*controls[9], ! lookAhead);
}
