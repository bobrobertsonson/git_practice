// AxeElevator - pre-amp DI enhancer for guitar and bass. DSP core.
//
// Goes on a clean DI, before an amp sim. One mode runs at a time:
//
//   in ─► input trim ─► [ MODE ] ─► mix (phase-aligned dry) ─► gate ─► output ─► bypass xfade ─► out
//                          │
//                          ├─ TIGHT    3-band compressor: the lows clamp fast so palm mutes don't flub
//                          ├─ SUB      octave down blended with the DI. Poly: a filter-bank divider
//                          │           that holds up on chords. Mono: RASP's WSOLA shifter (single notes)
//                          ├─ STRINGS  "new strings": harmonic exciter, transient clarity, air, and a
//                          │           Learn match EQ (fresh-strings DI -> pull dull strings toward it)
//                          └─ BUZZSAW  HM-2-style heavy drive: big lifts near 100 Hz and 1 kHz, scooped
//                                      middle, 4x oversampled clippers with antiderivative anti-aliasing
//
// Every mode has guitar and bass voicings (crossovers, filter corners, EQ centres).
//
// Rules this file keeps, so the same code runs in Logic, on the Anagram / MOD
// Dwarf (LV2) and later on a Daisy Seed:
//   - header-only C++17, no heap, no exceptions, no locks, no dependencies
//   - no allocation and no system calls in process()
//   - it never touches CPU flags (flush-to-zero belongs to the host)
//   - no lookahead and no block buffering: zero reported latency
//   - every continuous parameter is smoothed; mode and instrument changes
//     crossfade between two engines, so scene changes don't click or drop out
//   - deterministic: two instances fed the same input after reset() give
//     bit-identical output (Anagram's dual-mono pairing)
//
// Shared building blocks (filters, fast math, oversampling filter, WSOLA
// shifter, Learner/MatchEQ, leveler-style detectors) come from RASP
// (rasp/dsp/rasp.h), copied here so the two projects can evolve separately.
#pragma once

#ifndef AXE_SR_SCALE
#define AXE_SR_SCALE 1      // delay buffers sized for <= 96 kHz; the plugin sets 2 (192 kHz)
#endif

#include <cmath>
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
enum Mode : int { kTight = 0, kSub, kStrings, kBuzzsaw, kNumModes };
enum Instrument : int { kGuitar = 0, kBass, kNumInstruments };

inline const char* modeName(int m) {
    static const char* n[kNumModes] = {"Tight", "Sub", "New Strings", "Buzzsaw"};
    return (m >= 0 && m < kNumModes) ? n[m] : "?";
}

constexpr int kMatchBands = 8;
constexpr float kMatchHz[kMatchBands] = {250.f, 500.f, 1000.f, 1600.f, 2500.f, 4000.f, 6300.f, 10000.f};

struct Params {
    // global
    int   mode = kTight;
    int   instrument = kGuitar;
    float inDb = 0.f;          // input trim, -12..+12 dB
    float gate = 0.f;          // 0 off .. 1 (threshold -75..-30 dBFS)
    float mix = 1.f;           // dry/wet, the dry taking the mode's own filter path
    float outDb = 0.f;         // -24..+12 dB
    bool  bypass = false;

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

    // NEW STRINGS
    float bright = 0.5f;       // harmonic exciter: rebuilds the zing dull strings lost
    float clarity = 0.5f;      // pick-attack presence
    float airDb = 2.f;         // high shelf, 0..9 dB
    float matchAmount = 1.f;   // how far to pull toward the learned fresh strings (gains are 0 until Learn)
    float match[kMatchBands] = {0, 0, 0, 0, 0, 0, 0, 0};   // learned EQ, dB (set by Learn)

