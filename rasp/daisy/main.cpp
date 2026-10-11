// RASP firmware for Electrosmith Daisy Seed (STM32H750, 480 MHz Cortex-M7).
// Build inside a DaisyExamples-style project (libDaisy only).
//
// Panel - same layout as the plugin-style UI:
//   CONTROL  lo cut, hi cut, drive, mix, chop, release   + tone toggle (dark/natural/bright)
//   THICKEN  spread, diffuse, darken, width              + solo button
//   FLAVOUR  tail, blend, repeats, blend, tilt, duck     + time toggle (1/8, 1/8 dotted, 1/4)
//   Faders   control, thicken, flavour, output level
//   Footswitches: bypass (relay), thicken on, flavour on, tap tempo
//
// 20 pots: 16 through two CD4051 8:1 muxes, 4 faders direct.
// Pin map (Daisy Seed: D15..D20 are also A0..A5, D21..D25 are A6..A10, D28 is A11):
//   A0 (D15)  mux 1 out (control 6 + thicken spread/diffuse), select D0 D1 D2
//   A1 (D16)  mux 2 out (thicken darken/width + flavour 6),   select D3 D4 D5
//   A2..A5 (D17..D20)  level faders: control, thicken, flavour, output
//   D6/D7  tone toggle    D8/D9  delay-time toggle    D22 solo button
//   D10 FS bypass   D13 FS thicken   D14 FS flavour   D21 FS tap
//   D23 LED effect on   D24 LED chop (gain reduction > 3 dB)   D25 LED thicken
//   D26 LED flavour     D27 LED tap   D28 bypass relay driver (via transistor)
// Check every pin against the Daisy Seed pinout before you build a PCB.
#include "daisy_seed.h"
#include "../dsp/rasp.h"

using namespace daisy;

static DaisySeed hw;
// ~1.8 MB of delay lines: must live in the 64 MB SDRAM, not internal SRAM.
static rasp::Processor DSY_SDRAM_BSS proc;
static rasp::Params params;
static Switch fsBypass, fsThicken, fsFlavour, fsTap, btnSolo;
static GPIO toneA, toneB, timeA, timeB;
static GPIO ledOn, ledChop, ledThk, ledFlv, ledTap, relay;
static volatile bool bypass = false;
static volatile float grDb = 0.f;

enum AdcChan { MUX1, MUX2, FADER_CTL, FADER_THK, FADER_FLV, FADER_OUT, NUM_ADC };

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    for (size_t i = 0; i < size; i++) {
        float x = in[0][i];
        // Keep the DSP running in bypass so tails and envelopes stay warm;
        // the relay does the actual bypass in hardware.
        proc.process(x, out[0][i], out[1][i]);
        if (bypass) out[0][i] = out[1][i] = x;
    }
    grDb = proc.gainReductionDb();
}

static int readToggle(GPIO& a, GPIO& b) { return !a.Read() ? 0 : (!b.Read() ? 2 : 1); }
static float faderDb(float v, float lo, float hi) { return lo + (hi - lo) * v; }

