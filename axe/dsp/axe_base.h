// AxeElevator - a pre-amp sound mangler for guitar and bass. DSP core, shared parts.
//
// Goes before an amp sim. One mode runs at a time (see axe.h for the chain and
// the mode list); axe_tone.h holds the tone modes (Tight, Sub, Buzzsaw),
// axe_fx.h the triggered / tempo modes (Glitch, Laser, Dive Bomb, Chiptune,
// Stutter Gate, Ring Shredder, Freeze, Feedback, Swarm, Radio).
//
// Rules these files keep, so the same code runs in Logic, on the Anagram / MOD
// (LV2) and later on a Daisy Seed:
//   - header-only C++17, no heap, no exceptions, no locks, no dependencies
//   - no allocation and no system calls in process()
//   - it never touches CPU flags (flush-to-zero belongs to the host)
//   - no lookahead and no block buffering: zero reported latency
//   - every continuous parameter is smoothed; mode and instrument changes
//     crossfade between two engines, so scene changes don't click or drop out
//   - deterministic: two instances fed the same input after reset() give
//     bit-identical output (Anagram's dual-mono pairing); "random" choices
//     come from seeded generators
//
// Shared building blocks (filters, fast math, oversampling filter, WSOLA
// shifter, Learner, detectors) come from RASP (rasp/dsp/rasp.h), copied here
// so the two projects can evolve separately.
#pragma once

#ifndef AXE_SR_SCALE
#define AXE_SR_SCALE 1      // delay buffers sized for <= 96 kHz; the plugin sets 2 (192 kHz)
#endif

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <initializer_list>