    // BUZZSAW
    float gain = 0.65f;
    float lows = 0.7f;         // the ~100 Hz lift (bass ~70 Hz), 0..+15 dB
    float highs = 0.7f;        // the ~1 kHz lift (bass ~800 Hz), 0..+15 dB
    float scoop = 0.5f;        // mid scoop depth, 0..-12 dB
    float level = 0.f;         // -18..+12 dB (output is level-matched first)
    float blend = 0.f;         // clean DI under the drive (bass players: try 0.3)
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
// TIGHT: 3-band compressor.
// Linkwitz-Riley 4th-order split (LP+HP of an LR4 pair sums to a 2nd-order
// allpass, so the low band also goes through the high crossover's allpass
// and the three bands sum flat). Each band has a feed-forward compressor
// whose threshold rides that band's own program level. The low band gets the
// fastest attack and the steepest ratio: a palm mute's lows bloom 10-40 ms
// after the pick, and clamping that bloom is what "tight" means; Punch
// lengthens the attack so the pick click itself still gets through.
// ---------------------------------------------------------------------------
class Tight {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        for (auto& p : prog) p.init(sr);
        playing.set(1.f, 300.f, sr);
        tight.init(0.5f, sr); punch.init(0.5f, sr); rel.init(0.4f, sr);
        for (auto& g : bandDb) g.init(0.f, sr);
    }
    void configure(int instrument) {
        inst = instrument;
        const bool bass = inst == kBass;
        float xl = bass ? 90.f : 150.f, xh = bass ? 800.f : 1800.f;
        hpIn[0].highpass(bass ? 28.f : 60.f, 0.7071f, sr); hpIn[1] = hpIn[0];
        for (int i = 0; i < 2; i++) {
            lpL[i].lowpass(xl, 0.7071f, sr); hpL[i].highpass(xl, 0.7071f, sr);
            lpH[i].lowpass(xh, 0.7071f, sr); hpH[i].highpass(xh, 0.7071f, sr);
        }
        apH.allpass(xh, 0.7071f, sr);
        reset();
    }
    void setTargets(const Params& p) {
        tight.target = p.tight; punch.target = p.punch; rel.target = p.release;
        bandDb[0].target = p.lowDb; bandDb[1].target = p.midDb; bandDb[2].target = p.highDb;
    }
    void snap() { tight.snap(); punch.snap(); rel.snap(); for (auto& g : bandDb) g.snap(); first = true; control(); }
    void reset() {
        for (auto* b : {&hpIn[0], &hpIn[1], &lpL[0], &lpL[1], &hpL[0], &hpL[1], &lpH[0], &lpH[1], &hpH[0], &hpH[1], &apH}) b->reset();
        for (auto& p : prog) p.reset();
        for (auto& g : gr) g = 0.f;
        playing.reset();
    }
    void control() {
        tight.control(); punch.control(); rel.control();
        for (auto& g : bandDb) g.control();
        for (int i = 0; i < 3; i++) { offPrev[i] = offsetDb[i]; mkPrev[i] = makeupDb[i]; ratPrev[i] = ratio[i]; }
        if (tight.moving() || punch.moving() || rel.moving() || first) updateCoefs();
        if (first) for (int i = 0; i < 3; i++) { offPrev[i] = offsetDb[i]; mkPrev[i] = makeupDb[i]; ratPrev[i] = ratio[i]; }
        first = false;
        ph = 0;
    }
    float process(float x, float& dry) {
        x = hpIn[1].process(hpIn[0].process(x));
        float lo = lpL[1].process(lpL[0].process(x));
        float rest = hpL[1].process(hpL[0].process(x));
        float mid = lpH[1].process(lpH[0].process(rest));
        float hi = hpH[1].process(hpH[0].process(rest));
        lo = apH.process(lo);
        dry = lo + mid + hi;
        bool on = playing.process(x) > -60.f;
        float b[3] = {lo, mid, hi}, y = 0.f;
        // the dynamics settings glide per sample across the block (a block
        // step in threshold, ratio or makeup would click on a fast attack)
        const float f = (float)(++ph) * (1.f / kCtl);
        for (int i = 0; i < 3; i++) {
            float lvl = prog[i].process(b[i], on);
            float thr = lvl + lerp(offPrev[i], offsetDb[i], f);
            float target = kneeCurve(fastLinToDb(std::fabs(b[i])) - thr, lerp(ratPrev[i], ratio[i], f), 6.f);
            gr[i] = target + (target < gr[i] ? attC[i] : relC[i]) * (gr[i] - target);
            y += b[i] * fastDbToLin(gr[i] + lerp(mkPrev[i], makeupDb[i], f) + bandDb[i].next());
        }
        return y;
    }
    float gainReductionDb(int band) const { return -gr[band]; }
private:
    void updateCoefs() {
        float t = tight.cur, pu = punch.cur;
        // Thresholds sit relative to each band's program level: at Tight 1
        // the low band compresses from 9 dB below its average, 8:1.
        static const float baseRatio[3] = {2.f, 1.5f, 1.3f}, addRatio[3] = {6.f, 2.5f, 1.2f};
        static const float depth[3] = {15.f, 10.f, 8.f};
        float attMs[3] = {expMap(pu, 0.3f, 18.f), expMap(pu, 1.f, 25.f), expMap(pu, 0.5f, 15.f)};
        float relMs = expMap(rel.cur, 30.f, 300.f);
        float relScale[3] = {0.7f, 1.f, 0.8f};
        for (int i = 0; i < 3; i++) {
            ratio[i] = 1.f + (baseRatio[i] - 1.f + addRatio[i]) * t;
            ratio[i] = ratio[i] < 1.01f ? 1.01f : ratio[i];
            offsetDb[i] = 6.f - depth[i] * t;
            attC[i] = coefMs(attMs[i], sr);
            relC[i] = coefMs(relMs * relScale[i], sr);
            // Auto makeup: give back what a band a little above its average level loses.
            makeupDb[i] = -1.0f * kneeCurve(-offsetDb[i] + 6.f, ratio[i], 6.f);
        }
    }
    float sr = 48000.f;
    int inst = kGuitar;
    bool first = true;
    Biquad hpIn[2], lpL[2], hpL[2], lpH[2], hpH[2], apH;
    ProgramLevel prog[3];
    Follower playing;
    Ramp tight, punch, rel, bandDb[3];
    float ratio[3] = {2, 1.5f, 1.3f}, offsetDb[3] = {0, 0, 0}, attC[3] = {}, relC[3] = {}, makeupDb[3] = {}, gr[3] = {};
    float ratPrev[3] = {2, 1.5f, 1.3f}, offPrev[3] = {}, mkPrev[3] = {};
    int ph = 0;
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
// SUB, poly: a filter-bank frequency divider.
//
// The input is split into narrow bands (1/12 octave apart). In each band a
// cascade of two state-variable filters gives a quadrature pair (bp, lp),
// i.e. the band as a rotating phasor z = A e^{i phi}. Halving the rotation,
// w = sqrt(z), with the sign chosen so w moves continuously, gives that
// band's content an octave down, at the band's own amplitude: out = A cos(phi/2).
// Each note's partials land in different bands, so a chord divides note by
// note instead of fighting over one period (what makes a single-voice
// shifter warble on chords). A square root has two signs, so a partial that
// straddles two bands could come out of them in opposite polarity and cancel;
// each band therefore also lines its polarity up with its lower neighbour
// whenever the two carry the same partial (high short-term coherence).
//
// There is no buffering: the dry DI is untouched and the plugin reports zero
// latency. What you trade instead is bloom time - a narrow band takes time
// to ring up (~2Q/omega): the sub of a low E reaches full level ~40-70 ms
// after the pick, higher notes faster (measured: low E ~45-75 ms, A3 ~35 ms).
// Q is fixed at 8.5: at Q 8 every note of E5, A5, open E, open G, drop-C,
// drop-A and a close low E-G#-B triad gets its own sub within ~2 dB; at Q 6
// the close triad loses a note, and lower Q barely blooms faster.
// ---------------------------------------------------------------------------
class SubPoly {
public:
    static constexpr int kMaxBands = 56;
    // The bank runs decimated to ~6 kHz (everything it touches is below
    // ~1.7 kHz): 1/8 the work at 48 kHz. 4th-order lowpasses at 1.6 kHz
    // before decimation and after the zero-stuffed interpolation.
    void init(float sampleRate) {
        sr = sampleRate;
        dec = (int)std::lround(sr / 6000.f); if (dec < 1) dec = 1;
        srD = sr / (float)dec;
        aa[0].lowpass(1600.f, 0.5412f, sr); aa[1].lowpass(1600.f, 1.3066f, sr);
        ai[0] = aa[0]; ai[1] = aa[1];
    }
    float process(float x) {
        float xa = aa[1].process(aa[0].process(x));
        float u = 0.f;
        if (++phase >= dec) { phase = 0; u = processBank(xa) * (float)dec; }
        return ai[1].process(ai[0].process(u));
    }
    void configure(int instrument) {
        const bool bass = instrument == kBass;
        f0 = bass ? 28.f : 50.f;
        nb = kMaxBands;   // 56 bands at 1/12 octave, 4.6 octaves: guitar 50 Hz..1.3 kHz (drop A), bass 28..720 Hz (low B)
        for (int k = 0; k < nb; k++) {
            fc[k] = f0 * std::pow(2.f, k / 12.f);
            post[k].set(fc[k] * 0.5f, 1.3f, srD);
        }
        curQ = -1.f;
        reset();
    }
    void reset() {
        for (auto* f : {&aa[0], &aa[1], &ai[0], &ai[1]}) f->reset();
        phase = 0;
        for (int k = 0; k < kMaxBands; k++) {
            a[k].reset(); b[k].reset(); post[k].reset();
            wr[k] = 1.f; wi[k] = 0.f; sig[k] = 1.f; sigT[k] = 1.f; cr[k] = ci[k] = cm[k] = 0.f; zre[k] = zim[k] = zm[k] = sm[k] = 0.f;
        }
    }
    // q: analysis sharpness; rangeHz: top of the divided range; weights fade over half an octave.
    void set(float q, float rangeHz) {
        if (std::fabs(q - curQ) > 1e-3f) {
            curQ = q;
            // narrower bands see less of each partial; measured: -4.3 dB per doubling of Q from 4
            cal = 0.6f * dbToLin(4.3f * std::log2(q / 4.f));
            for (int k = 0; k < nb; k++) { a[k].set(fc[k], q, srD); b[k].set(fc[k], q, srD); }
        }
        for (int k = 0; k < nb; k++) {
            float oct = std::log2(rangeHz / fc[k]);
            weight[k] = clampf(oct * 2.f + 1.f, 0.f, 1.f);
        }
        cohC = coefMs(40.f, srD);
        sigC = coefMs(4.f, srD);
        magC = coefMs(4.f, srD);
    }
private:
    float processBank(float x) {
        // pass 1: each band's phasor z, its continuous square root w, and a
        // smoothed magnitude for the dominance weights
        for (int k = 0; k < nb; k++) {
            if (weight[k] <= 0.f) { zm[k] = 0.f; sm[k] = 0.f; continue; }
            a[k].process(x);
            b[k].process(a[k].bp * a[k].k);
            float zr = b[k].bp * b[k].k, zi = b[k].lp * b[k].k;
            float mag = std::sqrt(zr * zr + zi * zi);
            float wr_ = std::sqrt(0.5f * (mag + zr)), wi_ = std::sqrt(clampf(0.5f * (mag - zr), 0.f, 1e30f));
            if (zi < 0.f) wi_ = -wi_;
            if (wr_ * wr[k] + wi_ * wi[k] < 0.f) { wr_ = -wr_; wi_ = -wi_; }   // stay on the continuous branch
            wr[k] = wr_; wi[k] = wi_;
            zre[k] = zr; zim[k] = zi; zm[k] = mag;
            sm[k] = mag + magC * (sm[k] - mag);
        }
        // pass 2: dominance weights (a band speaks for a partial only where
        // it is the strongest of its neighbours: bands between two chord
        // notes beat and would add noise), polarity alignment, divide
        float y = 0.f;
        float prevWr = 0.f, prevWi = 0.f, prevDom = 0.f;
        for (int k = 0; k < nb; k++) {
            if (weight[k] <= 0.f) { prevWr = prevWi = 0.f; prevDom = 0.f; continue; }
            float c = sm[k] * sm[k] * sm[k];
            float l = k > 0 ? sm[k - 1] : 0.f, r = k + 1 < nb ? sm[k + 1] : 0.f;
            float dom = c / (c + l * l * l + r * r * r + 1e-30f);
            // polarity alignment with the lower band when both hold the same partial
            if (k > 0 && zm[k - 1] > 0.f) {
                float pr = zre[k] * zre[k - 1] + zim[k] * zim[k - 1], pi = zim[k] * zre[k - 1] - zre[k] * zim[k - 1];
                cr[k] = pr + cohC * (cr[k] - pr); ci[k] = pi + cohC * (ci[k] - pi);
                float pm = zm[k] * zm[k - 1];
                cm[k] = pm + cohC * (cm[k] - pm);
                float coh = std::sqrt(cr[k] * cr[k] + ci[k] * ci[k]) / (cm[k] + 1e-20f);
                // only between two bands that both speak for one partial: a
                // band holding two chord notes beats, and its coherence with
                // a neighbour drops over the 40 ms window
                if (coh > 0.9f && dom > 0.2f && prevDom > 0.2f && cm[k] > 1e-10f) {
                    float m2 = std::sqrt(pr * pr + pi * pi);
                    float hr = std::sqrt(0.5f * (m2 + pr)), hi = std::sqrt(clampf(0.5f * (m2 - pr), 0.f, 1e30f));
                    if (pi < 0.f) hi = -hi;
                    float qr = wr[k] * prevWr + wi[k] * prevWi, qi = wi[k] * prevWr - wr[k] * prevWi;
                    sigT[k] = (qr * hr + qi * hi) >= 0.f ? 1.f : -1.f;
                }
            }
            sig[k] = sigT[k] + sigC * (sig[k] - sigT[k]);
            // the partial's amplitude from this band and its neighbours (a
            // partial between two bands is seen weaker by both)
            float amp = std::sqrt(sm[k] * sm[k] + l * l + r * r);
            float cosHalf = wr[k] / (std::sqrt(zm[k]) + 1e-15f);
            post[k].process(cosHalf * amp * sig[k] * dom);
            y += weight[k] * post[k].bp * post[k].k;
            float sgn = sig[k] >= 0.f ? 1.f : -1.f;
            prevWr = wr[k] * sgn; prevWi = wi[k] * sgn; prevDom = dom;
        }
        return y * cal;
    }
    float sr = 48000.f, srD = 6000.f, f0 = 50.f, curQ = -1.f, cal = 0.6f, cohC = 0.f, sigC = 0.f, magC = 0.f;
    int nb = kMaxBands, dec = 8, phase = 0;
    Biquad aa[2], ai[2];
    float fc[kMaxBands] = {}, weight[kMaxBands] = {};
    SVF a[kMaxBands], b[kMaxBands], post[kMaxBands];
    float wr[kMaxBands] = {}, wi[kMaxBands] = {}, sig[kMaxBands] = {}, sigT[kMaxBands] = {};
    float cr[kMaxBands] = {}, ci[kMaxBands] = {}, cm[kMaxBands] = {};
    float zre[kMaxBands] = {}, zim[kMaxBands] = {}, zm[kMaxBands] = {}, sm[kMaxBands] = {};
};

class Sub {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        poly.init(sr);
        mono.init(sr, 0.5f);
        sub.init(0.5f, sr); dry.init(1.f, sr); tone.init(0.5f, sr); range.init(0.5f, sr); grit.init(0.f, sr);
        rmsA.setCutoff(4.f, sr); rmsB.setCutoff(4.f, sr);
    }
    void configure(int instrument) {
        inst = instrument;
        poly.configure(inst);
        hpOut.highpass(inst == kBass ? 18.f : 28.f, 0.7071f, sr);
        reset();
    }
    void setTargets(const Params& p) {
        type = p.subType;
        sub.target = p.sub; dry.target = p.dry; tone.target = p.subTone; range.target = p.range; grit.target = p.grit;
    }
    void snap() { sub.snap(); dry.snap(); tone.snap(); range.snap(); grit.snap(); first = true; control(); }
    void reset() {
        poly.reset(); mono.reset();
        lp[0].reset(); lp[1].reset(); hpOut.reset(); monoIn[0].reset(); monoIn[1].reset(); rmsA.reset(); rmsB.reset(); gritG = 1.f;
        monoFade = type == 1 ? 1.f : 0.f;
    }
    void control() {
        sub.control(); dry.control(); tone.control(); range.control(); grit.control();
        if (first || tone.moving() || range.moving()) {
            const bool bass = inst == kBass;
            float rHz = bass ? expMap(range.cur, 80.f, 600.f) : expMap(range.cur, 150.f, 1200.f);
            poly.set(kQ, rHz);
            float tHz = expMap(tone.cur, 80.f, 1600.f);
            Biquad t; t.lowpass(tHz, 0.7071f, sr);
            lp[0].glideTo(t, first); lp[1].glideTo(t, first);
            t.lowpass(rHz, 0.7071f, sr);
            monoIn[0].glideTo(t, first); monoIn[1].glideTo(t, first);
        }
        first = false;
        monoStep = (type == 1 ? 1.f : -1.f) * (float)kCtl / (0.03f * sr);
    }
    float process(float x, float& dryOut) {
        dryOut = x;
        // Poly and mono crossfade (30 ms) when the type switches; only the
        // live one(s) run.
        monoFade = clampf(monoFade + monoStep / kCtl, 0.f, 1.f);
        float s = 0.f;
        if (monoFade < 1.f) s += (1.f - monoFade) * poly.process(x);
        if (monoFade > 0.f) s += monoFade * mono.process(monoIn[1].process(monoIn[0].process(x)));
        // grit: a soft clip, level-matched so it changes the tone, not the level
        float gr = grit.next();
        if (gr > 1e-4f) {
            float drive = 1.f + 60.f * gr * gr;
            float c = fastTanh(drive * 8.f * s) * 0.125f;
            float a = rmsA.lp(s * s), b = rmsB.lp(c * c);
            if (a > 1e-12f) gritG += 0.002f * (clampf(std::sqrt(a / (b + 1e-14f)), 0.1f, 50.f) - gritG);
            s = lerp(s, c * gritG, clampf(gr * 4.f, 0.f, 1.f));
        }
        s = hpOut.process(lp[1].process(lp[0].process(s)));
        float sg = sub.next(); sg = 2.f * sg * sg;      // 0..2 (+6 dB), squared taper
        return dry.next() * x + sg * s;
    }
private:
    static constexpr float kQ = 8.5f;   // poly band Q: see SubPoly (Q 6 loses notes of close chords)
    float sr = 48000.f, monoFade = 0.f, monoStep = 0.f, gritG = 1.f;
    int inst = kGuitar, type = 0;
    bool first = true;
    SubPoly poly;
    Shifter mono;
    Biquad lp[2], hpOut, monoIn[2];
    OnePole rmsA, rmsB;
    Ramp sub, dry, tone, range, grit;
};

