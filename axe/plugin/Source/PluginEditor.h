// AxeElevator window: an elevator panel. Floor buttons 1-4 pick the mode,
// the indicator shows the floor, the knobs belong to the floor you're on.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class AxeLook : public juce::LookAndFeel_V4 {
public:
    AxeLook();
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle, juce::Slider&) override;
};

// A knob with its name above and value below, attached to one parameter.
class Knob : public juce::Component {
public:
    Knob(juce::AudioProcessorValueTreeState& s, const juce::String& id, const juce::String& title);
    void resized() override;
    juce::Slider slider;
private:
    juce::Label name;
    juce::AudioProcessorValueTreeState::SliderAttachment att;
};

class AxeEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit AxeEditor(AxeProcessor&);
    ~AxeEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override;
    void showMode(int mode);
    int currentMode() const;
    void setChoice(const char* id, int index);

    AxeProcessor& proc;
    AxeLook look;
    int shownMode = -1;

    juce::TextButton floors[axe::kNumModes];
    juce::TextButton guitarBtn{"GUITAR"}, bassBtn{"BASS"}, bypassBtn{"BYPASS"};
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAtt;

    // knobs of every page; only the current mode's page is visible
    std::vector<std::unique_ptr<Knob>> modeKnobs[axe::kNumModes];
    std::vector<std::unique_ptr<Knob>> globalKnobs;
    // Sub type, Learn
    juce::TextButton polyBtn{"POLY"}, monoBtn{"MONO"};
    juce::TextButton learnFreshBtn{"LEARN FRESH"}, learnNowBtn{"LEARN NOW"};
    juce::Label learnStatus;
    std::vector<std::unique_ptr<juce::Slider>> matchSliders;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> matchAtts;

    juce::Rectangle<int> indicatorArea, pageArea, meterArea, matchArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AxeEditor)
};
