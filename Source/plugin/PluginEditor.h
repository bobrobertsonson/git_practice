#pragma once

#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>
#include <vector>

class SubForgeEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit SubForgeEditor (SubForgeProcessor&);
    ~SubForgeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // One labelled control bound to a parameter: a knob, a menu or a switch.
    struct Control
    {
        juce::Label label;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::ComboBox> combo;
        std::unique_ptr<juce::ToggleButton> toggle;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;

        juce::Component& widget();
    };

    struct Section
    {
        juce::String title;
        std::vector<Control*> controls;
        juce::Rectangle<int> bounds;
    };

    // Vertical bar meter: either level (fills up) or gain reduction (fills down).
    class Meter : public juce::Component
    {
    public:
        Meter (juce::String name, float minDb, float maxDb, bool reduction);
        void setValue (float db);
        void setMarker (float db) { marker = db; repaint(); }
        void paint (juce::Graphics&) override;

    private:
        juce::String name;
        float minDb, maxDb, value, marker;
        bool reduction;
    };

    Control& addControl (Section&, const char* paramId);
    void timerCallback() override;

    SubForgeProcessor& processor;
    juce::LookAndFeel_V4 lookAndFeel;

    std::vector<std::unique_ptr<Control>> controls;
    Section sound, drop, lowEnd, mix;

    Control* punchControl = nullptr;
    Control* punchTimeControl = nullptr;
    Control* releaseControl = nullptr;
    Control* rollWindowControl = nullptr;
    Control* rollDepthControl = nullptr;

    Meter outputMeter { "Output", -48.0f, 0.0f, false };
    Meter subMeter { "Sub Clip", 0.0f, 18.0f, true };
    Meter duckMeter { "Duck", 0.0f, 24.0f, true };

    float shownOutput = -180.0f, shownSub = 0.0f, shownDuck = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubForgeEditor)
};