// ---------------------------------------------------------------------------
// Learn + Match (adapted from RASP's Learner/MatchEQ). Learn measures the
// long-term spectrum of what you play in 8 bands, relative to the level of
// the 250 Hz-1 kHz bands (so it doesn't care how hard you hit or how hot the
// DI is). Learn "Fresh" on new strings stores a reference; Learn "Now" on
// the dull strings computes the EQ that pulls them toward it (the
// difference, refined against the real peaking-filter cascade). It skips
// silence and only counts while you play.
// ---------------------------------------------------------------------------
class Learner {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        for (int i = 0; i < kMatchBands; i++) band[i].set(kMatchHz[i], 2.9f, sr);   // ~1/2 octave wide
        msC = coefMs(50.f, sr); fall = 10.f / sr;
        need = (int)(12.f * sr);
    }
    void setSeconds(float s) { need = (int)(s * sr); }
    void start() { active = true; got = 0; ms = 0.f; runMax = -120.f; for (auto& e : energy) e = 0.0; for (auto& b : band) b.reset(); }
    void cancel() { active = false; }
    bool learning() const { return active; }
    float progress() const { return active ? (float)got / (float)need : 0.f; }
    bool feed(float x, float* profile) {
        if (!active) return false;
        ms = x * x + msC * (ms - x * x);
        float lvl = fastPowDb10(ms);
        runMax = std::fmax(lvl, runMax - fall);
        bool playing = lvl > -60.f && lvl > runMax - 25.f;
        for (int i = 0; i < kMatchBands; i++) {
            band[i].process(x);
            if (playing) { float b = band[i].bp * band[i].k; energy[i] += (double)b * b; }
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
    SVF band[kMatchBands];
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

// fresh, now: Learner profiles. Low bands are capped at +-3 dB (they depend
// more on what you played than on the strings), the highs at +-12 dB.
inline void matchGains(const float* fresh, const float* now, float* gainsOut) {
    float want[kMatchBands], g[kMatchBands];
    for (int i = 0; i < kMatchBands; i++) {
        float lim = kMatchHz[i] < 1200.f ? 3.f : 12.f;
        want[i] = clampf(fresh[i] - now[i], -lim, lim);
        g[i] = want[i];
    }
    const float fs = 48000.f;
    for (int it = 0; it < 8; it++) {
        Biquad bq[kMatchBands];
        for (int j = 0; j < kMatchBands; j++) bq[j].peaking(kMatchHz[j], kMatchQ, g[j], fs);
        for (int i = 0; i < kMatchBands; i++) {
            float w = 2.f * kPi * kMatchHz[i] / fs, resp = 0.f;
            for (int j = 0; j < kMatchBands; j++) resp += biquadDb(bq[j], w);
            g[i] = clampf(g[i] + 0.6f * (want[i] - resp), -12.f, 12.f);
        }
    }
    for (int i = 0; i < kMatchBands; i++) gainsOut[i] = g[i];
}

// ---------------------------------------------------------------------------
// NEW STRINGS: what fresh strings have that dull ones don't is upper
// harmonics, a sharper pick transient, and a longer-lasting top. EQ can't
// add harmonics that are no longer there (it just lifts noise), so:
//   - Bright: a level-independent exciter. The band above ~1.5 kHz (bass
//     ~900 Hz) is normalised by its own envelope, gently saturated, and
//     only the new harmonics (3-8 kHz, bass 1.8-5 kHz: the "zing", and what an
//     amp sim keeps) are added back, scaled by the envelope again.
//   - Clarity: a presence bell (3 kHz, bass 1.8 kHz) that opens on pick attacks.
//   - Air: a high shelf (6 kHz, bass 4 kHz).
//   - Match: the learned EQ, times Amount.
// ---------------------------------------------------------------------------
class Strings {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        bright.init(0.5f, sr); clarity.init(0.5f, sr); air.init(2.f, sr); amount.init(0.f, sr);
        for (auto& m : match) m.init(0.f, sr);
        fast.set(0.5f, 30.f, sr); slow.set(20.f, 250.f, sr);
        env.set(1.f, 40.f, sr);
    }
    void configure(int instrument) {
        inst = instrument;
        const bool bass = inst == kBass;
        exHz = bass ? 900.f : 1500.f;
        exHp[0].highpass(exHz, 0.7071f, sr); exHp[1] = exHp[0];
        harmHp[0].highpass(exHz * 2.f, 0.7071f, sr); harmHp[1] = harmHp[0];
        harmLp[0].lowpass(bass ? 5000.f : 8000.f, 0.7071f, sr); harmLp[1] = harmLp[0];
        pres.set(bass ? 1800.f : 3000.f, 0.9f, sr);
        reset();
    }
    void setTargets(const Params& p) {
        bright.target = p.bright; clarity.target = p.clarity; air.target = p.airDb; amount.target = p.matchAmount;
        for (int i = 0; i < kMatchBands; i++) match[i].target = p.match[i];
    }
    void snap() { bright.snap(); clarity.snap(); air.snap(); amount.snap(); for (auto& m : match) m.snap(); first = true; control(); }
    void reset() {
        for (auto& b : eq) b.reset();
        exHp[0].reset(); exHp[1].reset(); harmHp[0].reset(); harmHp[1].reset(); harmLp[0].reset(); harmLp[1].reset(); shelf.reset(); pres.reset();
        fast.reset(); slow.reset(); env.reset();
    }
    void control() {
        bright.control(); clarity.control(); air.control(); amount.control();
        bool mm = first || amount.moving();
        for (auto& m : match) { m.control(); mm = mm || m.moving(); }
        if (mm) {
            eqOn = amount.cur > 1e-4f || amount.prev > 1e-4f;
            Biquad t;
            for (int i = 0; i < kMatchBands; i++) { t.peaking(kMatchHz[i], kMatchQ, clampf(match[i].cur, -12.f, 12.f) * amount.cur, sr); eq[i].glideTo(t, first); }
        }
        if (first || air.moving()) { Biquad t; t.highShelf(inst == kBass ? 4000.f : 6000.f, air.cur, sr); shelf.glideTo(t, first); }
        first = false;
    }
    float process(float x, float& dryOut) {
        dryOut = x;
        float y = x;
        if (eqOn) for (auto& b : eq) y = b.process(y);
        // exciter
        float hb = exHp[1].process(exHp[0].process(y));
        float e = fastDbToLin(env.process(hb)) + 1e-6f;
        float u = clampf(hb / e, -1.2f, 1.2f);              // ~unit level whatever the DI level
        // Chebyshev polynomials: for a unit sine, T2 and T3 are exactly its
        // 2nd and 3rd harmonics, with no copy of the input (a saturator's
        // linear part would partly cancel the original after the filters)
        float n = 0.6f * (2.f * u * u - 1.f) + 0.4f * u * (4.f * u * u - 3.f);
        float h = harmLp[1].process(harmLp[0].process(harmHp[1].process(harmHp[0].process(n)))) * e;
        float br = bright.next();
        y += 1.5f * br * h;
        // transient presence: how far the fast envelope is above the slow one
        float f = fast.process(y), s = slow.process(y);
        float tr = clampf((f - s) / 9.f, 0.f, 1.f);
        pres.process(y);
        float boost = clarity.next() * tr * 1.0f;           // up to +6 dB at the bell on attacks
        y += boost * pres.bp * pres.k;
        return shelf.process(y);
    }
private:
    float sr = 48000.f, exHz = 2500.f;
    int inst = kGuitar;
    bool first = true, eqOn = false;
    Biquad eq[kMatchBands], exHp[2], harmHp[2], harmLp[2], shelf;
    SVF pres;
    Follower fast, slow, env;
    Ramp bright, clarity, air, amount, match[kMatchBands];
};

// ---------------------------------------------------------------------------
// BUZZSAW: an HM-2-style heavy drive, our own circuit. What makes that sound
// is the EQ around two hard clipping stages: a lift into the clippers, then
// big resonant boosts near 100 Hz and 1 kHz with the 300-500 Hz region
// scooped between them - "the chainsaw". Bass voicing moves the lifts to
// ~70 Hz / 800 Hz and keeps a clean blend for the fundamental.
//   pre: high-pass, +6 dB bell at ~900 Hz (into the clippers)
//   clip 1: asymmetric soft clip  -> interstage lowpass
//   clip 2: harder symmetric clip (both 4x oversampled, ADAA)
//   post: lows bell, highs bell, scoop, 7 kHz lowpass; auto level, then Level
// ---------------------------------------------------------------------------
class Buzzsaw {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        gain.init(0.65f, sr); lows.init(0.7f, sr); highs.init(0.7f, sr); scoop.init(0.5f, sr); level.init(0.f, sr); blend.init(0.f, sr);
        rmsIn.setCutoff(10.f, sr); rmsOut.setCutoff(10.f, sr);
    }
    void configure(int instrument) {
        inst = instrument;
        const bool bass = inst == kBass;
        hpIn.highpass(bass ? 35.f : 70.f, 0.7071f, sr);
        pre.peaking(bass ? 700.f : 900.f, 0.8f, 6.f, sr);
        inter.setCutoff(bass ? 4000.f : 5000.f, sr * 4.f);
        dcBlock.setCutoff(15.f, sr * 4.f);
        lp[0].lowpass(bass ? 5000.f : 7000.f, 0.7071f, sr); lp[1] = lp[0];
        hpOut.highpass(bass ? 30.f : 45.f, 0.7071f, sr);
        reset();
    }
    void setTargets(const Params& p) {
        gain.target = p.gain; lows.target = p.lows; highs.target = p.highs; scoop.target = p.scoop; level.target = p.level; blend.target = p.blend;
    }
    void snap() { gain.snap(); lows.snap(); highs.snap(); scoop.snap(); level.snap(); blend.snap(); first = true; control(); }
    void reset() {
        up.init(); down.init(); downDry.init();
        hpIn.reset(); pre.reset(); inter.reset(); dcBlock.reset(); lp[0].reset(); lp[1].reset(); hpOut.reset();
        bLow.reset(); bHigh.reset(); bScoop.reset(); rmsIn.reset(); rmsOut.reset();
        p1 = kBias; p1F = logCosh(kBias); p2 = p2F = 0.f;
        autoG = dbToLin(-24.f - 6.f * lows.target - 7.f * highs.target);   // a first guess; it adapts while you play
    }
    void control() {
        gain.control(); lows.control(); highs.control(); scoop.control(); level.control(); blend.control();
        if (first || lows.moving() || highs.moving() || scoop.moving()) {
            const bool bass = inst == kBass;
            Biquad t;
            t.peaking(bass ? 70.f : 100.f, 0.9f, 15.f * lows.cur, sr); bLow.glideTo(t, first);
            t.peaking(bass ? 800.f : 1050.f, 1.1f, 15.f * highs.cur, sr); bHigh.glideTo(t, first);
            t.peaking(bass ? 300.f : 420.f, 0.7f, -12.f * scoop.cur, sr); bScoop.glideTo(t, first);
        }
        first = false;
    }
    float process(float x, float& dryOut) {
        float d = gain.next();
        float g = fastDbToLin(6.f + 42.f * d);
        float pre_ = pre.process(hpIn.process(x));
        float yc = 0.f, yd = 0.f;
        for (int k = 0; k < 4; k++) {
            float u = up.process(k == 0 ? pre_ * 4.f : 0.f);
            // clip 1: asymmetric (a bias, then the DC is removed)
            float c1 = clip(g * u + kBias, p1, p1F) - kBiasOut;
            c1 = dcBlock.hp(c1);
            c1 = inter.lp(c1);
            // clip 2: harder, symmetric
            float c2 = clip(4.f * c1, p2, p2F);
            yc = down.process(c2);
        }
        // the clean blend takes the same oversampling filters, so it can't cancel
        for (int k = 0; k < 4; k++) yd = downDry.process(k == 0 ? x * 4.f : 0.f);
        dryOut = yd;
        float y = bScoop.process(bHigh.process(bLow.process(yc)));
        y = hpOut.process(lp[1].process(lp[0].process(y)));
        // auto level: the drive comes out about as loud as the DI went in
        // (it only adapts while you play: in the gaps it would creep up and
        // blast the next note)
        float ein = rmsIn.lp(x * x), eout = rmsOut.lp(y * y);
        if (ein > 1e-5f) {
            float want = clampf(std::sqrt(ein / (eout + 1e-12f)), 0.01f, 2.f);
            autoG += (want < 0.5f * autoG ? 0.01f : 0.0005f) * (want - autoG);   // too loud: catch up fast
        }
        y *= autoG * fastDbToLin(level.next());
        float bl = blend.next();
        return safetyClip(y * (1.f - 0.5f * bl) + bl * yd);
    }
private:
    static constexpr float kBias = 0.15f, kBiasOut = 0.14888503f;   // tanh(0.15)
    static float logCosh(float x) {
        float a = std::fabs(x);
        if (a > 5.f) return a - 0.69314718f;
        return a + std::log1p(std::exp(-2.f * a)) - 0.69314718f;
    }
    // tanh with first-order antiderivative anti-aliasing (as in RASP's Drive)
    static float clip(float c, float& prevC, float& prevF) {
        float dx = c - prevC, F = logCosh(c), y;
        if (std::fabs(dx) > 1e-4f) y = (F - prevF) / dx;
        else y = std::tanh(0.5f * (c + prevC));
        prevC = c; prevF = F;
        return y;
    }
    float sr = 48000.f, autoG = 1.f;
    int inst = kGuitar;
    bool first = true;
    float p1 = 0, p1F = 0, p2 = 0, p2F = 0;
    OSFilter up, down, downDry;
    Biquad hpIn, pre, lp[2], hpOut, bLow, bHigh, bScoop;
    OnePole inter, dcBlock, rmsIn, rmsOut;
    Ramp gain, lows, highs, scoop, level, blend;
};

