#pragma once

#include "DuckProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>
#include <vector>

class SubForgeDuckEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit SubForgeDuckEditor (SubForgeDuckProcessor&);
    ~SubForgeDuckEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Control
    {
        juce::Label label;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::ToggleButton> toggle;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;

        juce::Component& widget() { return slider != nullptr ? static_cast<juce::Component&> (*slider) : *toggle; }
    };

    void timerCallback() override;

    SubForgeDuckProcessor& duckProcessor;
    juce::LookAndFeel_V4 lookAndFeel;
    std::vector<std::unique_ptr<Control>> controls;
    float shownReduction = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubForgeDuckEditor)
};
