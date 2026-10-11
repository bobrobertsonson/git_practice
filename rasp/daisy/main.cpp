// RASP firmware for Electrosmith Daisy Seed (STM32H750, 480 MHz Cortex-M7).
// Build inside a DaisyExamples-style project (libDaisy only).
//
// Panel - same layout as the plugin-style UI:
//   CONTROL  lo cut, hi cut, drive, mix, chop, release   + tone toggle (dark/natural/bright)
//   THICKEN  spread, diffuse, darken, width              + solo button
//   FLAVOUR  tail, blend, repeats, blend, tilt, duck     + time toggle (1/8, 1/8 dotted, 1/4)
//   Faders   control, thicken, flavour, output level
//   Trims    bleed reducer, de-esser, glue (set-and-forget, side panel)
//   Footswitches: bypass (relay), thicken on, flavour on, tap tempo
//
// Pin map (Daisy Seed: D15..D23 are also A0..A8, D25 is A10, D28 is A11):
//   A0 (D15)  mux 1 out (control 6 + spread, diffuse), CD4051 select D0 D1 D2
//   A1 (D16)  mux 2 out (darken, width + flavour 6),   CD4051 select D3 D4 D5
//   A2..A5 (D17..D20)  faders: control, thicken, flavour, output
//   A6..A8 (D21..D23)  trims: bleed, de-ess, glue
//   D6/D7  tone toggle    D8/D9  delay-time toggle    D14 solo button
//   D10 FS bypass   D11 FS thicken   D12 FS flavour   D13 FS tap
//   D24 / D25 / D26  74HC595 data / clock / latch -> LEDs:
//       Q0 effect on, Q1 chop (>3 dB GR), Q2 bleed reducer active, Q3 thicken,
//       Q4 flavour, Q5 tap, Q6 bus limiter
//   D27 bypass relay driver (via transistor)
// Check every pin against the Daisy Seed pinout before you build a PCB.
#include "daisy_seed.h"
#include "../dsp/rasp.h"

using namespace daisy;

static DaisySeed hw;
// ~1.4 MB of delay lines: must live in the 64 MB SDRAM, not internal SRAM.
static rasp::Processor DSY_SDRAM_BSS proc;
static rasp::Params params;
static Switch fsBypass, fsThicken, fsFlavour, fsTap, btnSolo;
static GPIO toneA, toneB, timeA, timeB, srData, srClock, srLatch, relay;
static volatile bool bypass = false;
static volatile float grDb = 0.f, bleedDb = 0.f, limDb = 0.f;

enum AdcChan { MUX1, MUX2, FADER_CTL, FADER_THK, FADER_FLV, FADER_OUT, TRIM_BLEED, TRIM_DEESS, TRIM_GLUE, NUM_ADC };

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    for (size_t i = 0; i < size; i++) {
        float x = in[0][i];
        // Keep the DSP running in bypass so tails and envelopes stay warm;
        // the relay does the actual bypass in hardware.
        proc.process(x, out[0][i], out[1][i]);
        if (bypass) out[0][i] = out[1][i] = x;
    }
    grDb = proc.gainReductionDb();
    bleedDb = proc.bleedReductionDb();
    limDb = proc.limiterDb();
}

static int readToggle(GPIO& a, GPIO& b) { return !a.Read() ? 0 : (!b.Read() ? 2 : 1); }
static float faderDb(float v, float lo, float hi) { return lo + (hi - lo) * v; }

static void writeLeds(uint8_t bits) {
    srLatch.Write(false);
    for (int i = 7; i >= 0; i--) {
        srData.Write((bits >> i) & 1);
        srClock.Write(true);
        srClock.Write(false);
    }
    srLatch.Write(true);
}

int main() {
    hw.Init(true);                 // boost to 480 MHz
    // Flush denormals to zero (FPSCR.FZ). Reverb and filter tails decay into
    // denormals between phrases; without this they cost far more CPU.
    __set_FPSCR(__get_FPSCR() | (1u << 24));
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
    adc[TRIM_BLEED].InitSingle(seed::A6);
    adc[TRIM_DEESS].InitSingle(seed::A7);
    adc[TRIM_GLUE].InitSingle(seed::A8);
    hw.adc.Init(adc, NUM_ADC);
    hw.adc.Start();

    fsBypass.Init(seed::D10, 1000.f);
    fsThicken.Init(seed::D11, 1000.f);
    fsFlavour.Init(seed::D12, 1000.f);
    fsTap.Init(seed::D13, 1000.f);
    btnSolo.Init(seed::D14, 1000.f);
    toneA.Init(seed::D6, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    toneB.Init(seed::D7, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    timeA.Init(seed::D8, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    timeB.Init(seed::D9, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    srData.Init(seed::D24, GPIO::Mode::OUTPUT);
    srClock.Init(seed::D25, GPIO::Mode::OUTPUT);
    srLatch.Init(seed::D26, GPIO::Mode::OUTPUT);
    relay.Init(seed::D27, GPIO::Mode::OUTPUT);

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
        params.outDb         = faderDb(hw.adc.GetFloat(FADER_OUT), -24.f, 12.f);
        params.bleed         = hw.adc.GetFloat(TRIM_BLEED);
        params.deEss         = hw.adc.GetFloat(TRIM_DEESS);
        params.glue          = hw.adc.GetFloat(TRIM_GLUE);
        params.brightness = readToggle(toneA, toneB);
        params.delayTime  = readToggle(timeA, timeB);
        // NOTE: setParams runs outside the audio interrupt. A production build
        // should hand params over with a lock-free double buffer.
        proc.setParams(params);

        relay.Write(!bypass);          // energized = effect in circuit; unpowered = true bypass
        uint8_t leds = 0;
        leds |= (!bypass) << 0;
        leds |= (!bypass && grDb > 3.f) << 1;
        leds |= (!bypass && bleedDb > 6.f) << 2;   // set the bleed trim so this is lit when you're silent
        leds |= params.thickenOn << 3;
        leds |= params.flavourOn << 4;
        leds |= (now < tapLedOff) << 5;
        leds |= (!bypass && limDb > 1.f) << 6;
        writeLeds(leds);
        System::Delay(1);
    }
}