// ---------------------------------------------------------------------------
// Voice: one complete mode chain at one instrument setting. The Processor
// keeps two, and crossfades when the mode or instrument changes.
// To add a mode: write its class (init/configure/setTargets/snap/reset/
// control/process), add it to Mode and the switches below, add its params.
// ---------------------------------------------------------------------------
class Voice {
public:
    void init(float sr) {
        tight.init(sr); sub.init(sr); strings.init(sr); buzz.init(sr);
        mix.init(1.f, sr);
    }
    void configure(int m, int instrument, const Params& p) {
        mode = m; inst = instrument;
        switch (mode) {
            case kTight: tight.configure(inst); tight.setTargets(p); tight.snap(); break;
            case kSub: sub.setTargets(p); sub.configure(inst); sub.snap(); break;
            case kStrings: strings.configure(inst); strings.setTargets(p); strings.snap(); break;
            default: buzz.configure(inst); buzz.setTargets(p); buzz.snap(); break;
        }
        mix.snap(p.mix);
    }
    void setTargets(const Params& p) {
        switch (mode) {
            case kTight: tight.setTargets(p); break;
            case kSub: sub.setTargets(p); break;
            case kStrings: strings.setTargets(p); break;
            default: buzz.setTargets(p); break;
        }
        mix.target = p.mix;
    }
    void snap() {
        switch (mode) {
            case kTight: tight.snap(); break;
            case kSub: sub.snap(); break;
            case kStrings: strings.snap(); break;
            default: buzz.snap(); break;
        }
        mix.snap();
    }
    void reset() {
        switch (mode) {
            case kTight: tight.reset(); break;
            case kSub: sub.reset(); break;
            case kStrings: strings.reset(); break;
            default: buzz.reset(); break;
        }
    }
    void control() {
        switch (mode) {
            case kTight: tight.control(); break;
            case kSub: sub.control(); break;
            case kStrings: strings.control(); break;
            default: buzz.control(); break;
        }
        mix.control();
    }
    float process(float x) {
        float dry = x, wet;
        switch (mode) {
            case kTight: wet = tight.process(x, dry); break;
            case kSub: wet = sub.process(x, dry); break;
            case kStrings: wet = strings.process(x, dry); break;
            default: wet = buzz.process(x, dry); break;
        }
        float m = mix.next();
        return dry + m * (wet - dry);
    }
    int mode = -1, inst = -1;
    Tight tight;
    Sub sub;
    Strings strings;
    Buzzsaw buzz;
    Ramp mix;
};

