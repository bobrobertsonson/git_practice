// RASP firmware for Electrosmith Daisy Seed (STM32H750, 480 MHz Cortex-M7).
// Build inside a DaisyExamples-style project (libDaisy only).
// Builds against libDaisy v9.0.0 (see Makefile); not yet run on hardware.
//
// Panel - same layout as the plugin-style UI:
//   CONTROL  lo cut, hi cut, drive, mix, chop, release   + tone toggle (dark/natural/bright)
//   THICKEN  spread, diffuse, darken, width              + solo button
//   FLAVOUR  tail, blend, repeats, blend, tilt, duck     + time toggle (1/8, 1/8 dotted, 1/4)
//   Faders   control, thicken, flavour, output level
//   Trims    bleed reducer, de-esser, glue (set-and-forget, side panel)
//   Switch   mono (one-channel PA)
//
// Footswitches (short press / long press = held 0.6 s):
//   BYPASS   level-matched digital bypass            /  -
//   THICKEN  thicken on/off                           /  previous preset
//   FLAVOUR  flavour on/off                           /  next preset
//   TAP      tap tempo                                /  (held 2 s) clear feedback notches
// Solo button: short = solo thicken, held 2 s = save the current sound to this preset slot.
// USB MIDI: Program Change 0-7 recalls preset slots 1-8 (from a laptop/backing-track rig).
//
// Presets: 8 slots in QSPI flash (4 MB in), first boot fills them with the factory set
// (huge, natural, dark, bright, live, raw, natural, natural). After a recall,
// each knob keeps the preset's value until you move it ("pickup").
//
// Bypass: digital and level-matched (your voice doesn't drop ~18 dB when you
// talk between songs). The relay is energized while the pedal runs; without
// power it falls back to a hard-wired dry path from XLR in to XLR out.
//
// Pin map (Daisy Seed: D15..D23 are also A0..A8, D25 is A10, D28 is A11):
//   A0 (D15)  mux 1 out (control 6 + spread, diffuse), CD4051 select D0 D1 D2
//   A1 (D16)  mux 2 out (darken, width + flavour 6),   CD4051 select D3 D4 D5
//   A2..A5 (D17..D20)  faders: control, thicken, flavour, output
//   A6..A8 (D21..D23)  trims: bleed, de-ess, glue
//   D6/D7  tone toggle    D8/D9  delay-time toggle    D14 solo button   D28 mono switch
//   D10 FS bypass   D11 FS thicken   D12 FS flavour   D13 FS tap
//   D24 / D25 / D26  74HC595 data / clock / latch -> LEDs:
//       Q0 effect on, Q1 chop (>3 dB GR), Q2 bleed reducer active, Q3 thicken,
//       Q4 flavour, Q5 tap / preset blink, Q6 bus limiter, Q7 feedback notch
//   D27 bypass relay driver (via transistor)
//   USB (micro-USB on the Seed): MIDI device + power while bench-testing
// Check every pin against the Daisy Seed pinout before you build a PCB.
#include "daisy_seed.h"
#include "../dsp/rasp.h"
#include "../dsp/presets.h"
#include <cstring>

using namespace daisy;

static DaisySeed hw;
// ~1.5 MB of delay lines: must live in the 64 MB SDRAM, not internal SRAM.
static rasp::Processor DSY_SDRAM_BSS proc;
static rasp::Params params;
static Switch fsBypass, fsThicken, fsFlavour, fsTap, btnSolo;
static GPIO toneA, toneB, timeA, timeB, monoSw, srData, srClock, srLatch, relay;
static MidiUsbHandler midi;
static volatile float grDb = 0.f, bleedDb = 0.f, limDb = 0.f;
static volatile int howlEvents = 0;

// ---------------------------------------------------------------- presets in flash
constexpr int kSlots = 8;
struct Stored {
    rasp::Params slot[kSlots];
    int current;
    bool operator==(const Stored& o) const { return std::memcmp(this, &o, sizeof(Stored)) == 0; }
    bool operator!=(const Stored& o) const { return !(*this == o); }
};
static PersistentStorage<Stored> storage(hw.qspi);

