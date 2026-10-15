// AxeElevator window: an elevator panel. Floor buttons 1-13 pick the mode,
// the indicator shows the floor, the controls belong to the floor you're on,
// and the trigger strip (TRIGGER, Engage, Sync, Tempo, Rate, Swing) drives the
// fx floors.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class AxeLook : public juce::LookAndFeel_V4 {
public:
    AxeLook();
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
};

// One parameter as a control: a knob (float), a menu (choice) or a toggle (bool),
// with its name above.
class ParamControl : public juce::Component {
public:
    ParamControl(juce::AudioProcessorValueTreeState& s, const ParamSpec& spec);
    void resized() override;
private:
    juce::Label name;
    std::unique_ptr<juce::Slider> knob;
    std::unique_ptr<juce::ComboBox> menu;
    std::unique_ptr<juce::TextButton> toggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> knobAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> menuAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> toggleAtt;
};

// Hold to fire; shift-click (or right-click) latches.
class TriggerButton : public juce::TextButton {
public:
    explicit TriggerButton(juce::RangedAudioParameter& p) : juce::TextButton("TRIGGER"), param(p) {}
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
private:
    void set(bool on);
    juce::RangedAudioParameter& param;
    bool latched = false;
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
    void layoutSetup(juce::Rectangle<int> page);

    AxeProcessor& proc;
    AxeLook look;
    int shownMode = -1;

    juce::TextButton floors[axe::kNumModes];
    juce::TextButton guitarBtn{"GUITAR"}, bassBtn{"BASS"}, bypassBtn{"BYPASS"}, setupBtn{"SETUP"};
    // SETUP page: Learn My Playing and Tone Match
    juce::TextButton learnPlayingBtn{"LEARN MY PLAYING"}, learnTargetBtn{"LEARN TARGET"};
    ParamControl* targetMenu = nullptr;   // in setupControls
    std::vector<std::unique_ptr<ParamControl>> setupControls;
    bool setupShown = false;
    juce::Rectangle<int> statusArea;
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAtt;
    TriggerButton triggerBtn;

    std::vector<std::unique_ptr<ParamControl>> modeControls[axe::kNumModes];
    std::vector<std::unique_ptr<ParamControl>> globalControls, triggerControls;

    juce::Rectangle<int> indicatorArea, pageArea, meterArea, stepArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AxeEditor)
};