// ---------------------------------------------------------------------------
// Processor: the whole plugin, mono in / mono out.
// ---------------------------------------------------------------------------
class Processor {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        for (auto& v : voices) v.init(sr);
        inG.init(1.f, sr); outG.init(1.f, sr);
        learner.init(sr);
        gateKeyC = coefMs(5.f, sr); gateAtt = coefMs(1.f, sr); gateRel = coefMs(120.f, sr);
        holdLen = (int)(0.05f * sr);
        fadeLen = (int)(0.04f * sr);        // mode / instrument crossfade, 40 ms
        bypLen = (int)(0.02f * sr);         // bypass crossfade, 20 ms
        params = Params{};
        voices[0].configure(params.mode, params.instrument, params);
        voices[1].configure(params.mode, params.instrument, params);
        active = 0; fade = 0;
        haveParams = false;
        reset();
    }
    void setParams(const Params& p) {
        params = p;
        params.mode = p.mode < 0 ? 0 : (p.mode >= kNumModes ? kNumModes - 1 : p.mode);
        params.instrument = p.instrument == kBass ? kBass : kGuitar;
        inG.target = fastDbToLin(clampf(p.inDb, -24.f, 24.f));
        outG.target = fastDbToLin(clampf(p.outDb, -60.f, 24.f));
        gateOn = p.gate > 1e-3f; gateThr = -75.f + 45.f * clampf(p.gate, 0.f, 1.f);
        voices[active].setTargets(params);
        if (fade > 0) voices[1 - active].setTargets(params);
        if (!haveParams) {   // the first parameters after init are not a "change"
            haveParams = true;
            applyNow();
        }
    }
    // kx:Reset / transport reset: clear all state and jump every smoothed
    // value to its target (no glides, no fades).
    void reset() {
        applyNow();
        inG.snap(); outG.snap();
        bypassMix = params.bypass ? 0.f : 1.f;
        gateMs = 0.f; gateGr = 0.f; hold = 0;
        ctl = 0;
    }
    void process(const float* in, float* out, int n) {
        for (int i = 0; i < n; i++) {
            if (ctl == 0) controlTick();
            ctl = (ctl + 1) % kCtl;
            float x = in[i];
            float xi = x * inG.next();
            if (learner.learning() && learner.feed(xi, learnResult)) onLearnDone();
            float y;
            if (fade > 0) {
                float t = (float)fade / (float)fadeLen;      // 1 -> 0: old voice fading out
                y = t * voices[1 - active].process(xi) + (1.f - t) * voices[active].process(xi);
                if (--fade == 0) voices[1 - active].mode = -1;
            } else {
                y = voices[active].process(xi);
            }
            y *= gate(xi) * outG.next();
            // bypass: linear crossfade with the untouched input
            float bt = params.bypass ? 0.f : 1.f;
            if (std::fabs(bypassMix - bt) > 0.f) bypassMix = clampf(bypassMix + (bt > bypassMix ? 1.f : -1.f) / (float)bypLen, 0.f, 1.f);
            out[i] = x + bypassMix * (y - x);
        }
        // Never pass NaN/inf on (a broken host buffer, an unstable filter): recover silently.
        for (int i = 0; i < n; i++) if (!std::isfinite(out[i])) { reset(); for (int j = 0; j < n; j++) out[j] = 0.f; break; }
    }

    // --- Learn (New Strings) ---------------------------------------------
    void learnFresh() { learnTarget = 1; learner.start(); }
    void learnNow() { learnTarget = 2; learner.start(); }
    void cancelLearn() { learner.cancel(); learnTarget = 0; }
    void setLearnSeconds(float s) { learner.setSeconds(s); }
    bool learning() const { return learner.learning(); }
    int learningWhat() const { return learner.learning() ? learnTarget : 0; }   // 1 fresh, 2 now
    float learnProgress() const { return learner.progress(); }
    bool hasFresh() const { return haveFresh; }
    const float* freshProfile() const { return fresh; }
    void setFreshProfile(const float* f) { for (int i = 0; i < kMatchBands; i++) fresh[i] = f[i]; haveFresh = true; }
    // Returns true once after Learn Now finished; gains in dB for Params::match.
    bool learnedMatch(float* gains) {
        if (!matchReady) return false;
        matchReady = false;
        for (int i = 0; i < kMatchBands; i++) gains[i] = matchOut[i];
        return true;
    }
    float gainReductionDb(int band) const { return voices[active].mode == kTight ? voices[active].tight.gainReductionDb(band) : 0.f; }
    float gateReductionDb() const { return -gateGr; }
    int latencySamples() const { return 0; }

