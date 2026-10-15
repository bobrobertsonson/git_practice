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
//   BYPASS   level-matched digital bypass            /  (held 2 s) LEARN: sing for ~15 s; the
//            effect LED blinks while it listens. The match is saved and applies to every preset
//   THICKEN  thicken on/off                           /  previous preset
//   FLAVOUR  flavour on/off                           /  next preset
//   TAP      tap tempo                                /  (held 2 s) clear feedback notches
// Solo button: short = solo thicken, held 2 s = save the current sound to this preset slot.
// AUX jack (TRS, for a dual momentary footswitch such as a Boss FS-7, or an FS-6
// set to momentary): tip = MEGA, ring = THROW, both on only while held.
//   MEGA   the voice becomes a hard-clipped, mid-focused burst
//   THROW  the voice feeds a long ping-pong delay; repeats ring out after release
// A mono (TS) footswitch shorts the ring, so a switch that reads "held" at
// power-up is ignored until it is first released.
// AUX 2 jack (TRS, same kind of footswitch): tip = STUTTER, ring = GLITCH, on while held.
//   STUTTER  repeats the slice that starts at the press, on the tap/MIDI tempo grid
//   GLITCH   the same, but repeats roll, reverse, drop an octave or cut out at random
// SLICE trim (side panel): stutter slice 1/4, 1/8, 1/16, 1/32 note.
// Both buttons of a dual footswitch at once: AUX = FREEZE (the moment sustains
// as a drone), AUX 2 = TAPE STOP (the voice slows to a stop).
// USB MIDI: Program Change 0-7 recalls preset slots 1-8 (from a laptop/backing-track rig);
// CC 80 = MEGA, CC 81 = THROW (value >= 64 on), for triggering from a timeline;
// CC 86 (>= 64) = start Learn. CC 87 (>= 64 on) = double-track (lead as two hard-panned takes).
// CC 88 = STUTTER, CC 89 = GLITCH (>= 64 on), CC 90 = slice (0-127 -> 1/4..1/32),
// CC 91 = glitch amount (0-127). CC 92 = TAPE STOP, CC 93 = FREEZE, CC 94 = GATE
// (>= 64 on); CC 95 = gate pattern, CC 96 = glitch pattern (0 = random), CC 97 =
// throw style (>= 64 glitch), CC 102 = crush, CC 103 = lo-fi, CC 104 = tape stop time.
// CC 105 / 106 / 107 = Sculpt weight / fill / smooth (also saved per preset slot).
// CC 108 = learned match on/off, CC 109 = compare (held), CC 110 = character
// (0 off, auto, scream, shout, clean), CC 111 = plosive control.
// CC 112 = delay duck, CC 113 = delay swell time, CC 114 = delay time (all 8 values;
// the panel toggle covers the first three).
// CC 82 / 83 = pitch 1 / 2 level (0-127), CC 84 / 85 = pitch 1 / 2 interval
// (0-127 -> -12..+12 semitones). A knob takes over again once moved.
//
// Presets: 8 slots in QSPI flash (4 MB in), first boot fills them with the factory set
// (huge, natural, dark, bright, live, raw, air, natural). After a recall,
// each knob keeps the preset's value until you move it ("pickup").
//
// Bypass: digital and level-matched (your voice doesn't drop ~18 dB when you
// talk between songs).
//
// FAIL-SAFE (BUILD.md, "Fail-safe"):
//  - The bypass relay is held by a heartbeat, not a level: D27 toggles once per
//    audio block (500 Hz) into a charge pump that keeps the relay transistor on.
//    No power, a crash, a hang or a stalled audio interrupt all stop the pulse,
//    and within ~50 ms the relay drops to the hard-wired XLR in -> XLR out path.
//  - Every block is checked: a non-finite sample, or the output silent for 1 s
//    while a voice is coming in, mutes the block and stops the heartbeat; the
//    main loop re-initializes the engine and the pulse resumes (~0.2 s of
//    plain mic).
//  - The STM32 independent watchdog reboots the chip if the main loop stops for
//    4 s (long enough for the bootloader); the relay has long since dropped.
//  - DSP load above 90% sheds harmony pairs 3-4 and freeze, then all harmony and
//    double-track, then thicken, instead of glitching; they come back after 5 s
//    below 65%.
//  - XLR "dry thru" is a passive split of the mic: a clean channel at FOH that
//    works with the pedal unplugged.
//
// Pin map (Daisy Seed: D15..D23 are also A0..A8, D25 is A10, D28 is A11):
//   A0 (D15)  mux 1 out (control 6 + spread, diffuse), CD4051 select D0 D1 D2
//   A1 (D16)  mux 2 out (darken, width + flavour 6),   CD4051 select D3 D4 D5
//   A2..A5 (D17..D20)  faders: control, thicken, flavour, output
//   A6, A7 (D21, D22)  trims: bleed, de-ess
//   A8 (D23)  mux 3 out (glue trim, pitch 1 level, pitch 2 level, pitch 1 interval,
//             pitch 2 interval, AUX 2 tip, AUX 2 ring, slice trim), CD4051 select.
//             AUX 2 tip/ring: 10k pull-up to 3V3 each, the footswitch shorts to
//             the sleeve (ground): reads ~1.0 open, ~0 held
//             shares D0 D1 D2 with mux 1 (libDaisy steps every mux in lockstep)
//   D6/D7  tone toggle    D8/D9  delay-time toggle    D14 solo button   D28 mono switch
//   D10 FS bypass   D11 FS thicken   D12 FS flavour   D13 FS tap
//   D24 / D25 / D26  74HC595 data / clock / latch -> LEDs:
//       Q0 effect on, Q1 chop (>3 dB GR), Q2 bleed reducer active, Q3 thicken,
//       Q4 flavour, Q5 tap / preset blink, Q6 bus limiter, Q7 feedback notch
//   D27 bypass relay driver (via transistor)
//   D29 / D30  AUX jack tip (MEGA) / ring (THROW), sleeve to ground; internal pull-ups
//   USB (micro-USB on the Seed): MIDI device + power while bench-testing
// Check every pin against the Daisy Seed pinout before you build a PCB.
#include "daisy_seed.h"
#include "util/CpuLoadMeter.h"
#include "failsafe.h"
#include "../dsp/rasp.h"
#include "../dsp/presets.h"
#include <cstring>

