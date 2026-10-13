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
//   TRS MIDI in    CC 80 = MEGA, CC 81 = THROW (value >= 64 on), CC 82 / 83 = pitch 1 / 2 level, CC 84 / 85 = pitch 1 / 2 interval (-12..+12 st), CC 87 = double-track, CC 88 / 89 = STUTTER / GLITCH (held), CC 90 = slice, CC 91 = glitch amount, CC 92-94 = TAPE STOP / FREEZE / GATE (held), CC 95 = gate pattern, CC 96 = glitch pattern, CC 97 = throw style, CC 102 = crush, CC 103 = lo-fi, CC 104 = tape stop time, CC 105-107 = weight / fill / smooth, CC 108 match on, CC 109 compare, CC 110 character, CC 111 plosive, CC 112 delay duck, CC 113 delay swell, CC 114 delay time
//   LED 2          red = chop compressing > 3 dB, blue = feedback notch active,
//                  white = a momentary effect is held
#include "daisy_pod.h"
#include "../../dsp/rasp.h"
#include "../../dsp/presets.h"
#include "util/CpuLoadMeter.h"
#include "../failsafe.h"

using namespace daisy;

static DaisyPod hw;
static rasp::Processor DSY_SDRAM_BSS proc;
static rasp::Params params;
static int preset = 0, gainStep = 2;
static volatile float grDb = 0.f;

// Fail-safes (daisy/failsafe.h): the Pod has no bypass relay, so a fault mutes
// and the engine restarts; load shedding is the same as the pedal's.
static CpuLoadMeter cpuLoad;
static rasp_pedal::HealthCheck health;
static rasp_pedal::LoadShedder shedder;
static volatile bool silenceExpected = false;

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    cpuLoad.OnBlockStart();
    if (!health.needsReset())
        for (size_t i = 0; i < size; i++) proc.process(in[0][i], out[0][i], out[1][i]);
    health.check(in[0], out[0], out[1], size, silenceExpected);
    cpuLoad.OnBlockEnd();
    grDb = proc.gainReductionDb();
}

static void loadPreset() {
    bool bypass = params.bypass, thk = params.thickenOn, flv = params.flavourOn;
    bool mega = params.mega, thr = params.throwOn, st = params.stutter, gl = params.glitch;
    int sdiv = params.stutterDiv; float gamt = params.glitchAmount;
    bool ts = params.tapeStop, fz = params.freeze, gt = params.gate;
    int gpat = params.glitchPattern, gtp = params.gatePattern, tst = params.throwStyle, ttm = params.tapeTime;
    float cr = params.crush, lf = params.lofi;
    float od = params.octDown, ou = params.octUp;           // set over MIDI: a performance setting
    int s1 = params.pitch1Semi, s2 = params.pitch2Semi;
    float match[rasp::kMatchBands], amt = params.matchAmount;          // Learn result: the singer's, not the preset's
    for (int b = 0; b < rasp::kMatchBands; b++) match[b] = params.match[b];
    params = rasp::presetParams(preset);
    params.bypass = bypass; params.mega = mega; params.throwOn = thr;
    params.stutter = st; params.glitch = gl; params.stutterDiv = sdiv; params.glitchAmount = gamt;
    params.tapeStop = ts; params.freeze = fz; params.gate = gt; params.glitchPattern = gpat; params.gatePattern = gtp;
    params.throwStyle = tst; params.tapeTime = ttm; params.crush = cr; params.lofi = lf;
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
    cpuLoad.Init(hw.AudioSampleRate(), hw.AudioBlockSize());
    health.init(hw.AudioSampleRate(), hw.AudioBlockSize());
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
                if (cc.control_number == 87) params.doubleTrack = cc.value >= 64;
                if (cc.control_number == 88) params.stutter = cc.value >= 64;
                if (cc.control_number == 89) params.glitch = cc.value >= 64;
                if (cc.control_number == 90) params.stutterDiv = std::min(3, cc.value / 32);
                if (cc.control_number == 91) params.glitchAmount = cc.value / 127.f;
                if (cc.control_number == 92) params.tapeStop = cc.value >= 64;
                if (cc.control_number == 93) params.freeze = cc.value >= 64;
                if (cc.control_number == 94) params.gate = cc.value >= 64;
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
        silenceExpected = params.gate || params.tapeStop || params.stutter || params.glitch;
        if (health.needsReset()) { proc.init(hw.AudioSampleRate()); System::Delay(200); health.recovered(); }
        shedder.update(cpuLoad.GetAvgCpuLoad(), now);
        proc.setParams(shedder.apply(params));

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