private:
    void applyNow() {
        voices[active].configure(params.mode, params.instrument, params);
        voices[active].reset();
        voices[active].snap();
        voices[1 - active].mode = -1;
        fade = 0;
    }
    void controlTick() {
        inG.control(); outG.control();
        Voice& v = voices[active];
        if (fade == 0 && (v.mode != params.mode || v.inst != params.instrument)) {
            // start a crossfade to a freshly configured voice
            active = 1 - active;
            voices[active].configure(params.mode, params.instrument, params);
            voices[active].reset();
            fade = fadeLen;
        }
        voices[active].control();
        if (fade > 0) voices[1 - active].control();
    }
    float gate(float key) {
        if (!gateOn) { gateGr = 0.f; return 1.f; }
        gateMs = key * key + gateKeyC * (gateMs - key * key);
        float lvl = fastPowDb10(gateMs);
        float target = 0.f;
        if (lvl >= gateThr) hold = holdLen;
        else if (hold > 0) hold--;
        else target = clampf((lvl - gateThr) * 3.f, -60.f, 0.f);    // 1:4 below threshold
        gateGr = target + (target > gateGr ? gateAtt : gateRel) * (gateGr - target);
        return fastDbToLin(gateGr);
    }
    void onLearnDone() {
        if (learnTarget == 1) {
            for (int i = 0; i < kMatchBands; i++) fresh[i] = learnResult[i];
            haveFresh = true;
        } else if (learnTarget == 2 && haveFresh) {
            matchGains(fresh, learnResult, matchOut);
            matchReady = true;
        }
        learnTarget = 0;
    }

    float sr = 48000.f;
    Params params;
    Voice voices[2];
    int active = 0, fade = 0, fadeLen = 1920, bypLen = 960, ctl = 0;
    bool haveParams = false;
    Ramp inG, outG;
    float bypassMix = 1.f;
    // gate
    bool gateOn = false;
    float gateThr = -60.f, gateMs = 0.f, gateGr = 0.f, gateKeyC = 0.f, gateAtt = 0.f, gateRel = 0.f;
    int hold = 0, holdLen = 2400;
    // learn
    Learner learner;
    int learnTarget = 0;
    float learnResult[kMatchBands] = {}, fresh[kMatchBands] = {}, matchOut[kMatchBands] = {};
    bool haveFresh = false, matchReady = false;
};

}  // namespace axe