using namespace daisy;

static DaisySeed hw;
// ~1.5 MB of delay lines: must live in the 64 MB SDRAM, not internal SRAM.
static rasp::Processor DSY_SDRAM_BSS proc;
static rasp::Params params;
static Switch fsBypass, fsThicken, fsFlavour, fsTap, btnSolo, auxMega, auxThrow;
static GPIO toneA, toneB, timeA, timeB, monoSw, srData, srClock, srLatch, relay;
static MidiUsbHandler midi;
static volatile float grDb = 0.f, bleedDb = 0.f, limDb = 0.f;
static volatile int howlEvents = 0;

// ---------------------------------------------------------------- presets in flash
constexpr int kSlots = 8;
// Bump when rasp::Params changes shape (new fields): a stored layout that
// doesn't match is replaced by the factory set instead of read as garbage.
constexpr uint32_t kLayout = 0x52415300u + (uint32_t)sizeof(rasp::Params);
struct Stored {
    uint32_t layout;
    rasp::Params slot[kSlots];
    int current;
    float match[rasp::kMatchBands];   // Learn result: belongs to the singer and mic, not a preset
    float matchAmount;
    bool operator==(const Stored& o) const { return std::memcmp(this, &o, sizeof(Stored)) == 0; }
    bool operator!=(const Stored& o) const { return !(*this == o); }
};
static PersistentStorage<Stored> storage(hw.qspi);

