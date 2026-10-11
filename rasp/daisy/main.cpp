// RASP firmware for Electrosmith Daisy Seed (STM32H750, 480 MHz Cortex-M7).
// Build inside a DaisyExamples-style project (libDaisy + DaisySP not required).
// Wiring (suggested):
//   ADC knobs  A0..A5 -> Drive, Fry, Throat, Vowel, Air, Mix
//   D15 footswitch (bypass), D16 3-way toggle via 2 pins D17/D18 (mode)
//   D22 LED (on/clip)
#include "daisy_seed.h"
#include "../dsp/rasp.h"

using namespace daisy;

static DaisySeed hw;
static rasp::Processor proc;
static rasp::Params params;
static Switch footswitch;
static GPIO modeA, modeB, led;
static bool bypass = false;

enum Knob { DRIVE, FRY, THROAT, VOWEL, AIR, MIX, NUM_KNOBS };

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    for (size_t i = 0; i < size; i++) {
        float x = in[0][i];
        float y = bypass ? x : proc.process(x);
        out[0][i] = y;
        out[1][i] = y;
    }
}

int main() {
    hw.Init(true);                 // boost to 480 MHz
    hw.SetAudioBlockSize(16);      // ~0.33 ms per block @ 48 kHz -> low latency
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    AdcChannelConfig adc[NUM_KNOBS];
    for (int k = 0; k < NUM_KNOBS; k++) adc[k].InitSingle(seed::A0 + k);
    hw.adc.Init(adc, NUM_KNOBS);
    hw.adc.Start();

    footswitch.Init(seed::D15, 1000.f);
    modeA.Init(seed::D17, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    modeB.Init(seed::D18, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    led.Init(seed::D22, GPIO::Mode::OUTPUT);

    proc.init(hw.AudioSampleRate());
    hw.StartAudio(AudioCallback);

    for (;;) {
        // Control rate (~1 kHz) - cheap enough to recompute filter coefficients.
        footswitch.Debounce();
        if (footswitch.RisingEdge()) bypass = !bypass;

        params.drive  = hw.adc.GetFloat(DRIVE);
        params.fry    = hw.adc.GetFloat(FRY);
        params.throat = hw.adc.GetFloat(THROAT);
        params.vowel  = hw.adc.GetFloat(VOWEL);
        params.air    = hw.adc.GetFloat(AIR);
        params.mix    = hw.adc.GetFloat(MIX);
        params.mode   = !modeA.Read() ? 1 : (!modeB.Read() ? 2 : 0);
        proc.setParams(params);  // NOTE: for production, double-buffer or smooth params

        led.Write(!bypass);
        System::Delay(1);
    }
}