// ---------------------------------------------------------------- analog controls
enum AdcChan { MUX1, MUX2, FADER_CTL, FADER_THK, FADER_FLV, FADER_OUT, TRIM_BLEED, TRIM_DEESS, TRIM_GLUE, NUM_ADC };
constexpr int kPots = 23;   // 16 mux knobs + 4 faders + 3 trims

// Knob "pickup": after a preset recall a control keeps the preset's value
// until it is moved by more than ~3% from where it sat at recall time.
struct Pickup {
    float at = 0.f;
    bool locked = false;
    bool moved(float v) {
        if (locked && std::fabs(v - at) > 0.03f) locked = false;
        return !locked;
    }
};
static Pickup pickup[kPots];

static float readPot(int i) {
    if (i < 8)  return hw.adc.GetMuxFloat(MUX1, i);
    if (i < 16) return hw.adc.GetMuxFloat(MUX2, i - 8);
    return hw.adc.GetFloat(FADER_CTL + (i - 16));
}

static float faderDb(float v, float lo, float hi) { return lo + (hi - lo) * v; }

// Writes pot i into params, unless it's still parked after a preset recall.
static void applyPot(int i) {
    float v = readPot(i);
    if (!pickup[i].moved(v)) return;
    switch (i) {
        case 0:  params.loCut = v; break;       case 1:  params.hiCut = v; break;
        case 2:  params.drive = v; break;       case 3:  params.mix = v; break;
        case 4:  params.chop = v; break;        case 5:  params.release = v; break;
        case 6:  params.spread = v; break;      case 7:  params.diffuse = v; break;
        case 8:  params.darken = v; break;      case 9:  params.width = v; break;
        case 10: params.tail = v; break;        case 11: params.tailBlend = v; break;
        case 12: params.repeats = v; break;     case 13: params.delayBlend = v; break;
        case 14: params.tilt = v; break;        case 15: params.duck = v; break;
        case 16: params.controlGainDb = faderDb(v, -18.f, 12.f); break;
        case 17: params.thickenGainDb = faderDb(v, -24.f, 6.f); break;
        case 18: params.flavourGainDb = faderDb(v, -24.f, 6.f); break;
        case 19: params.outDb = faderDb(v, -24.f, 12.f); break;
        case 20: params.bleed = v; break;       case 21: params.deEss = v; break;
        case 22: params.glue = v; break;
    }
}

static void recall(int slot) {
    Stored& s = storage.GetSettings();
    slot = (slot % kSlots + kSlots) % kSlots;
    bool bypass = params.bypass, mono = params.mono;    // live switches aren't part of a sound
    float bpm = params.bpm;
    params = s.slot[slot];
    params.bypass = bypass; params.mono = mono; params.bpm = bpm;
    s.current = slot;
    for (int i = 0; i < kPots; i++) { pickup[i].at = readPot(i); pickup[i].locked = true; }
}

static void saveCurrent() {
    Stored& s = storage.GetSettings();
    s.slot[s.current] = params;
    storage.Save();
}

// ---------------------------------------------------------------- audio
static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    for (size_t i = 0; i < size; i++) proc.process(in[0][i], out[0][i], out[1][i]);
    grDb = proc.gainReductionDb();
    bleedDb = proc.bleedReductionDb();
    limDb = proc.limiterDb();
    howlEvents = proc.howlEvents();
}