// ---------------------------------------------------------------- analog controls
enum AdcChan { MUX1, MUX2, FADER_CTL, FADER_THK, FADER_FLV, FADER_OUT, TRIM_BLEED, TRIM_DEESS, MUX3, NUM_ADC };
constexpr int kPots = 27;   // 16 mux knobs + 4 faders + 2 trims + mux 3 (glue, pitch 1/2 level, pitch 1/2 interval)

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
    if (i < 22) return hw.adc.GetFloat(FADER_CTL + (i - 16));    // faders, bleed, de-ess
    return hw.adc.GetMuxFloat(MUX3, i - 22);
}

static float faderDb(float v, float lo, float hi) { return lo + (hi - lo) * v; }

// Which pots are fitted. The lean build (make LEAN=1) has 8 knobs: drive,
// drive mix, chop, reverb, delay, thicken level, output, bleed reducer; every
// other setting comes from the preset slot (and MIDI). Unfitted mux inputs are
// tied to ground on the board and never read.
#ifdef RASP_LEAN
static constexpr bool kFitted[kPots] = {
    0, 0, 1, 1, 1, 0, 0, 0,      // mux 1: drive, mix, chop
    0, 0, 0, 1, 0, 1, 0, 0,      // mux 2: reverb (tail blend), delay (delay blend)
    0, 1, 0, 1, 1, 0,            // thicken level, output, bleed
    0, 0, 0, 0, 0};              // mux 3 (AUX 2 and the slice trim are read separately)
#else
static constexpr bool kFitted[kPots] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
#endif

// Writes pot i into params, unless it's still parked after a preset recall.
static void applyPot(int i) {
    if (!kFitted[i]) return;
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
        case 23: params.octDown = v; break;     case 24: params.octUp = v; break;
        // Interval knobs: 25 detent-like steps, -12..+12 semitones.
        case 25: params.pitch1Semi = (int)std::lround(v * 24.f) - 12; break;
        case 26: params.pitch2Semi = (int)std::lround(v * 24.f) - 12; break;
    }
}

static void recall(int slot) {
    Stored& s = storage.GetSettings();
    slot = (slot % kSlots + kSlots) % kSlots;
    bool bypass = params.bypass, mono = params.mono;    // live switches aren't part of a sound
    float bpm = params.bpm;
    params = s.slot[slot];
    params.bypass = bypass; params.mono = mono; params.bpm = bpm;
    for (int b = 0; b < rasp::kMatchBands; b++) params.match[b] = s.match[b];
    params.matchAmount = s.matchAmount;
    s.current = slot;
    for (int i = 0; i < kPots; i++) { pickup[i].at = readPot(i); pickup[i].locked = true; }
}

static void saveCurrent() {
    Stored& s = storage.GetSettings();
    s.slot[s.current] = params;
    s.slot[s.current].mega = s.slot[s.current].throwOn = false;   // momentaries aren't part of a sound
    s.slot[s.current].stutter = s.slot[s.current].glitch = false;
    s.slot[s.current].freeze = s.slot[s.current].tapeStop = s.slot[s.current].gate = false;
    storage.Save();
}

