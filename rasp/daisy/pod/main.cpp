// RASP bench firmware for the Electrosmith Daisy Pod (Daisy Seed + line in/out,
// headphone amp, 2 knobs, 2 buttons, encoder, 2 RGB LEDs). Plug a mic into an
// audio interface, the interface's line out into the Pod's IN, and listen on
// the Pod's headphone out: same DSP as the full pedal, ~1 ms through the box.
// Builds against libDaisy v9.0.0 (see Makefile); not yet run on hardware.
//
//   Encoder turn   preset: huge, natural, dark, bright, live, raw (LED 1 colour)
//   Encoder press  bypass (level-matched; LED 1 off while bypassed)
//   Knob 1         drive          Knob 2   chop
//   Button 1       input gain step: 0 / +6 / +12 / +18 dB (LED 2 flashes the step)
//   Button 2       thicken + flavour on/off; hold 2 s = clear feedback notches
//   LED 2          red = chop compressing > 3 dB, blue = feedback notch active
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
    params = rasp::presetParams(preset);
    params.bypass = bypass;
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

    static const float presetColour[rasp::kNumPresets][3] = {
        {1, 0.3f, 0}, {0, 1, 0}, {0.6f, 0, 1}, {0, 0.8f, 1}, {1, 1, 0}, {1, 1, 1}};
    uint32_t flashUntil = 0;
    for (;;) {
        hw.ProcessAllControls();
        uint32_t now = System::GetNow();
        int inc = hw.encoder.Increment();
        if (inc) { preset = (preset + inc + rasp::kNumPresets) % rasp::kNumPresets; loadPreset(); }
        if (hw.encoder.RisingEdge()) params.bypass = !params.bypass;
        if (hw.button1.RisingEdge()) { gainStep = (gainStep + 1) % 4; flashUntil = now + 150 * (gainStep + 1); }
        if (hw.button2.FallingEdge() && hw.button2.TimeHeldMs() < 2000) {
            params.thickenOn = params.flavourOn = !params.thickenOn;
        }
        if (hw.button2.Pressed() && hw.button2.TimeHeldMs() > 2000) proc.clearHowl();

        params.drive = hw.knob1.Process();
        params.chop = hw.knob2.Process();
        params.inDb = 6.f * gainStep;
        proc.setParams(params);

        const float* c = presetColour[preset];
        float on = params.bypass ? 0.f : 1.f;
        hw.led1.Set(c[0] * on, c[1] * on, c[2] * on);
        bool flash = now < flashUntil && ((flashUntil - now) / 150) % 2 == 0;
        hw.led2.Set(flash ? 1.f : (grDb > 3.f ? 1.f : 0.f), flash ? 1.f : 0.f,
                    flash ? 1.f : (proc.howlNotches() > 0 ? 1.f : 0.f));
        hw.UpdateLeds();
        System::Delay(1);
    }
}
