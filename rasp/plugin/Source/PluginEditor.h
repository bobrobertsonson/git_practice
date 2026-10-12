#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

// The plugin window: Control / Thicken / Flavour / Bus+Live, like the pedal.
class RaspEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit RaspEditor(RaspProcessor&);
    ~RaspEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

    struct Panel {
        juce::String title;
        juce::Rectangle<int> bounds;
        std::unique_ptr<juce::ToggleButton> power;   // section on/off, in the title bar
        std::vector<std::unique_ptr<juce::Component>> cells;
        int cols = 3;
        bool newRow = false;        // start a second row of panels
    };

private:
    void timerCallback() override;
    void addKnob(Panel&, const char* id, const juce::String& label);
    void addChoice(Panel&, const char* id, const juce::String& label);
    void addToggle(Panel&, const char* id, const juce::String& label);
    void addPower(Panel&, const char* id);
    void addHold(Panel&, const char* id, const juce::String& label, const juce::String& tip);
    std::vector<juce::Component*> holds;   // HoldCells, lit by the timer when automation holds them

    RaspProcessor& proc;
    juce::LookAndFeel_V4 laf;
    std::vector<Panel> panels;
    juce::ComboBox preset;
    juce::TextButton bypass{"BYPASS"}, clearNotches{"Clear notches"};
    juce::TextButton megaBtn{"MEGA"}, throwBtn{"THROW"};   // momentary: on while held
    juce::TextButton stutterBtn{"STUTTER"}, glitchBtn{"GLITCH"};
    juce::TextButton learnBtn{"LEARN"};                    // listen to ~15 s of singing, set Match
    void holdButton(juce::TextButton&, const char* id);
    juce::Label status;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttach;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> powerAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RaspEditor)
};