// ---------------------------------------------------------------- audio
static CpuLoadMeter cpuLoad;
static rasp_pedal::HealthCheck health;
static rasp_pedal::LoadShedder shedder;
static volatile bool silenceExpected = false;   // a gate / tape stop / stutter is held: silence is fine
static bool pulse = false;

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    cpuLoad.OnBlockStart();
    if (!health.needsReset())
        for (size_t i = 0; i < size; i++) proc.process(in[0][i], out[0][i], out[1][i]);
    health.check(in[0], out[0], out[1], size, silenceExpected);
    // Heartbeat: the relay stays energized only while this keeps toggling.
    if (health.healthy()) { pulse = !pulse; relay.Write(pulse); }
    cpuLoad.OnBlockEnd();
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
    adc[MUX3].InitMux(seed::A8, 8, seed::D0, seed::D1, seed::D2);
    hw.adc.Init(adc, NUM_ADC);
    hw.adc.Start();

    fsBypass.Init(seed::D10, 1000.f);
    fsThicken.Init(seed::D11, 1000.f);
    fsFlavour.Init(seed::D12, 1000.f);
    fsTap.Init(seed::D13, 1000.f);
    btnSolo.Init(seed::D14, 1000.f);
    auxMega.Init(seed::D29, 1000.f);
    auxThrow.Init(seed::D30, 1000.f);
    toneA.Init(seed::D6, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    toneB.Init(seed::D7, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    timeA.Init(seed::D8, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    timeB.Init(seed::D9, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    monoSw.Init(seed::D28, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    srData.Init(seed::D24, GPIO::Mode::OUTPUT);
    srClock.Init(seed::D25, GPIO::Mode::OUTPUT);
    srLatch.Init(seed::D26, GPIO::Mode::OUTPUT);
    relay.Init(seed::D27, GPIO::Mode::OUTPUT);
    relay.Write(false);            // the heartbeat from the audio callback energizes it

    // Presets: first boot fills flash with the factory set.
    Stored defaults;
    defaults.layout = kLayout;
    for (int i = 0; i < kSlots; i++) defaults.slot[i] = rasp::presetParams(i < rasp::kNumPresets ? i : 1);
    defaults.current = 1;          // natural
    for (int b = 0; b < rasp::kMatchBands; b++) defaults.match[b] = 0.f;
    defaults.matchAmount = 0.f;    // no Learn yet
    // Presets live 4 MB into the QSPI flash, well clear of the program the
    // bootloader stores at its start.
    storage.Init(defaults, 0x400000);
    if (storage.GetSettings().layout != kLayout) storage.RestoreDefaults();
    params = storage.GetSettings().slot[storage.GetSettings().current];
    for (int b = 0; b < rasp::kMatchBands; b++) params.match[b] = storage.GetSettings().match[b];
    params.matchAmount = storage.GetSettings().matchAmount;

    MidiUsbHandler::Config midiCfg;
    midiCfg.transport_config.periph = MidiUsbTransport::Config::INTERNAL;
    midi.Init(midiCfg);
    midi.StartReceive();

    proc.init(hw.AudioSampleRate());
    proc.setParams(params);
    recall(storage.GetSettings().current);
    cpuLoad.Init(hw.AudioSampleRate(), hw.AudioBlockSize());
    health.init(hw.AudioSampleRate(), hw.AudioBlockSize());
    hw.StartAudio(AudioCallback);

    // Independent watchdog: LSI ~32 kHz / 256 = 125 Hz, reload 500 -> ~4 s.
    // Long enough to cover the bootloader after a reset; the relay drops long
    // before it fires.
    static IWDG_HandleTypeDef iwdg;
    iwdg.Instance = IWDG1;
    iwdg.Init.Prescaler = IWDG_PRESCALER_256;
    iwdg.Init.Reload = 500;
    iwdg.Init.Window = IWDG_WINDOW_DISABLE;
    HAL_IWDG_Init(&iwdg);

    uint32_t lastTap = 0, tapLedOff = 0, notchLedOff = 0, blinkUntil = 0;
    int lastHowl = 0, blinkCount = 0;
    // AUX: ignore a contact that is already closed at power-up (a TS plug
    // shorts the ring) until it opens once.
    System::Delay(20);
    auxMega.Debounce(); auxThrow.Debounce();
    bool megaArmed = !auxMega.RawState(), throwArmed = !auxThrow.RawState();
    bool midiMega = false, midiThrow = false;
    // AUX 2 on mux 3 (analog inputs read as switches, with hysteresis).
    bool aux2[2] = {false, false}, aux2Armed[2];
    for (int k = 0; k < 2; k++) aux2Armed[k] = hw.adc.GetMuxFloat(MUX3, 5 + k) > 0.6f;
    bool midiStutter = false, midiGlitch = false, midiTape = false, midiFreeze = false, midiGate = false;
    float lastSliceKnob = -1.f;
    for (;;) {
        uint32_t now = System::GetNow();
        fsBypass.Debounce(); fsThicken.Debounce(); fsFlavour.Debounce(); fsTap.Debounce(); btnSolo.Debounce();
        auxMega.Debounce(); auxThrow.Debounce();
        if (!auxMega.Pressed()) megaArmed = true;
        if (!auxThrow.Pressed()) throwArmed = true;
        for (int k = 0; k < 2; k++) {
            const float v = hw.adc.GetMuxFloat(MUX3, 5 + k);
            if (v < 0.3f) aux2[k] = true; else if (v > 0.6f) { aux2[k] = false; aux2Armed[k] = true; }
        }
        {   // slice trim: only takes over when moved, so MIDI CC 90 can set it too
            const float v = hw.adc.GetMuxFloat(MUX3, 7);
            if (lastSliceKnob < 0.f || std::fabs(v - lastSliceKnob) > 0.08f) {
                lastSliceKnob = v;
                params.stutterDiv = std::min(3, (int)(v * 4.f));
            }
        }

        // Short press acts on release; long press acts once while held.
        static bool longDone[5];
        Switch* sw[5] = {&fsBypass, &fsThicken, &fsFlavour, &fsTap, &btnSolo};
        const float longMs[5] = {2000.f, 600.f, 600.f, 2000.f, 2000.f};
        for (int i = 0; i < 5; i++) {
            if (sw[i]->RisingEdge()) longDone[i] = false;
            if (sw[i]->Pressed() && !longDone[i] && sw[i]->TimeHeldMs() > longMs[i]) {
                longDone[i] = true;
                if (i == 0) proc.startLearn();
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
            if (e.type == ControlChange) {
                auto cc = e.AsControlChange();
                if (cc.control_number == 80) midiMega = cc.value >= 64;
                if (cc.control_number == 81) midiThrow = cc.value >= 64;
                if (cc.control_number == 82) params.octDown = cc.value / 127.f;
                if (cc.control_number == 83) params.octUp = cc.value / 127.f;
                if (cc.control_number == 86 && cc.value >= 64) proc.startLearn();
                if (cc.control_number == 84) params.pitch1Semi = (int)std::lround(cc.value * 24.f / 127.f) - 12;
                if (cc.control_number == 85) params.pitch2Semi = (int)std::lround(cc.value * 24.f / 127.f) - 12;
                if (cc.control_number == 87) params.doubleTrack = cc.value >= 64;
                if (cc.control_number == 88) midiStutter = cc.value >= 64;
                if (cc.control_number == 89) midiGlitch = cc.value >= 64;
                if (cc.control_number == 90) params.stutterDiv = std::min(3, cc.value / 32);
                if (cc.control_number == 91) params.glitchAmount = cc.value / 127.f;
                if (cc.control_number == 92) midiTape = cc.value >= 64;
                if (cc.control_number == 93) midiFreeze = cc.value >= 64;
                if (cc.control_number == 94) midiGate = cc.value >= 64;
                if (cc.control_number == 95) params.gatePattern = std::min(3, cc.value / 32);
                if (cc.control_number == 96) params.glitchPattern = std::min(4, cc.value * 5 / 128);
                if (cc.control_number == 97) params.throwStyle = cc.value >= 64 ? 1 : 0;
                if (cc.control_number == 102) params.crush = cc.value / 127.f;
                if (cc.control_number == 103) params.lofi = cc.value / 127.f;
                if (cc.control_number == 104) params.tapeTime = std::min(3, cc.value / 32);
                if (cc.control_number == 105) params.weight = cc.value / 127.f;
                if (cc.control_number == 106) params.fill = cc.value / 127.f;
                if (cc.control_number == 107) params.smooth = cc.value / 127.f;
                if (cc.control_number == 108) params.matchOn = cc.value >= 64;
                if (cc.control_number == 109) params.compare = cc.value >= 64;
                if (cc.control_number == 110) params.character = std::min(4, cc.value * 5 / 128);
                if (cc.control_number == 111) params.plosive = cc.value / 127.f;
                if (cc.control_number == 112) params.delayDuck = cc.value / 127.f;
                if (cc.control_number == 113) params.duckRelease = cc.value / 127.f;
                if (cc.control_number == 114) params.delayTime = std::min(7, cc.value / 16);
                if (cc.control_number == 115) params.voxMode = std::min(4, cc.value * 5 / 128);
                if (cc.control_number == 116) params.voxMix = cc.value / 127.f;
                if (cc.control_number == 117) params.voxNote = std::min(24, cc.value * 25 / 128);
                if (cc.control_number == 118) params.voxChord = std::min(5, cc.value * 6 / 128);
                if (cc.control_number == 119) params.voxFollow = cc.value >= 64;
                if (cc.control_number == 20) params.voxFormant = cc.value / 127.f;
                if (cc.control_number == 21) params.voxRing = cc.value / 127.f;
            }
        }

        for (int i = 0; i < kPots; i++) applyPot(i);
        params.brightness = readToggle(toneA, toneB);
        params.delayTime  = readToggle(timeA, timeB);
        params.mono       = !monoSw.Read();
        {   // Learn finished: keep it for every preset and across power cycles.
            float g[rasp::kMatchBands];
            if (proc.learnedMatch(g)) {
                Stored& st = storage.GetSettings();
                for (int b = 0; b < rasp::kMatchBands; b++) params.match[b] = st.match[b] = g[b];
                params.matchAmount = st.matchAmount = 1.f;
                storage.Save();
                blinkCount = 2;
            }
        }
        // Both buttons of a dual footswitch together: AUX = FREEZE, AUX 2 = TAPE STOP.
        const bool fsMega = megaArmed && auxMega.Pressed(), fsThrow = throwArmed && auxThrow.Pressed();
        const bool fsStut = aux2Armed[0] && aux2[0], fsGlit = aux2Armed[1] && aux2[1];
        const bool auxBoth = fsMega && fsThrow, aux2Both = fsStut && fsGlit;
        params.mega       = (fsMega && !auxBoth) || midiMega;
        params.throwOn    = (fsThrow && !auxBoth) || midiThrow;
        params.freeze     = auxBoth || midiFreeze;
        params.stutter    = (fsStut && !aux2Both) || midiStutter;
        params.glitch     = (fsGlit && !aux2Both) || midiGlitch;
        params.tapeStop   = aux2Both || midiTape;
        params.gate       = midiGate;
        // Fail-safe housekeeping: feed the watchdog, recover from a fault,
        // shed load.
        HAL_IWDG_Refresh(&iwdg);
        silenceExpected = params.gate || params.tapeStop || params.stutter || params.glitch;
        if (health.needsReset()) {            // audio is muted, relay on the plain mic
            proc.init(hw.AudioSampleRate());
            System::Delay(200);
            health.recovered();
            blinkCount = 1;                   // one blink: a fault was caught
        }
        shedder.update(cpuLoad.GetAvgCpuLoad(), now);
        // NOTE: setParams runs outside the audio interrupt. A production build
        // should hand params over with a lock-free double buffer.
        proc.setParams(shedder.apply(params));

        // LEDs. The tap LED blinks the preset number after a recall or save.
        if (howlEvents != lastHowl) { lastHowl = howlEvents; notchLedOff = now + 2000; }
        if (blinkCount > 0 && now >= blinkUntil) { blinkUntil = now + 300; blinkCount--; }
        bool blinkOn = now < blinkUntil && (blinkUntil - now) > 150;
        uint8_t leds = 0;
        const bool learnBlink = proc.learning() && (now / 120) % 2;
        leds |= (!params.bypass && !learnBlink) << 0;
        leds |= (!params.bypass && grDb > 3.f) << 1;
        leds |= (!params.bypass && bleedDb > 6.f) << 2;   // set the bleed trim so this is lit when you're silent
        leds |= params.thickenOn << 3;
        leds |= params.flavourOn << 4;
        leds |= (now < tapLedOff || blinkOn || params.mega || params.throwOn || params.stutter || params.glitch || params.freeze || params.tapeStop || params.gate) << 5;   // also lit while a momentary is held
        leds |= (!params.bypass && limDb > 1.f) << 6;
        leds |= (now < notchLedOff || proc.howlNotches() > 0) << 7;
        writeLeds(leds);
        System::Delay(1);
    }
}