static int readToggle(GPIO& a, GPIO& b) { return !a.Read() ? 0 : (!b.Read() ? 2 : 1); }

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
    monoSw.Init(seed::D28, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    srData.Init(seed::D24, GPIO::Mode::OUTPUT);
    srClock.Init(seed::D25, GPIO::Mode::OUTPUT);
    srLatch.Init(seed::D26, GPIO::Mode::OUTPUT);
    relay.Init(seed::D27, GPIO::Mode::OUTPUT);
    relay.Write(true);             // effect path in circuit while powered

    // Presets: first boot fills flash with the factory set.
    Stored defaults;
    for (int i = 0; i < kSlots; i++) defaults.slot[i] = rasp::presetParams(i < rasp::kNumPresets ? i : 1);
    defaults.current = 1;          // natural
    // Presets live 4 MB into the QSPI flash, well clear of the program the
    // bootloader stores at its start.
    storage.Init(defaults, 0x400000);
    params = storage.GetSettings().slot[storage.GetSettings().current];

    MidiUsbHandler::Config midiCfg;
    midiCfg.transport_config.periph = MidiUsbTransport::Config::INTERNAL;
    midi.Init(midiCfg);
    midi.StartReceive();

    proc.init(hw.AudioSampleRate());
    proc.setParams(params);
    recall(storage.GetSettings().current);
    hw.StartAudio(AudioCallback);

    uint32_t lastTap = 0, tapLedOff = 0, notchLedOff = 0, blinkUntil = 0;
    int lastHowl = 0, blinkCount = 0;
    for (;;) {
        uint32_t now = System::GetNow();
        fsBypass.Debounce(); fsThicken.Debounce(); fsFlavour.Debounce(); fsTap.Debounce(); btnSolo.Debounce();

        // Short press acts on release; long press acts once while held.
        static bool longDone[5];
        Switch* sw[5] = {&fsBypass, &fsThicken, &fsFlavour, &fsTap, &btnSolo};
        const float longMs[5] = {1e9f, 600.f, 600.f, 2000.f, 2000.f};
        for (int i = 0; i < 5; i++) {
            if (sw[i]->RisingEdge()) longDone[i] = false;
            if (sw[i]->Pressed() && !longDone[i] && sw[i]->TimeHeldMs() > longMs[i]) {
                longDone[i] = true;
                if (i == 1) { recall(storage.GetSettings().current - 1); blinkCount = storage.GetSettings().current + 1; }
                if (i == 2) { recall(storage.GetSettings().current + 1); blinkCount = storage.GetSettings().current + 1; }
                if (i == 3) proc.clearHowl();
                if (i == 4) { saveCurrent(); blinkCount = 3; }
            }
            if (sw[i]->FallingEdge() && !longDone[i]) {
                switch (i) {
                    case 0: params.bypass = !params.bypass; break;
                    case 1: params.thickenOn = !params.thickenOn; break;
                    case 2: params.flavourOn = !params.flavourOn; break;
                    case 3: {
                        uint32_t dt = now - lastTap;
                        if (dt > 250 && dt < 1500) params.bpm = 60000.f / dt;
                        lastTap = now; tapLedOff = now + 60;
                        break;
                    }
                    case 4: params.solo = !params.solo; break;
                }
            }
        }

        // USB MIDI: Program Change n -> preset slot n (0-7).
        midi.Listen();
        while (midi.HasEvents()) {
            MidiEvent e = midi.PopEvent();
            if (e.type == ProgramChange) {
                int pc = e.AsProgramChange().program;
                if (pc < kSlots) { recall(pc); blinkCount = pc + 1; }
            }
        }

        for (int i = 0; i < kPots; i++) applyPot(i);
        params.brightness = readToggle(toneA, toneB);
        params.delayTime  = readToggle(timeA, timeB);
        params.mono       = !monoSw.Read();
        // NOTE: setParams runs outside the audio interrupt. A production build
        // should hand params over with a lock-free double buffer.
        proc.setParams(params);

        // LEDs. The tap LED blinks the preset number after a recall or save.
        if (howlEvents != lastHowl) { lastHowl = howlEvents; notchLedOff = now + 2000; }
        if (blinkCount > 0 && now >= blinkUntil) { blinkUntil = now + 300; blinkCount--; }
        bool blinkOn = now < blinkUntil && (blinkUntil - now) > 150;
        uint8_t leds = 0;
        leds |= (!params.bypass) << 0;
        leds |= (!params.bypass && grDb > 3.f) << 1;
        leds |= (!params.bypass && bleedDb > 6.f) << 2;   // set the bleed trim so this is lit when you're silent
        leds |= params.thickenOn << 3;
        leds |= params.flavourOn << 4;
        leds |= (now < tapLedOff || blinkOn) << 5;
        leds |= (!params.bypass && limDb > 1.f) << 6;
        leds |= (now < notchLedOff || proc.howlNotches() > 0) << 7;
        writeLeds(leds);
        System::Delay(1);
    }
}
