// RASP firmware for Electrosmith Daisy Seed (STM32H750, 480 MHz Cortex-M7).
// Build inside a DaisyExamples-style project (libDaisy only; DaisySP not needed).
//
// Panel (12 pots, 2 three-way toggles, 3 footswitches, 2 LEDs):
//   CONTROL  A0 lo cut   A1 hi cut   A2 drive   A3 mix   A4 chop   A5 release
//   THICKEN  A6 spread   A7 darken
//   FLAVOUR  A8 tail blend   A9 delay blend   A10 repeats   A11 duck
//   Toggle 1 (D7/D8): drive type   Fry / Growl / Squeal
//   Toggle 2 (D9/D10): delay time  slap / 1/8 / 1/4
//   FS1 D11: bypass   FS2 D12: flavour on/off   FS3 D13: tap tempo
//   LED D14: effect on   LED D15: gate open (set CHOP so it only lights when you sing)
// Everything else (tail, tilt, width, diffuse, fry/throat/vowel/air, section gains)
// lives on an encoder + OLED menu - left out of this sketch for clarity.
#include "daisy_seed.h"
#include "../dsp/rasp.h"

using namespace daisy;

static DaisySeed hw;
// ~1.6 MB of delay lines: must live in the 64 MB SDRAM, not internal SRAM.
static rasp::Processor DSY_SDRAM_BSS proc;
static rasp::Params params;
static Switch fsBypass, fsFlavour, fsTap;
static GPIO typeA, typeB, timeA, timeB, ledOn, ledGate;
static volatile bool bypass = false;

enum Pot { LOCUT, HICUT, DRIVE, MIX, CHOP, RELEASE, SPREAD, DARKEN,
           TAILBLEND, DELAYBLEND, REPEATS, DUCK, NUM_POTS };

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    for (size_t i = 0; i < size; i++) {
        float x = in[0][i];
        if (bypass) { out[0][i] = out[1][i] = x; continue; }  // true-bypass relay is better; see README
        proc.process(x, out[0][i], out[1][i]);
    }
}

static int readToggle(GPIO& a, GPIO& b) { return !a.Read() ? 0 : (!b.Read() ? 2 : 1); }

int main() {
    hw.Init(true);                 // boost to 480 MHz
    // 4-sample blocks: buffering adds only ~0.17 ms at 48 kHz.
    // The codec's own converter delay sits on top of that (measure it, see README).
    hw.SetAudioBlockSize(4);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    AdcChannelConfig adc[NUM_POTS];
    const Pin pins[NUM_POTS] = {seed::A0, seed::A1, seed::A2, seed::A3, seed::A4, seed::A5,
                                seed::A6, seed::A7, seed::A8, seed::A9, seed::A10, seed::A11};
    for (int k = 0; k < NUM_POTS; k++) adc[k].InitSingle(pins[k]);
    hw.adc.Init(adc, NUM_POTS);
    hw.adc.Start();

    fsBypass.Init(seed::D11, 1000.f);
    fsFlavour.Init(seed::D12, 1000.f);
    fsTap.Init(seed::D13, 1000.f);
    typeA.Init(seed::D7, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    typeB.Init(seed::D8, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    timeA.Init(seed::D9, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    timeB.Init(seed::D10, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    ledOn.Init(seed::D14, GPIO::Mode::OUTPUT);
    ledGate.Init(seed::D15, GPIO::Mode::OUTPUT);

    proc.init(hw.AudioSampleRate());
    hw.StartAudio(AudioCallback);

    uint32_t lastTap = 0;
    for (;;) {
        // Control rate ~1 kHz. Gains are smoothed inside the DSP; filter
        // coefficients use TPT SVFs, which tolerate being updated live.
        fsBypass.Debounce(); fsFlavour.Debounce(); fsTap.Debounce();
        if (fsBypass.RisingEdge()) bypass = !bypass;
        if (fsFlavour.RisingEdge()) params.flavourOn = !params.flavourOn;
        if (fsTap.RisingEdge()) {
            uint32_t now = System::GetNow(), dt = now - lastTap;
            if (dt > 250 && dt < 1500) params.bpm = 60000.f / dt;
            lastTap = now;
        }

        params.loCut      = hw.adc.GetFloat(LOCUT);
        params.hiCut      = hw.adc.GetFloat(HICUT);
        params.drive      = hw.adc.GetFloat(DRIVE);
        params.mix        = hw.adc.GetFloat(MIX);
        params.chop       = hw.adc.GetFloat(CHOP);
        params.release    = hw.adc.GetFloat(RELEASE);
        params.spread     = hw.adc.GetFloat(SPREAD);
        params.darken     = hw.adc.GetFloat(DARKEN);
        params.tailBlend  = hw.adc.GetFloat(TAILBLEND);
        params.delayBlend = hw.adc.GetFloat(DELAYBLEND);
        params.repeats    = hw.adc.GetFloat(REPEATS);
        params.duck       = hw.adc.GetFloat(DUCK);
        params.driveType  = readToggle(typeA, typeB);
        params.delayTime  = readToggle(timeA, timeB);
        // NOTE: setParams runs outside the audio interrupt. A production build
        // should hand params over with a lock-free double buffer.
        proc.setParams(params);

        ledOn.Write(!bypass);
        ledGate.Write(!bypass && proc.gateOpen());
        System::Delay(1);
    }
}
