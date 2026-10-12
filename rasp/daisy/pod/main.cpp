// RASP bench firmware for the Electrosmith Daisy Pod (Daisy Seed + line in/out,
// headphone amp, 2 knobs, 2 buttons, encoder, 2 RGB LEDs). Plug a mic into an
// audio interface, the interface's line out into the Pod's IN, and listen on
// the Pod's headphone out: same DSP as the full pedal, ~1 ms through the box.
// Builds against libDaisy v9.0.0 (see Makefile); not yet run on hardware.
//
//   Encoder turn   preset: huge, natural, dark, bright, live, raw, air (LED 1 colour)
//   Encoder press  bypass (level-matched; LED 1 off while bypassed); hold 2 s = clear feedback notches
//   Knob 1         drive          Knob 2   chop
//   Button 1       tap: input gain step 0 / +6 / +12 / +18 dB (LED 2 flashes the step)
//                  hold: MEGA while held (hard-clipped mid burst)
//   Button 2       tap: thicken + flavour on/off
//                  hold: THROW while held (long delay; repeats ring out after release)
//   Both buttons   hold 2 s: LEARN (sing ~15 s; LED 1 blinks while it listens). Applies to every preset
//   TRS MIDI in    CC 80 = MEGA, CC 81 = THROW (value >= 64 on), CC 82 / 83 = pitch 1 / 2 level, CC 84 / 85 = pitch 1 / 2 interval (-12..+12 st)
//   LED 2          red = chop compressing > 3 dB, blue = feedback notch active,
//                  white = a momentary effect is held
#include "daisy_pod.h"
#include "../../dsp/rasp.h"
#include "../../dsp/presets.h"

using namespace daisy;

static DaisyPod hw;
static rasp::Processor DSY_SDRAM_BSS proc;
static rasp::Params params;
static int preset = 0, gainStep = 2;
static volatile float grDb = 0.f;

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    for (size_t i = 0; i < size; i++) proc.process(in[0][i], out[0][i], out[1][i]);
    grDb = proc.gainReductionDb();
}

static void loadPreset() {
    bool bypass = params.bypass, thk = params.thickenOn, flv = params.flavourOn;
    bool mega = params.mega, thr = params.throwOn;
    float od = params.octDown, ou = params.octUp;           // set over MIDI: a performance setting
    int s1 = params.pitch1Semi, s2 = params.pitch2Semi;
    float match[rasp::kMatchBands], amt = params.matchAmount;          // Learn result: the singer's, not the preset's
    for (int b = 0; b < rasp::kMatchBands; b++) match[b] = params.match[b];
    params = rasp::presetParams(preset);
    params.bypass = bypass; params.mega = mega; params.throwOn = thr;
    params.octDown = od; params.octUp = ou; params.pitch1Semi = s1; params.pitch2Semi = s2;
    for (int b = 0; b < rasp::kMatchBands; b++) params.match[b] = match[b];
    params.matchAmount = amt;
    if (!thk) params.thickenOn = false;
    if (!flv) params.flavourOn = false;
}

int main() {
    hw.Init(true);
    __set_FPSCR(__get_FPSCR() | (1u << 24));     // flush denormals
    hw.SetAudioBlockSize(4);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    proc.init(hw.AudioSampleRate());
    loadPreset();
    proc.setParams(params);
    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    hw.midi.StartReceive();

    static const float presetColour[rasp::kNumPresets][3] = {
        {1, 0.3f, 0}, {0, 1, 0}, {0.6f, 0, 1}, {0, 0.8f, 1}, {1, 1, 0}, {1, 1, 1}, {1, 0, 0.5f}};
    uint32_t flashUntil = 0;
    constexpr float kHoldMs = 250.f;   // longer than a tap: the button acts as a momentary
    bool midiMega = false, midiThrow = false, cleared = false, learnArmed = false;
    float encHeld = 0.f, b1Held = 0.f, b2Held = 0.f;
    for (;;) {
        hw.ProcessAllControls();
        uint32_t now = System::GetNow();
        int inc = hw.encoder.Increment();
        if (inc) { preset = (preset + inc + rasp::kNumPresets) % rasp::kNumPresets; loadPreset(); }
        // Taps act on release, so a hold never also triggers the tap action.
        // libDaisy's TimeHeldMs() reads 0 once released, so keep the last value.
        if (hw.encoder.Pressed()) encHeld = hw.encoder.TimeHeldMs();
        if (hw.button1.Pressed()) b1Held = hw.button1.TimeHeldMs();
        if (hw.button2.Pressed()) b2Held = hw.button2.TimeHeldMs();
        if (hw.encoder.FallingEdge() && encHeld < 2000) params.bypass = !params.bypass;
        if (hw.encoder.Pressed() && encHeld > 2000 && !cleared) { proc.clearHowl(); cleared = true; }
        if (!hw.encoder.Pressed()) cleared = false;
        if (hw.button1.FallingEdge() && b1Held < kHoldMs) {
            gainStep = (gainStep + 1) % 4; flashUntil = now + 150 * (gainStep + 1);
        }
        if (hw.button2.FallingEdge() && b2Held < kHoldMs) {
            params.thickenOn = params.flavourOn = !params.thickenOn;
        }
        hw.midi.Listen();
        while (hw.midi.HasEvents()) {
            MidiEvent e = hw.midi.PopEvent();
            if (e.type == ControlChange) {
                auto cc = e.AsControlChange();
                if (cc.control_number == 80) midiMega = cc.value >= 64;
                if (cc.control_number == 81) midiThrow = cc.value >= 64;
                if (cc.control_number == 82) params.octDown = cc.value / 127.f;
                if (cc.control_number == 83) params.octUp = cc.value / 127.f;
                if (cc.control_number == 84) params.pitch1Semi = (int)std::lround(cc.value * 24.f / 127.f) - 12;
                if (cc.control_number == 85) params.pitch2Semi = (int)std::lround(cc.value * 24.f / 127.f) - 12;
            }
        }
        // Both buttons held 2 s: Learn (not Mega/Throw).
        const bool both = hw.button1.Pressed() && hw.button2.Pressed();
        if (both && b1Held > 2000 && b2Held > 2000 && !learnArmed) { proc.startLearn(); learnArmed = true; }
        if (!hw.button1.Pressed() && !hw.button2.Pressed()) learnArmed = false;
        params.mega    = !both && ((hw.button1.Pressed() && b1Held >= kHoldMs) || midiMega);
        params.throwOn = !both && ((hw.button2.Pressed() && b2Held >= kHoldMs) || midiThrow);
        {
            float g[rasp::kMatchBands];
            if (proc.learnedMatch(g)) {
                for (int b = 0; b < rasp::kMatchBands; b++) params.match[b] = g[b];
                params.matchAmount = 1.f;
            }
        }

        params.drive = hw.knob1.Process();
        params.chop = hw.knob2.Process();
        params.inDb = 6.f * gainStep;
        proc.setParams(params);

        const float* c = presetColour[preset];
        float on = (params.bypass || (proc.learning() && (now / 120) % 2)) ? 0.f : 1.f;
        hw.led1.Set(c[0] * on, c[1] * on, c[2] * on);
        bool flash = now < flashUntil && ((flashUntil - now) / 150) % 2 == 0;
        bool held = params.mega || params.throwOn;
        hw.led2.Set(flash || held ? 1.f : (grDb > 3.f ? 1.f : 0.f), flash || held ? 1.f : 0.f,
                    flash || held ? 1.f : (proc.howlNotches() > 0 ? 1.f : 0.f));
        hw.UpdateLeds();
        System::Delay(1);
    }
}