namespace axe {

constexpr float kPi = 3.14159265358979f;
constexpr int kCtl = 16;    // control rate: smoothed parameters and coefficients update every 16 samples

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float dbToLin(float db) { return std::pow(10.f, db / 20.f); }
inline float linToDb(float x) { return 20.f * std::log10(x + 1e-9f); }
inline float expMap(float t, float lo, float hi) { return lo * std::pow(hi / lo, t); }
inline float coefMs(float ms, float sr) { return std::exp(-1.f / (ms * 0.001f * sr)); }

inline float fastTanh(float x) {
    x = clampf(x, -3.f, 3.f);
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}
inline float fastLog2(float x) {
    union { float f; uint32_t i; } v{x};
    float e = (float)((int)((v.i >> 23) & 255) - 127);
    v.i = (v.i & 0x7FFFFFu) | 0x3F800000u;
    float m = v.f - 1.f;
    return e + ((((0.0434289078f * m - 0.187722635f) * m + 0.408721744f) * m - 0.705704158f) * m + 1.44126742f) * m + 3.19081312e-05f;
}
inline float fastExp2(float x) {
    x = clampf(x, -126.f, 126.f);
    float fl = std::floor(x), f = x - fl;
    float p = ((((0.00189438f * f + 0.00894058f) * f + 0.05587656f) * f + 0.24013169f) * f + 0.69315678f) * f + 0.99999977f;
    union { uint32_t i; float f; } v{(uint32_t)((int)fl + 127) << 23};
    return p * v.f;
}
inline float fastDbToLin(float db) { return fastExp2(db * 0.166096404f); }
inline float fastLinToDb(float x) { return 6.02059991f * fastLog2(x + 1e-9f); }
inline float fastPowDb10(float ms) { return 3.01029996f * fastLog2(ms + 1e-12f); }

// Linear up to 0.9, then rounds off to 1.0 (from RASP). Last-resort protection only.
inline float safetyClip(float x) {
    float a = std::fabs(x);
    if (a <= 0.9f) return x;
    float y = 0.9f + 0.1f * fastTanh((a - 0.9f) * 10.f);
    return x < 0.f ? -y : y;
}

// Gain computer: soft-knee static curve in dB. Returns gain change (<= 0 dB).
inline float kneeCurve(float overDb, float ratio, float knee) {
    if (overDb > knee * 0.5f) return overDb * (1.f / ratio - 1.f);
    if (overDb > -knee * 0.5f) { float t = overDb + knee * 0.5f; return (1.f / ratio - 1.f) * t * t / (2.f * knee); }
    return 0.f;
}

struct OnePole {
    float a = 0.f, z = 0.f;
    void setCutoff(float hz, float sr) { a = std::exp(-2.f * kPi * hz / sr); }
    float lp(float x) { z = x + a * (z - x); return z; }
    float hp(float x) { return x - lp(x); }
    void reset() { z = 0.f; }
};

// TPT state-variable filter (Zavalishin) - stable under modulation. Its
// integrators are trapezoidal, so lp is exactly 90 degrees behind bp at every
// frequency: (bp, lp) is a quadrature pair, which the Sub divider relies on.
struct SVF {
    float g = 0, k = 1, ic1 = 0, ic2 = 0, lp = 0, bp = 0, hp = 0;
    void set(float hz, float q, float sr) {
        g = std::tan(kPi * clampf(hz, 5.f, sr * 0.45f) / sr);
        k = 1.f / q;
    }
    void process(float x) {
        float v3 = x - ic2;
        float v1 = (g * v3 + ic1) / (1.f + g * (g + k));
        float v2 = ic2 + g * v1;
        ic1 = 2.f * v1 - ic1; ic2 = 2.f * v2 - ic2;
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
    void reset() { ic1 = ic2 = lp = bp = hp = 0.f; }
};

// Biquad, transposed direct form II. RBJ cookbook designs.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    float d0 = 0, d1 = 0, d2 = 0, e1 = 0, e2 = 0;   // per-sample coefficient increments while gliding
    int left = 0;
    void set(const float* c) { b0 = c[0]; b1 = c[1]; b2 = c[2]; a1 = c[3]; a2 = c[4]; left = 0; }
    // Move to t's coefficients linearly over one control block (kCtl
    // samples) instead of jumping: a coefficient step in a running filter
    // clicks. snap = jump (first use, reset).
    void glideTo(const Biquad& t, bool snap) {
        if (snap) { copyCoefs(t); left = 0; return; }
        const float k = 1.f / kCtl;
        d0 = (t.b0 - b0) * k; d1 = (t.b1 - b1) * k; d2 = (t.b2 - b2) * k; e1 = (t.a1 - a1) * k; e2 = (t.a2 - a2) * k;
        left = kCtl;
    }
    float process(float x) {
        if (left > 0) { b0 += d0; b1 += d1; b2 += d2; a1 += e1; a2 += e2; left--; }
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0.f; }
    void copyCoefs(const Biquad& o) { b0 = o.b0; b1 = o.b1; b2 = o.b2; a1 = o.a1; a2 = o.a2; left = 0; }
    void norm(float B0, float B1, float B2, float A0, float A1, float A2) {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0; left = 0;
    }
    void peaking(float hz, float q, float db, float sr) {
        float A = std::pow(10.f, db / 40.f), w = 2.f * kPi * clampf(hz, 5.f, sr * 0.45f) / sr, al = std::sin(w) / (2.f * q), c = std::cos(w);
        norm(1.f + al * A, -2.f * c, 1.f - al * A, 1.f + al / A, -2.f * c, 1.f - al / A);
    }
    void lowShelf(float hz, float db, float sr) {
        float A = std::pow(10.f, db / 40.f), w = 2.f * kPi * hz / sr, c = std::cos(w);
        float al = std::sin(w) / 2.f * std::sqrt(2.f), s = 2.f * std::sqrt(A) * al;
        norm(A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
             (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
    }
    void highShelf(float hz, float db, float sr) {
        float A = std::pow(10.f, db / 40.f), w = 2.f * kPi * clampf(hz, 5.f, sr * 0.45f) / sr, c = std::cos(w);
        float al = std::sin(w) / 2.f * std::sqrt(2.f), s = 2.f * std::sqrt(A) * al;
        norm(A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
             (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
    }
    void lowpass(float hz, float q, float sr) {
        float w = 2.f * kPi * clampf(hz, 5.f, sr * 0.45f) / sr, al = std::sin(w) / (2.f * q), c = std::cos(w);
        norm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void highpass(float hz, float q, float sr) {
        float w = 2.f * kPi * clampf(hz, 5.f, sr * 0.45f) / sr, al = std::sin(w) / (2.f * q), c = std::cos(w);
        norm((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void allpass(float hz, float q, float sr) {
        float w = 2.f * kPi * clampf(hz, 5.f, sr * 0.45f) / sr, al = std::sin(w) / (2.f * q), c = std::cos(w);
        norm(1 - al, -2 * c, 1 + al, 1 + al, -2 * c, 1 - al);
    }
};

// 10th-order elliptic lowpass for 4x oversampling (from RASP):
// scipy.signal.ellip(10, 0.1, 90, 0.21, output='sos'). Passband to 0.42*fs,
// >= 90 dB down from 0.58*fs. Independent of the base rate.
constexpr float kOS4[5][5] = {
    {2.1738402454e-04f, 2.7823415894e-04f, 2.1738402454e-04f, -1.5801157295e+00f, 6.3849354156e-01f},
    {1.0000000000e+00f, -5.0407447332e-01f, 1.0000000000e+00f, -1.5629856718e+00f, 7.1984100355e-01f},
    {1.0000000000e+00f, -1.1101093521e+00f, 1.0000000000e+00f, -1.5439919071e+00f, 8.2467542187e-01f},
    {1.0000000000e+00f, -1.3141678657e+00f, 1.0000000000e+00f, -1.5367789871e+00f, 9.1036321211e-01f},
    {1.0000000000e+00f, -1.3826713524e+00f, 1.0000000000e+00f, -1.5494885685e+00f, 9.7285772614e-01f},
};
struct OSFilter {
    Biquad s[5];
    void init() { for (int i = 0; i < 5; i++) { s[i].set(kOS4[i]); s[i].reset(); } }
    float process(float x) { for (auto& b : s) x = b.process(x); return x; }
};

template <int N>
struct DelayLine {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    float buf[(size_t)N] = {};
    int w = 0;
    void reset() { for (auto& v : buf) v = 0.f; w = 0; }
    void write(float x) { buf[w] = x; w = (w + 1) & (N - 1); }
    float read(float d) const {
        int di = (int)d;
        float frac = d - (float)di;
        float a = buf[(w - di) & (N - 1)], b = buf[(w - di - 1) & (N - 1)];
        return a + (b - a) * frac;
    }
    float tap(int d) const { return buf[(w - d) & (N - 1)]; }
};

// A control-rate smoothed value: an exponential glide (~25 ms) evaluated once
// per kCtl block, read per sample as a linear ramp across the block, so gains
// never step and filter coefficients move in small increments.
struct Ramp {
    float target = 0.f, cur = 0.f, prev = 0.f, inc = 0.f, v = 0.f, a = 0.f;
    void init(float x, float sr, float ms = 25.f) { a = std::exp(-(float)kCtl / (ms * 0.001f * sr)); snap(x); }
    void snap(float x) { target = cur = prev = v = x; inc = 0.f; }
    void snap() { snap(target); }
    void control() {   // once per block
        prev = cur;
        cur = target + a * (cur - target);
        if (std::fabs(cur - target) < 1e-6f) cur = target;
        v = prev; inc = (cur - prev) * (1.f / kCtl);
    }
    float next() { v += inc; return v; }   // per sample within the block
    bool moving() const { return std::fabs(cur - prev) > 0.f || std::fabs(cur - target) > 0.f; }
};

// ---------------------------------------------------------------------------
// Parameters. Knobs are 0..1 unless a unit is given.
// ---------------------------------------------------------------------------
enum Mode : int {
    kTight = 0, kSub, kBuzzsaw,                                        // tone
    kGlitch, kLaser, kDive, kChip, kStutter, kRing, kFreeze, kFeedback, kSwarm, kRadio,   // fx
    kNumModes
};
enum Instrument : int { kGuitar = 0, kBass, kNumInstruments };
enum Engage : int { kAlways = 0, kWhileTriggered };

inline const char* modeName(int m) {
    static const char* n[kNumModes] = {"Tight", "Sub", "Buzzsaw", "Glitch", "Laser", "Dive Bomb", "Chiptune",
                                       "Stutter Gate", "Ring Shredder", "Freeze", "Feedback", "Swarm", "Radio"};
    return (m >= 0 && m < kNumModes) ? n[m] : "?";
}

// Rate divisions (in beats of a quarter note), shared by every tempo mode.
constexpr int kNumRates = 12;
constexpr float kRateBeats[kNumRates] = {4.f, 2.f, 1.f, 0.5f, 0.25f, 0.125f,           // 1/1 .. 1/32
                                         4.f / 3.f, 2.f / 3.f, 1.f / 3.f, 1.f / 6.f,   // 1/2T .. 1/16T
                                         1.5f, 0.75f};                                 // 1/4D, 1/8D
inline const char* rateName(int r) {
    static const char* n[kNumRates] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/2T", "1/4T", "1/8T", "1/16T", "1/4D", "1/8D"};
    return (r >= 0 && r < kNumRates) ? n[r] : "?";
}

constexpr int kMatchBands = 8;
constexpr float kMatchHz[kMatchBands] = {250.f, 500.f, 1000.f, 1600.f, 2500.f, 4000.f, 6300.f, 10000.f};

struct Params {
    // global
    int   mode = kTight;
    int   instrument = kGuitar;
    float inDb = 0.f;          // input trim, -12..+12 dB
    float gate = 0.f;          // noise gate: 0 off .. 1 (threshold -75..-30 dBFS)
    float mix = 1.f;           // dry/wet, the dry taking the mode's own filter path
    float outDb = 0.f;         // -24..+12 dB
    bool  bypass = false;

    // trigger and clock (global)
    bool  trigger = false;     // the footswitch / button
    int   engage = kAlways;    // Always: the mode runs, Trigger adds its action. While Triggered: only while held
    int   sync = 1;            // 1: follow the host tempo when there is one; 0: always the Tempo knob
    float tempo = 120.f;       // BPM, 40..300
    int   rate = 4;            // kRateBeats index (1/16)
    float swing = 0.f;         // delays every 2nd step: 0 straight .. 1 (75%, hard shuffle)

    // LEARN MY PLAYING: filled in by Learn Playing (and settable by hand)
    float pickSense = 8.f;     // dB jump that counts as a pick attack, 3..16
    float pickHold = 60.f;     // shortest time between pick attacks, 20..250 ms
    float noiseFloor = -70.f;  // your rig's hiss and hum, -100..-30 dBFS
    bool  autoGate = false;    // the gate opens 8 dB above the noise floor (+ Gate x 24 dB)
    float lowNote = 0.f;       // pitch tracker range, Hz; 0 = the instrument's whole range
    float highNote = 0.f;

    // TONE MATCH: a global EQ in front of every floor: Target (learned or preset) minus your tone
    int   target = 0;          // Tone Match target: 0 = the learned one, 1.. = kTargetPresets
    float matchAmount = 1.f;   // 0..1
    float match[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // dB at kMatchHz

    // TIGHT
    float tight = 0.5f;        // compression depth on all three bands
    float punch = 0.5f;        // attack: 0 = clamps instantly, 1 = lets the pick through (~25 ms)
    float release = 0.4f;      // 30..300 ms
    float lowDb = 0.f, midDb = 0.f, highDb = 0.f;   // band levels, -9..+9 dB

    // SUB
    int   subType = 0;         // 0 poly (chords), 1 mono (single notes, WSOLA)
    float sub = 0.5f;          // sub level (0..1 -> 0..+6 dB re the DI note, squared taper)
    float dry = 1.f;           // DI level under the sub
    float subTone = 0.5f;      // lowpass on the sub, 80..1600 Hz
    float range = 0.5f;        // highest note that gets a sub (guitar 150..1200 Hz, bass 80..600 Hz)
    float grit = 0.f;          // saturates the sub: harmonics that keep it audible on small speakers

    // BUZZSAW
    float gain = 0.65f;
    float lows = 0.7f;         // the ~100 Hz lift (bass ~70 Hz), 0..+15 dB
    float highs = 0.7f;        // the ~1 kHz lift (bass ~800 Hz), 0..+15 dB
    float scoop = 0.5f;        // mid scoop depth, 0..-12 dB
    float level = 0.f;         // -18..+12 dB (output is level-matched first)
    float blend = 0.f;         // clean DI under the drive (bass players: try 0.3)

    // GLITCH (chaos 0 = a clean beat repeat)
    float gChaos = 0.4f;       // how often a step does something other than repeat
    float gCrush = 0.f;        // bit / sample-rate crush on the glitched audio
    float gJumps = 0.4f;       // share of pitch jumps (octave / fifth up and down)
    float gTapeStop = 0.3f;    // share of tape stops
    float gReverse = 0.3f;     // share of reversed steps
    float gSmooth = 0.3f;      // de-click fade at slice edges, 0.3..8 ms

    // LASER
    float lPitch = 0.5f;       // start pitch, 200..6000 Hz (unless tracking)
    bool  lTrack = false;      // start two octaves above the note you played
    float lDrop = 0.5f;        // how far it falls: 12..60 semitones
    float lSweep = 0.4f;       // fall time, 20..800 ms
    float lTone = 0.3f;        // 0 sine .. 1 square
    float lLevel = 0.5f;       // 0..+6 dB re your pick, squared taper

    // DIVE BOMB
    float dDepth = 12.f;       // semitones down, 1..36
    float dTime = 0.5f;        // dive time, 1/16 .. 4 beats
    float dReturn = 0.3f;      // return time, 1/16 .. 4 beats
    float dCurve = 0.5f;       // 0 drops fast then slows .. 1 slow start then plunges
    float dWobble = 0.2f;      // vibrato at the bottom
    float dDry = 0.f;          // the untouched DI under the dive

    // CHIPTUNE
    float cPulse = 0.5f;       // pulse width 12.5..50%
    float cCrush = 0.4f;       // 8-bit-ness
    int   cArp = 1;            // Trigger runs it: 0 off, 1 octave, 2 power, 3 major, 4 minor, 5 random
    int   cOctave = 1;         // 0: -1, 1: 0, 2: +1
    float cGlide = 0.2f;       // 0..300 ms
    float cDry = 0.2f;         // the DI under the chip voice

    // STUTTER GATE
    int   sPattern = 0;        // 0 all, 1 offbeat, 2 trance, 3 gallop, 4 chug, 5 random
    float sDuty = 0.5f;        // how much of each step is open
    float sDepth = 1.f;        // how far a closed step dips
    float sShape = 0.3f;       // edge time, 0.5..20 ms
    float sRandom = 0.f;       // random step flips
    float sTone = 0.f;         // closed steps go muffled instead of quiet

    // RING SHREDDER
    int   rType = 0;           // 0 ring modulator, 1 frequency shifter
    float rFreq = 0.4f;        // carrier 20..3000 Hz (Track: octave -2..+2 of your note)
    bool  rTrack = false;
    float rJump = 0.4f;        // Trigger: chance per step that the carrier jumps
    float rGlide = 0.2f;       // carrier glide, 0..300 ms
    float rDrive = 0.2f;       // grit into the modulator

    // FREEZE
    float fzSize = 0.4f;       // captured length, 80..600 ms
    float fzAttack = 0.2f;     // 5..2000 ms
    float fzRelease = 0.4f;    // 20..4000 ms
    float fzDrift = 0.3f;      // grain detune, 0..25 cents
    float fzShimmer = 0.f;     // share of octave-up grains
    float fzLevel = 0.7f;

    // FEEDBACK
    int   fbHarmonic = 1;      // 0 the note, 1 octave, 2 octave + fifth, 3 two octaves
    float fbSwell = 0.4f;      // 50..3000 ms
    float fbRelease = 0.3f;    // 20..3000 ms
    float fbLevel = 0.6f;
    float fbBloom = 0.3f;      // vibrato that grows in
    float fbTone = 0.6f;       // lowpass 1..10 kHz

    // SWARM
    float swDensity = 0.5f;    // 2..120 grains/s (Trigger in Always: x4 burst)
    float swSize = 0.4f;       // 10..250 ms
    float swSpray = 0.4f;      // how far back grains come from, 0..1.5 s
    float swPitch = 0.3f;      // share of shifted grains (octaves, fifths)
    float swReverse = 0.2f;    // share of reversed grains
    float swLevel = 0.7f;

    // RADIO
    float rdBand = 0.5f;       // home station, 300..3000 Hz (bass 150..1500 Hz)
    float rdWidth = 0.4f;      // bandwidth (Q 6 .. 0.7)
    float rdStatic = 0.3f;
    float rdWarble = 0.3f;
    float rdSpin = 0.4f;       // Trigger spins the dial: 50..2000 ms
    float rdFade = 0.3f;       // slow fading in and out
};

// ---------------------------------------------------------------------------
// Envelope / dynamics helpers
// ---------------------------------------------------------------------------
struct Follower {   // peak follower in dB with attack/release
    float att = 0, rel = 0, env = -120.f;
    void set(float attMs, float relMs, float sr) { att = coefMs(attMs, sr); rel = coefMs(relMs, sr); }
    float process(float x) {
        float d = fastLinToDb(std::fabs(x));
        env = d + (d > env ? att : rel) * (env - d);
        return env;
    }
    void reset() { env = -120.f; }
};

// Slow program level: where this band normally sits while you play. The
// compressors set their thresholds relative to it, so Tight compresses the
// same whatever your DI level (they span -30..-6 dBFS in the wild).
struct ProgramLevel {
    float ms = 0.f, msC = 0.f, lvl = -40.f, up = 0.f, down = 0.f;
    void init(float sr) { msC = coefMs(30.f, sr); up = coefMs(400.f, sr); down = coefMs(2500.f, sr); }
    float process(float x, bool playing) {
        ms = x * x + msC * (ms - x * x);
        float d = fastPowDb10(ms);
        if (playing && d > -70.f) lvl = d + (d > lvl ? up : down) * (lvl - d);
        return lvl;
    }
    void reset() { ms = 0.f; lvl = -40.f; }
};

// ---------------------------------------------------------------------------
// WSOLA shifter (from RASP): fixed-ratio pitch shift with two read heads that
// pick their restart point by normalised correlation. Clean on single notes;
// on chords there is no single period to align to, and it warbles - which is
// why Sub's default is the poly divider below.
// ---------------------------------------------------------------------------
class Shifter {
public:
    void init(float sampleRate, float ratio) {
        sr = sampleRate;
        grain = (int)(0.04f * sr); half = grain / 2;
        corr = (int)(0.006f * sr); search = (int)(0.012f * sr) & ~1;
        lead = (int)(0.0065f * sr);
        base = (double)(corr + lead + 2);
        cur = -1.f;
        setRatio(ratio);
    }
    void reset() { dl.reset(); float r = cur; cur = -1.f; setRatio(r); }
    void setRatio(float ratio) {
        if (std::fabs(ratio - cur) < 1e-6f) return;
        cur = ratio; rho = 1.0 - ratio;
        start = rho > 0.0 ? base : base + grain * (-rho + 0.013);
        h[0] = Head{start, 0}; h[1] = Head{start + half * rho, half};
    }
    float process(float x) {
        dl.write(x);
        float y = 0.f;
        for (int k = 0; k < 2; k++) {
            Head& hd = h[k];
            int remaining = grain - hd.age;
            if (remaining == lead) beginSearch(hd, h[1 - k]);
            if (hd.searching && remaining > 0) searchStep(hd, remaining);
            if (hd.age >= grain) { hd.d = start + hd.best; hd.age = 0; hd.searching = false; }
            float sn = std::sin(kPi * (float)hd.age / (float)grain);
            y += sn * sn * dl.read((float)hd.d);
            hd.d += rho; hd.age++;
        }
        return y;
    }
private:
    struct Head {
        double d = 0.0; int age = 0;
        bool searching = false, fine = false; int oR = 0, c = 0, best = 0; double bestScore = 0.0;
    };
    void beginSearch(Head& hd, const Head& other) {
        hd.searching = true; hd.fine = false; hd.c = 0; hd.best = 0; hd.bestScore = -1e30;
        hd.oR = (int)(other.d + rho * lead);
    }
    void searchStep(Head& hd, int delta) {
        int c = hd.c;
        if (!hd.fine) {
            score(hd, c, delta);
            hd.c += 2;
            if (hd.c > search) { hd.fine = true; hd.c = hd.best - 1; }
        } else {
            if (c >= 0 && c <= search && c != hd.best) score(hd, c, delta);
            if (c >= hd.best + 1) { hd.searching = false; return; }
            hd.c += (c + 1 == hd.best) ? 2 : 1;
        }
    }
    void score(Head& hd, int c, int delta) {
        int a0 = hd.oR - delta, b0 = (int)start + c - delta;
        double xy = 0.0, bb = 1e-9;
        for (int m = 0; m < corr; m += 2) {
            double a = dl.tap(a0 - m), b = dl.tap(b0 - m);
            xy += a * b; bb += b * b;
        }
        double sc = xy / std::sqrt(bb);
        if (sc > hd.bestScore) { hd.bestScore = sc; hd.best = c; }
    }
    float sr = 48000.f, cur = -1.f;
    double rho = 0.5, base = 600.0, start = 600.0;
    int grain = 1920, half = 960, corr = 288, search = 576, lead = 312;
    Head h[2];
    DelayLine<8192 * AXE_SR_SCALE> dl;
};

// ---------------------------------------------------------------------------
// Learn + Match (adapted from RASP's Learner/MatchEQ). Learn measures the
// long-term spectrum of what you play in 8 bands, relative to the level of
// the 250 Hz-1 kHz bands (so it doesn't care how hard you hit or how hot the
// DI is). One profile is the Target (learned, or a preset), the other is your
// tone; matchGains turns the difference into the EQ (refined against the real
// peaking-filter cascade). It skips
// silence and only counts while you play.
// ---------------------------------------------------------------------------
constexpr float kLearnStageQ = 1.5f;   // each of the Learner's three band-pass stages

class Learner {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        // three cascaded band-passes per band (~1/2 octave): skirts of -18 dB/octave,
        // so the strong low notes don't leak into the weak high bands of a DI
        for (int i = 0; i < kMatchBands; i++) for (auto& st : band[i]) st.set(kMatchHz[i], kLearnStageQ, sr);
        msC = coefMs(50.f, sr); fall = 10.f / sr;
        need = (int)(12.f * sr);
    }
    void setSeconds(float s) { need = (int)std::fmin(s * sr, 2.0e9f); }   // (an hour-long ask must not overflow)
    void start() { active = true; got = 0; ms = 0.f; runMax = -120.f; for (auto& e : energy) e = 0.0; for (auto& b : band) for (auto& st : b) st.reset(); }
    void cancel() { active = false; }
    bool learning() const { return active; }
    float progress() const { return active ? (float)got / (float)need : 0.f; }
    float heardSeconds() const { return (float)got / sr; }
    bool feed(float x, float* profile) {
        if (!active) return false;
        ms = x * x + msC * (ms - x * x);
        float lvl = fastPowDb10(ms);
        runMax = std::fmax(lvl, runMax - fall);
        bool playing = lvl > -60.f && lvl > runMax - 25.f;
        for (int i = 0; i < kMatchBands; i++) {
            float b = x;
            for (auto& st : band[i]) { st.process(b); b = st.bp * st.k; }
            if (playing) energy[i] += (double)b * b;
        }
        if (playing && ++got >= need) { active = false; result(profile); return true; }
        return false;
    }
    void result(float* profile) const {   // dB per band re the mean of the 250/500/1k bands
        double ref = (energy[0] + energy[1] + energy[2]) / 3.0 + 1e-30;
        for (int i = 0; i < kMatchBands; i++) profile[i] = (float)(10.0 * std::log10((energy[i] + 1e-30) / ref));
    }
private:
    float sr = 48000.f, ms = 0.f, msC = 0.f, runMax = -120.f, fall = 0.f;
    bool active = false;
    int got = 0, need = 576000;
    SVF band[kMatchBands][3];
    double energy[kMatchBands] = {};
};

constexpr float kMatchQ = 1.4f;

// |H| in dB of a biquad at angular frequency w.
inline float biquadDb(const Biquad& b, float w) {
    float cr = std::cos(w), sn = std::sin(w), c2 = std::cos(2.f * w), s2 = std::sin(2.f * w);
    float nr = b.b0 + b.b1 * cr + b.b2 * c2, ni = -(b.b1 * sn + b.b2 * s2);
    float dr = 1.f + b.a1 * cr + b.a2 * c2, di = -(b.a1 * sn + b.a2 * s2);
    return 10.f * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

// target, now: Learner profiles (dB re the 250 Hz-1 kHz bands). Finds the EQ
// gains that make the Learner measure `target` on the EQ'd signal. It models
// the measurement instead of assuming band i moves by gain i: your spectrum
// is interpolated from `now`, passed through the EQ and each band's three
// band-pass stages, and re-normalised to the 250 Hz-1 kHz reference. (On a
// guitar's steep top end a band's energy comes mostly from its lower edge,
// and changing the low bands moves the reference for every band.)
// Low bands are capped at +-3 dB (they depend more on what you played than
// on the instrument), the highs at +-12 dB.
// It runs a few iterations at a time (step), so the audio thread never spends
// more than a sliver of a block on it.
class MatchSolver {
public:
    void begin(const float* target, const float* now) {
        if (!gridReady) initGrid();
        for (int k = 0; k < kN; k++) {
            // spectral density (dB per Hz) between the band centres, from `now`
            const float lf = std::log(f[k]);
            int i = 0; while (i < kMatchBands - 2 && lf > std::log(kMatchHz[i + 1])) i++;
            const float la = std::log(kMatchHz[i]), lb = std::log(kMatchHz[i + 1]);
            const float da = now[i] - 10.f * std::log10(kMatchHz[i]), db = now[i + 1] - 10.f * std::log10(kMatchHz[i + 1]);
            const float u = clampf((lf - la) / (lb - la), -1.5f, 2.5f);   // gentle extrapolation past the ends
            dens[k] = std::pow(10.f, (da + u * (db - da)) / 10.f) * f[k];   // x f: energy per log-frequency step
        }
        for (int i = 0; i < kMatchBands; i++) {
            tgt[i] = target[i]; mine[i] = now[i]; g[i] = 0.f;
            lim[i] = kMatchHz[i] < 1200.f ? 3.f : 12.f;
        }
        measure(g, base);                        // the model's view of `now` with no EQ
        left = kIters;
    }
    bool busy() const { return left > 0; }
    // Runs up to n iterations; true when it has just finished.
    bool step(int n) {
        if (left <= 0) return false;
        // a little smoothing between neighbours: no +11 / -11 dB zig-zags
        // (they'd sound peaky for a fraction of a dB of match)
        constexpr float kSmooth = 0.05f;
        for (; n > 0 && left > 0; n--, left--) {
            float pr[kMatchBands], ng[kMatchBands];
            measure(g, pr);
            for (int i = 0; i < kMatchBands; i++) {
                const float predicted = mine[i] + (pr[i] - base[i]);
                const float l = i > 0 ? g[i - 1] : g[i], r = i < kMatchBands - 1 ? g[i + 1] : g[i];
                ng[i] = clampf(g[i] + 0.7f * (tgt[i] - predicted) - kSmooth * (2.f * g[i] - l - r), -lim[i], lim[i]);
            }
            for (int i = 0; i < kMatchBands; i++) g[i] = ng[i];
        }
        return left == 0;
    }
    void result(float* out) const { for (int i = 0; i < kMatchBands; i++) out[i] = g[i]; }
    static constexpr int kIters = 24;

private:
    static constexpr int kN = 96;                // log grid, 30 Hz - 18 kHz
    static constexpr float kFs = 48000.f;
    void initGrid() {
        const float lf0 = std::log(30.f), lf1 = std::log(18000.f);
        for (int k = 0; k < kN; k++) {
            f[k] = std::exp(lf0 + (lf1 - lf0) * (float)k / (kN - 1));
            const float w = 2.f * kPi * f[k] / kFs;
            cw[k] = std::cos(w); sw[k] = std::sin(w); c2[k] = std::cos(2.f * w); s2[k] = std::sin(2.f * w);
            for (int b = 0; b < kMatchBands; b++) {   // |band-pass|^2, three stages
                const float x = f[k] / kMatchHz[b], q2 = kLearnStageQ * kLearnStageQ;
                const float h = (x * x / q2) / ((1.f - x * x) * (1.f - x * x) + x * x / q2);
                wband[b][k] = h * h * h;
            }
        }
        gridReady = true;
    }
    void measure(const float* gains, float* prof) const {
        Biquad bq[kMatchBands];
        for (int j = 0; j < kMatchBands; j++) bq[j].peaking(kMatchHz[j], kMatchQ, gains[j], kFs);
        double e[kMatchBands] = {};
        for (int k = 0; k < kN; k++) {
            float db = 0.f;
            for (int j = 0; j < kMatchBands; j++) {
                const Biquad& q = bq[j];
                const float nr = q.b0 + q.b1 * cw[k] + q.b2 * c2[k], ni = -(q.b1 * sw[k] + q.b2 * s2[k]);
                const float dr = 1.f + q.a1 * cw[k] + q.a2 * c2[k], di = -(q.a1 * sw[k] + q.a2 * s2[k]);
                db += 10.f * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
            }
            const double sk = (double)dens[k] * std::pow(10.0, db / 10.0);
            for (int b = 0; b < kMatchBands; b++) e[b] += wband[b][k] * sk;
        }
        const double ref = (e[0] + e[1] + e[2]) / 3.0 + 1e-30;
        for (int b = 0; b < kMatchBands; b++) prof[b] = (float)(10.0 * std::log10(e[b] / ref + 1e-30));
    }
    bool gridReady = false;
    int left = 0;
    float f[kN] = {}, dens[kN] = {}, cw[kN] = {}, sw[kN] = {}, c2[kN] = {}, s2[kN] = {}, wband[kMatchBands][kN] = {};
    float tgt[kMatchBands] = {}, mine[kMatchBands] = {}, g[kMatchBands] = {}, base[kMatchBands] = {}, lim[kMatchBands] = {};
};

// All at once (tools and tests).
inline void matchGains(const float* target, const float* now, float* gainsOut) {
    MatchSolver m;
    m.begin(target, now);
    while (!m.step(MatchSolver::kIters)) {}
    m.result(gainsOut);
}

// The Tone Match EQ: 8 peaking bands at kMatchHz, times Amount. Coefficients
// glide (a coefficient step in a running filter clicks).
class MatchEQ {
public:
    void init(float sampleRate) { sr = sampleRate; for (auto& b : eq) b = Biquad{}; on = false; for (auto& g : cur) g = 0.f; curAmt = 0.f; }
    void reset() { for (auto& b : eq) b.reset(); }
    // once per control block
    void set(const float* gains, float amount, bool snap) {
        bool changed = std::fabs(amount - curAmt) > 1e-6f;
        for (int i = 0; i < kMatchBands; i++) changed = changed || std::fabs(gains[i] - cur[i]) > 1e-6f;
        if (!changed && !snap) return;
        curAmt = amount;
        on = false;
        Biquad t;
        for (int i = 0; i < kMatchBands; i++) {
            cur[i] = gains[i];
            const float db = clampf(gains[i], -12.f, 12.f) * clampf(amount, 0.f, 1.f);
            if (std::fabs(db) > 1e-3f) on = true;
            t.peaking(kMatchHz[i], kMatchQ, db, sr);
            eq[i].glideTo(t, snap);
        }
        if (!on) onUntil = snap ? 0 : kCtl;   // let the glide to flat finish
    }
    float process(float x) {
        if (!on) { if (onUntil <= 0) return x; onUntil--; }
        for (auto& b : eq) x = b.process(x);
        return x;
    }
private:
    float sr = 48000.f, curAmt = 0.f;
    float cur[kMatchBands] = {};
    bool on = false;
    int onUntil = 0;
    Biquad eq[kMatchBands];
};

}  // namespace axe