int main() {
    hw.Init(true);                 // boost to 480 MHz
    // 4-sample blocks: buffering adds ~0.17 ms at 48 kHz. The codec's
    // converter delay sits on top of that - measure the round trip.
    hw.SetAudioBlockSize(4);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    AdcChannelConfig adc[NUM_ADC];
    adc[MUX1].InitMux(seed::A0, 8, seed::D0, seed::D1, seed::D2);
    adc[MUX2].InitMux(seed::A1, 8, seed::D3, seed::D4, seed::D5);
    adc[FADER_CTL].InitSingle(seed::A2);
    adc[FADER_THK].InitSingle(seed::A3);
    adc[FADER_FLV].InitSingle(seed::A4);
    adc[FADER_OUT].InitSingle(seed::A5);
    hw.adc.Init(adc, NUM_ADC);
    hw.adc.Start();

    fsBypass.Init(seed::D10, 1000.f);
    fsThicken.Init(seed::D13, 1000.f);
    fsFlavour.Init(seed::D14, 1000.f);
    fsTap.Init(seed::D21, 1000.f);
    btnSolo.Init(seed::D22, 1000.f);
    toneA.Init(seed::D6, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    toneB.Init(seed::D7, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    timeA.Init(seed::D8, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    timeB.Init(seed::D9, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    ledOn.Init(seed::D23, GPIO::Mode::OUTPUT);
    ledChop.Init(seed::D24, GPIO::Mode::OUTPUT);
    ledThk.Init(seed::D25, GPIO::Mode::OUTPUT);
    ledFlv.Init(seed::D26, GPIO::Mode::OUTPUT);
    ledTap.Init(seed::D27, GPIO::Mode::OUTPUT);
    relay.Init(seed::D28, GPIO::Mode::OUTPUT);

    proc.init(hw.AudioSampleRate());
    hw.StartAudio(AudioCallback);

    uint32_t lastTap = 0, tapLedOff = 0;
    for (;;) {
        // Control rate ~1 kHz. Gains and delay time are smoothed inside the DSP.
        fsBypass.Debounce(); fsThicken.Debounce(); fsFlavour.Debounce(); fsTap.Debounce(); btnSolo.Debounce();
        if (fsBypass.RisingEdge()) bypass = !bypass;
        if (fsThicken.RisingEdge()) params.thickenOn = !params.thickenOn;
        if (fsFlavour.RisingEdge()) params.flavourOn = !params.flavourOn;
        if (btnSolo.RisingEdge()) params.solo = !params.solo;
        uint32_t now = System::GetNow();
        if (fsTap.RisingEdge()) {
            uint32_t dt = now - lastTap;
            if (dt > 250 && dt < 1500) params.bpm = 60000.f / dt;
            lastTap = now; tapLedOff = now + 60;
        }

        params.loCut      = hw.adc.GetMuxFloat(MUX1, 0);
        params.hiCut      = hw.adc.GetMuxFloat(MUX1, 1);
        params.drive      = hw.adc.GetMuxFloat(MUX1, 2);
        params.mix        = hw.adc.GetMuxFloat(MUX1, 3);
        params.chop       = hw.adc.GetMuxFloat(MUX1, 4);
        params.release    = hw.adc.GetMuxFloat(MUX1, 5);
        params.spread     = hw.adc.GetMuxFloat(MUX1, 6);
        params.diffuse    = hw.adc.GetMuxFloat(MUX1, 7);
        params.darken     = hw.adc.GetMuxFloat(MUX2, 0);
        params.width      = hw.adc.GetMuxFloat(MUX2, 1);
        params.tail       = hw.adc.GetMuxFloat(MUX2, 2);
        params.tailBlend  = hw.adc.GetMuxFloat(MUX2, 3);
        params.repeats    = hw.adc.GetMuxFloat(MUX2, 4);
        params.delayBlend = hw.adc.GetMuxFloat(MUX2, 5);
        params.tilt       = hw.adc.GetMuxFloat(MUX2, 6);
        params.duck       = hw.adc.GetMuxFloat(MUX2, 7);
        params.controlGainDb = faderDb(hw.adc.GetFloat(FADER_CTL), -18.f, 12.f);
        params.thickenGainDb = faderDb(hw.adc.GetFloat(FADER_THK), -24.f, 6.f);
        params.flavourGainDb = faderDb(hw.adc.GetFloat(FADER_FLV), -24.f, 6.f);
        params.outDb         = faderDb(hw.adc.GetFloat(FADER_OUT), -24.f, 6.f);
        params.brightness = readToggle(toneA, toneB);
        params.delayTime  = readToggle(timeA, timeB);
        // NOTE: setParams runs outside the audio interrupt. A production build
        // should hand params over with a lock-free double buffer.
        proc.setParams(params);

        relay.Write(!bypass);          // energized = effect in circuit; unpowered = true bypass
        ledOn.Write(!bypass);
        ledChop.Write(!bypass && grDb > 3.f);
        ledThk.Write(params.thickenOn);
        ledFlv.Write(params.flavourOn);
        ledTap.Write(now < tapLedOff);
        System::Delay(1);
    }
}
