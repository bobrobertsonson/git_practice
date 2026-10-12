// RASP - extreme vocal processor DSP core.
//
// A channel strip for a vocal that is ALREADY screamed: it doesn't synthesize
// a scream, it makes a real one sound like a produced record. The layout follows
// screamer-style plugins (Ablaze Screamer), as publicly described, and fills
// each section with what a mix engineer would put on a big metal vocal:
//
//              ┌──────────────── dry (control out) ─────────────────┐
//   in ─► CONTROL ─┼─► THICKEN  doubles + stereo combs (parallel) ──────┼─► BUS ─► out L/R
//              └─► FLAVOUR  plate reverb + delay, ducked (parallel) ─┘
//
//   CONTROL  lo/hi cut ─► drive (Tube-Screamer-style, 4x OS + ADAA, mix)
//            ─► voicing EQ (tone switch) ─► de-esser
//            ─► chop: FET-style peak comp ─► opto-style leveler, + parallel crush
//            ─► bleed reducer (expander keyed on the clean mic, for live use)
//   THICKEN  two micro-pitched doubles (±cents, Haas-delayed) + comb taps,
//            diffuse, darken, width, solo
//   FLAVOUR  8-line FDN plate (modulated), ping-pong delay, return EQ, tilt, duck
//   BUS      stereo glue compressor ─► -1 dBFS peak limiter
//   LIVE     mono mode, level-matched digital bypass, feedback notches
//            (HowlGuard) on the final output
//
// Header-only C++17, no heap, no dependencies. No lookahead and no block
// processing: the only delay on the dry path is filter phase delay in the
// drive (oversampling + tone filters, ~0.15 ms at 3 kHz).
#pragma once

// Delay buffers are sized for sample rates up to 96 kHz (the Daisy runs at
// 48k). Desktop builds that must also run at 176.4/192 kHz, like the plugin,
// define RASP_SR_SCALE 2, which doubles them (~3 MB instead of ~1.5 MB).
#ifndef RASP_SR_SCALE
#define RASP_SR_SCALE 1
#endif

#include <cmath>
#include <cstdint>
#include <utility>

namespace rasp {

constexpr float kPi = 3.14159265358979f;

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

// Fast log2 / exp2 for per-sample gain math (a polynomial on the mantissa).
// log2 error < 3.2e-5 (0.0002 dB); exp2 relative error < 2.4e-7. These
// replace ~10 pow/log10 calls per sample in the dynamics stages.
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
inline float fastDbToLin(float db) { return fastExp2(db * 0.166096404f); }          // log2(10)/20
inline float fastLinToDb(float x) { return 6.02059991f * fastLog2(x + 1e-9f); }     // 20*log10(2)
inline float fastPowDb10(float ms) { return 3.01029996f * fastLog2(ms + 1e-12f); }  // 10*log10(ms)

// Linear up to 0.9, then rounds off to 1.0. Last-resort protection only.
inline float safetyClip(float x) {
    float a = std::fabs(x);
    if (a <= 0.9f) return x;
    float y = 0.9f + 0.1f * fastTanh((a - 0.9f) * 10.f);
    return x < 0.f ? -y : y;
}

struct OnePole {
    float a = 0.f, z = 0.f;
    void setCutoff(float hz, float sr) { a = std::exp(-2.f * kPi * hz / sr); }
    float lp(float x) { z = x + a * (z - x); return z; }
    float hp(float x) { return x - lp(x); }
};

// Per-sample control smoother (~20 ms). State is double: a float smoother
// stalls short of large targets (a 24000-sample delay time stopped 3.75
// samples early). The STM32H7 has a double-precision FPU, so this is cheap.
struct Smoothed {
    double a = 0.0, cur = 0.0;
    float target = 0.f;
    void init(float v, float sr, float ms = 20.f) { a = std::exp(-1.0 / (ms * 0.001 * sr)); cur = target = v; }
    float next() { cur = target + a * (cur - target); return (float)cur; }
};

// TPT state-variable filter (Zavalishin) - stable under modulation.
struct SVF {
    float g = 0, k = 1, ic1 = 0, ic2 = 0, lp = 0, bp = 0, hp = 0;
    void set(float hz, float q, float sr) {
        g = std::tan(kPi * clampf(hz, 10.f, sr * 0.45f) / sr);
        k = 1.f / q;
    }
    void process(float x) {
        float v3 = x - ic2;
        float v1 = (g * v3 + ic1) / (1.f + g * (g + k));
        float v2 = ic2 + g * v1;
        ic1 = 2.f * v1 - ic1; ic2 = 2.f * v2 - ic2;
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
};

// Biquad, transposed direct form II. c = {b0, b1, b2, a1, a2}, a0 = 1.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(const float* c) { b0 = c[0]; b1 = c[1]; b2 = c[2]; a1 = c[3]; a2 = c[4]; }
    float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    // RBJ cookbook designs.
    void norm(float B0, float B1, float B2, float A0, float A1, float A2) {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    void peaking(float hz, float q, float db, float sr) {
        float A = std::pow(10.f, db / 40.f), w = 2.f * kPi * hz / sr, al = std::sin(w) / (2.f * q), c = std::cos(w);
        norm(1.f + al * A, -2.f * c, 1.f - al * A, 1.f + al / A, -2.f * c, 1.f - al / A);
    }
    void lowShelf(float hz, float db, float sr) {
        float A = std::pow(10.f, db / 40.f), w = 2.f * kPi * hz / sr, c = std::cos(w);
        float al = std::sin(w) / 2.f * std::sqrt(2.f), s = 2.f * std::sqrt(A) * al;
        norm(A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
             (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
    }
    void highShelf(float hz, float db, float sr) {
        float A = std::pow(10.f, db / 40.f), w = 2.f * kPi * hz / sr, c = std::cos(w);
        float al = std::sin(w) / 2.f * std::sqrt(2.f), s = 2.f * std::sqrt(A) * al;
        norm(A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
             (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
    }
};

// 10th-order elliptic lowpass for 4x oversampling, designed offline:
// scipy.signal.ellip(10, 0.1, 90, 0.21, output='sos'). Passband to 0.42*fs
// (20.2 kHz @ 48k), >= 90 dB down from 0.58*fs. Independent of the base rate.
constexpr float kOS4[5][5] = {
    {2.1738402454e-04f, 2.7823415894e-04f, 2.1738402454e-04f, -1.5801157295e+00f, 6.3849354156e-01f},
    {1.0000000000e+00f, -5.0407447332e-01f, 1.0000000000e+00f, -1.5629856718e+00f, 7.1984100355e-01f},
    {1.0000000000e+00f, -1.1101093521e+00f, 1.0000000000e+00f, -1.5439919071e+00f, 8.2467542187e-01f},
    {1.0000000000e+00f, -1.3141678657e+00f, 1.0000000000e+00f, -1.5367789871e+00f, 9.1036321211e-01f},
    {1.0000000000e+00f, -1.3826713524e+00f, 1.0000000000e+00f, -1.5494885685e+00f, 9.7285772614e-01f},
};
struct OSFilter {
    Biquad s[5];
    void init() { for (int i = 0; i < 5; i++) s[i].set(kOS4[i]); }
    float process(float x) { for (auto& b : s) x = b.process(x); return x; }
};

template <int N>
struct DelayLine {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    float buf[(size_t)N] = {};
    int w = 0;
    void write(float x) { buf[w] = x; w = (w + 1) & (N - 1); }
    float read(float d) const {  // d >= 1 samples behind the last write
        // Split integer and fraction first: a float position near 2^17 would
        // only keep ~1/128-sample precision.
        int di = (int)d;
        float frac = d - (float)di;
        float a = buf[(w - di) & (N - 1)], b = buf[(w - di - 1) & (N - 1)];
        return a + (b - a) * frac;
    }
    float tap(int d) const { return buf[(w - d) & (N - 1)]; }
};

template <int N>
struct Allpass {
    float buf[(size_t)N] = {};
    int len = N, i = 0;
    void setLen(int n) { len = n < 1 ? 1 : (n > N ? N : n); }
    float process(float x, float g) {
        float d = buf[i];
        float v = x + g * d;
        buf[i] = v;
        if (++i >= len) i = 0;
        return d - g * v;
    }
};

// Gain computer: soft-knee static curve in dB. Returns gain change (<= 0 dB).
inline float kneeCurve(float overDb, float ratio, float knee) {
    if (overDb > knee * 0.5f) return overDb * (1.f / ratio - 1.f);
    if (overDb > -knee * 0.5f) { float t = overDb + knee * 0.5f; return (1.f / ratio - 1.f) * t * t / (2.f * knee); }
    return 0.f;
}

// ---------------------------------------------------------------------------
// Parameters. Knobs are 0..1 unless noted.
// ---------------------------------------------------------------------------
struct Params {
    // CONTROL
    bool  controlOn  = true;
    float loCut      = 0.40f;  // 20..600 Hz   (0.4 = 78 Hz)
    float hiCut      = 0.80f;  // 2k..20k Hz   (0.8 = 12.6 kHz)
    float drive      = 0.40f;  // Tube-Screamer-style gain
    int   brightness = 1;      // 3-way tone: 0 dark, 1 natural, 2 bright (drive tone + voicing EQ)
    float mix        = 0.60f;  // parallel blend of the drive (level-matched)
    float chop       = 0.55f;  // compression amount: peak comp + leveler + parallel crush
    float release    = 0.40f;  // leveler release 60..800 ms
    float deEss      = 0.50f;  // de-esser depth (menu)
    float airDb      = 0.f;    // extra high shelf at 7 kHz, 0..12 dB (menu): the very bright
                               // modern-deathcore top (one producer lifted 6-10 kHz by ~13 dB)
    float bleed      = 0.70f;  // bleed reducer threshold: 0 off, 0.5 = -50, 0.7 = -42, 1 = -30 dBFS RMS (menu)
    float controlGainDb = 0.f;

    // THICKEN (parallel)
    bool  thickenOn  = true;
    float spread     = 0.60f;  // double detune (0..18 cents) + comb spacing
    float diffuse    = 0.35f;  // smear the combs into a dense cloud
    float darken     = 0.30f;  // lowpass on the thicken branch
    float width      = 0.85f;  // stereo width of the branch
    bool  solo       = false;  // hear only the thicken branch
    float thickenGainDb = -4.f;

    // FLAVOUR (parallel)
    bool  flavourOn  = true;
    float tail       = 0.35f;  // plate decay setting, 0.5..6 s (0.35 measures ~0.9 s with damping)
    float tailBlend  = 0.35f;  // reverb level
    float repeats    = 0.30f;  // delay feedback
    float delayBlend = 0.20f;  // delay level
    int   delayTime  = 0;      // 3-way: 0 = 1/8, 1 = dotted 1/8, 2 = 1/4
    float bpm        = 120.f;
    float tilt       = 0.50f;  // EQ on the effect: 0 dark .. 1 bright (+-6 dB)
    float duck       = 0.60f;  // effect dips while the vocal is present
    float flavourGainDb = -5.f;

    // BUS
    float glue       = 0.50f;  // stereo glue compression (menu)
    float outDb      = 0.f;    // drives the bus limiter: louder = denser
    float inDb       = 0.f;    // input gain (the preamp gain knob): set so screams peak near -6 dBFS

    // LIVE
    bool  bypass     = false;  // digital bypass: dry voice, level-matched, click-free crossfade
    bool  bypassMatch = true;  // match the bypassed voice to the effect's loudness
    bool  mono       = false;  // one-channel PA: drop the stereo comb taps, sum to mono
    bool  howl       = true;   // automatic feedback notches

    // MOMENTARY (held footswitch / button; on while held)
    bool  mega       = false;  // the voice becomes a mid-focused hard-clipped burst
    bool  throwOn    = false;  // the voice is sent to a long ping-pong delay; repeats ring out after release
};

// ---------------------------------------------------------------------------
// Drive: Tube-Screamer-style overdrive, 4x oversampled, level-matched.
//
// The TS808 op-amp stage only amplifies above ~720 Hz (4.7k + 0.047 uF to
// ground), clips that with a diode pair in the feedback loop, and adds the
// result back onto the clean input. A 51 pF feedback cap rolls the clipped
// top off as the drive goes up. That's why a TS tightens the low end and puts
// grit in the mids instead of fizz - exactly what a scream wants.
// ---------------------------------------------------------------------------
class Drive {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        up.init(); down.init();
        hp720.setCutoff(720.f, sr * 4.f);                         // set per tone in set()
        rmsIn.setCutoff(3.f, sr); rmsOut.setCutoff(3.f, sr);
        level.init(1.f, sr, 50.f);
    }
    void set(float drive, int brightness) {
        float d2 = drive * drive;
        gain = 1.f + 120.f * d2;                                   // TS: 1 + (51k + 500k*pot)/4.7k
        float rf = 51e3f + 500e3f * d2;
        capLp.setCutoff(clampf(1.f / (2.f * kPi * rf * 51e-12f), 1000.f, sr * 1.8f), sr * 4.f);
        int b = brightness < 0 ? 0 : (brightness > 2 ? 2 : brightness);
        static const float toneHz[3] = {4500.f, 7500.f, 11000.f};
        tone.set(toneHz[b], 0.6f, sr);
        // Where the drive starts amplifying, and its tone filter. The TS808's
        // 720 Hz corner and dark tone keep a guitar tight, but on real guttural
        // stems they thinned 160-400 Hz by 6-10 dB and dulled 2.5 kHz by up to
        // 11 dB against a producer's mix of the same takes (tools/mixprint.py),
        // so the corners sit lower and the tone filter higher.
        static const float cornerHz[3] = {200.f, 350.f, 600.f};
        hp720.setCutoff(cornerHz[b], sr * 4.f);
    }
    float process(float x) {
        float y = 0.f;
        for (int k = 0; k < 4; k++) {
            float u = up.process(k == 0 ? x * 4.f : 0.f);
            float c = capLp.lp(gain * hp720.hp(u)) * (1.f / kClip);
            float d = kClip * clipADAA(c);                         // diode pair
            y = down.process(u + d);
        }
        tone.process(y);
        float wet = tone.lp;
        // Auto level: keep the wet as loud as the dry so drive and mix
        // change texture, not volume.
        float ein = rmsIn.lp(x * x), eout = rmsOut.lp(wet * wet);
        if (ein > 1e-7f) level.target = clampf(std::sqrt(ein / (eout + 1e-12f)), 0.25f, 2.f);
        return wet * level.next();
    }
private:
    // log(cosh(x)), the antiderivative of tanh, without overflow.
    static float logCosh(float x) {
        float a = std::fabs(x);
        if (a > 5.f) return a - 0.69314718f;                       // correction < 5e-5
        return a + std::log1p(std::exp(-2.f * a)) - 0.69314718f;
    }
    // First-order antiderivative anti-aliasing (Parker et al., 2016) of tanh.
    // At 4x oversampling a hard-driven clipper still aliases (its harmonics
    // run past 96 kHz); ADAA removes most of that for a 1/8-sample delay.
    float clipADAA(float c) {
        float dx = c - prevC, F = logCosh(c), y;
        if (std::fabs(dx) > 1e-4f) y = (F - prevF) / dx;
        else y = std::tanh(0.5f * (c + prevC));
        prevC = c; prevF = F;
        return y;
    }
    static constexpr float kClip = 0.3f;
    float prevC = 0.f, prevF = 0.f;
    float sr = 48000.f, gain = 1.f;
    OSFilter up, down;
    OnePole hp720, capLp, rmsIn, rmsOut;
    SVF tone;
    Smoothed level;
};

// ---------------------------------------------------------------------------
// Voicing EQ: the "mixed" curve for an extreme vocal, per tone position.
// Chest weight, cut the boxy low mids, push the bite, open the air.
// ---------------------------------------------------------------------------
class Voicing {
public:
    void set(int brightness, float airDb, float sr) {
        extra.highShelf(7000.f, clampf(airDb, 0.f, 12.f), sr);
        struct V { float lowHz, lowDb, mudHz, mudDb, presHz, presDb, airHz, airDb; };
        static const V v[3] = {
            {170.f, 2.5f, 420.f, -2.0f, 2600.f, 1.5f, 10000.f, 0.5f},   // dark: weight, gutturals
            {150.f, 1.5f, 450.f, -3.0f, 3000.f, 3.0f, 10000.f, 1.0f},   // natural
            {120.f, 0.5f, 500.f, -3.5f, 3500.f, 4.0f, 11000.f, 4.0f},   // bright: highs, squeals
        };
        const V& c = v[brightness < 0 ? 0 : (brightness > 2 ? 2 : brightness)];
        low.lowShelf(c.lowHz, c.lowDb, sr);
        mud.peaking(c.mudHz, 1.2f, c.mudDb, sr);
        pres.peaking(c.presHz, 0.9f, c.presDb, sr);
        air.highShelf(c.airHz, c.airDb, sr);
    }
    // The de-esser runs after this, so extra air also gets its sibilance held back.
    float process(float x) { return extra.process(air.process(pres.process(mud.process(low.process(x))))); }
private:
    Biquad low, mud, pres, air, extra;
};

// ---------------------------------------------------------------------------
// De-esser: a dynamic bell at 7 kHz. When the sibilance band carries more
// than ~45% of the voice's level, the bell dips by up to 8 dB. A bell built
// from the SVF band-pass (x - (1-g)*k*bp) cuts by exactly g at its centre,
// unlike subtracting a phase-shifted high-pass.
// ---------------------------------------------------------------------------
class DeEsser {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        band.set(7000.f, 1.0f, sr);                    // k = 1/Q = 1
        envAtt = coefMs(0.5f, sr); envRel = coefMs(60.f, sr);
    }
    void set(float amount) { depth = amount; }
    float process(float x) {
        if (depth < 0.001f) return x;
        band.process(x);
        float b = band.bp * band.k;                    // unity gain at 7 kHz
        auto env = [&](float& e, float v) { float a = std::fabs(v); e = a + (a > e ? envAtt : envRel) * (e - a); };
        env(eHi, b); env(eAll, x);
        float over = clampf((eHi / (eAll + 1e-6f) - 0.45f) / 0.35f, 0.f, 1.f);
        float g = fastDbToLin(-8.f * depth * over);
        return x - (1.f - g) * b;
    }
private:
    float sr = 48000.f, depth = 0.5f, eHi = 0, eAll = 0, envAtt = 0, envRel = 0;
    SVF band;
};

// ---------------------------------------------------------------------------
// Bleed reducer: downward expander for live use. The compressor and the
// parallel crush lift quiet sound by up to ~24 dB, including drum and wedge
// bleed. This turns sound below the threshold down (1:4, at most -40 dB, so
// it can undo that lift), keyed on the clean mic signal before the drive.
// It's a slope rather than a gate's on/off, so phrase tails fade naturally.
// ---------------------------------------------------------------------------
class Expander {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        rmsC = coefMs(5.f, sr); att = coefMs(2.f, sr); rel = coefMs(150.f, sr);
    }
    void set(float amount) { on = amount > 0.001f; thr = -70.f + 40.f * amount; }
    float gain(float key) {
        if (!on) return 1.f;
        ms = key * key + rmsC * (ms - key * key);
        float lvl = fastPowDb10(ms);
        float target = lvl < thr ? clampf((lvl - thr) * 3.f, -40.f, 0.f) : 0.f;   // 1:4 below thr
        gr = target + (target > gr ? att : rel) * (gr - target);
        return fastDbToLin(gr);
    }
    float reductionDb() const { return -gr; }
private:
    float sr = 48000.f, thr = -50.f, rmsC = 0, att = 0, rel = 0, ms = 0, gr = 0;
    bool on = true;
};

// ---------------------------------------------------------------------------
// Chop + release. Three stages, all driven by the one knob, the way a
// produced scream is usually compressed:
//   1. FET-style peak comp: fast, catches the spikes.
//   2. Opto-style leveler: slow, program-dependent release, evens out phrases.
//   3. Parallel crush: a smashed copy blended underneath for density.
// Live, compression narrows the gap between voice and stage bleed; the
// bleed reducer (Expander) after it wins that gap back.
// ---------------------------------------------------------------------------
class Chop {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        fetAtt = coefMs(0.4f, sr); fetRel = coefMs(60.f, sr);
        optAtt = coefMs(12.f, sr); optRms = coefMs(8.f, sr);
        crAtt = coefMs(1.f, sr); crRel = coefMs(80.f, sr);
    }
    void set(float chop, float release) {
        amount = chop;
        fetThr = -8.f - 16.f * chop;  fetRatio = 4.f + 8.f * chop;
        optThr = -14.f - 14.f * chop; optRatio = 2.f + 2.f * chop;
        float relMs = expMap(release, 60.f, 800.f);
        optRelFast = coefMs(relMs * 0.25f, sr); optRelSlow = coefMs(relMs, sr);
        crushMix = 0.3f * chop;
        // Auto makeup: roughly what a -10 dBFS scream loses.
        float lost = -kneeCurve(-10.f - fetThr, fetRatio, 6.f) - kneeCurve(-10.f - optThr, optRatio, 10.f) * 0.7f;
        makeup = dbToLin(lost * 0.8f);
    }
    float process(float x) {
        if (amount < 0.001f) { grDb = 0.f; return x; }
        // 1. FET peak comp
        float xDb = fastLinToDb(std::fabs(x));
        float target = kneeCurve(xDb - fetThr, fetRatio, 6.f);
        fetGr = target + (target < fetGr ? fetAtt : fetRel) * (fetGr - target);
        float y = x * fastDbToLin(fetGr);
        // 2. Opto leveler on RMS, two-stage release (fast first 3 dB, then slow)
        optMs = y * y + optRms * (optMs - y * y);
        target = kneeCurve(fastPowDb10(optMs) - optThr, optRatio, 10.f);
        float rel = optGr < -3.f ? optRelSlow : optRelFast;
        optGr = target + (target < optGr ? optAtt : rel) * (optGr - target);
        y *= fastDbToLin(optGr);
        // 3. Parallel crush: 20:1 from -30 dB, blended under. Kept modest:
        //    a -40 dB threshold lifted stage bleed ~6 dB for little density.
        target = kneeCurve(xDb + 30.f, 20.f, 6.f);
        crGr = target + (target < crGr ? crAtt : crRel) * (crGr - target);
        float crushed = x * fastDbToLin(crGr + 16.f);
        grDb = -(fetGr + optGr);
        return (y + crushMix * crushed) * makeup;
    }
    float gainReductionDb() const { return grDb; }
private:
    float sr = 48000.f, amount = 0.5f, makeup = 1.f, crushMix = 0.f, grDb = 0.f;
    float fetThr = -16, fetRatio = 8, fetAtt = 0, fetRel = 0, fetGr = 0;
    float optThr = -20, optRatio = 3, optAtt = 0, optRms = 0, optRelFast = 0, optRelSlow = 0, optGr = 0, optMs = 0;
    float crAtt = 0, crRel = 0, crGr = 0;
};

// ---------------------------------------------------------------------------
// Micro-pitch shifter: two crossfading read heads sweeping a short delay.
// A few cents of detune plus a Haas delay is the classic "double-tracked"
// sound. The heads add 10-40 ms of delay, which is fine on a parallel branch.
// ---------------------------------------------------------------------------
class MicroPitch {
public:
    void init(float sampleRate, float baseMs) { sr = sampleRate; base = baseMs * 0.001f * sr; win = 0.03f * sr; }
    void setCents(float cents) { rate = std::pow(2.0, cents / 1200.0) - 1.0; }
    float process(float x) {
        dl.write(x);
        // Delay changes by -rate samples per sample -> pitch ratio (1 + rate).
        // The step is ~5e-6 per sample, too small for a float accumulator
        // to stay accurate, so the phase is double.
        ph += -rate / win;
        if (ph >= 1.0) ph -= 1.0;
        if (ph < 0.0) ph += 1.0;
        float p1 = (float)ph, p2 = p1 + 0.5f; if (p2 >= 1.f) p2 -= 1.f;
        float g1 = std::sin(kPi * p1), g2 = std::sin(kPi * p2);
        return g1 * g1 * dl.read(base + win * p1) + g2 * g2 * dl.read(base + win * p2);
    }
private:
    float sr = 48000.f, base = 600.f, win = 1440.f;
    double rate = 0.0, ph = 0.0;
    DelayLine<8192 * RASP_SR_SCALE> dl;
};

// ---------------------------------------------------------------------------
// Thicken: two micro-pitched doubles (one per side) + stereo comb taps,
// diffused, darkened, width-controlled. Runs on a parallel branch.
// ---------------------------------------------------------------------------
class Thicken {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        hpIn.set(150.f, 0.707f, sr);
        const float apMs[2][3] = {{3.1f, 5.3f, 7.9f}, {2.7f, 4.9f, 8.3f}};
        for (int c = 0; c < 2; c++) for (int i = 0; i < 3; i++) ap[c][i].setLen((int)(apMs[c][i] * 0.001f * sr));
        dblL.init(sr, 14.f); dblR.init(sr, 21.f);
        spreadS.init(0.6f, sr, 60.f);
    }
    void set(const Params& p) {
        spreadS.target = p.spread;
        diffuse = p.diffuse; width = p.width;
        float cents = 3.f + 15.f * p.spread;
        dblL.setCents(cents); dblR.setCents(-cents);
        float hz = expMap(1.f - p.darken, 1500.f, 18000.f);
        darkL.set(hz, 0.6f, sr); darkR.set(hz, 0.6f, sr);
    }
    void process(float x, float& L, float& R, bool mono = false) {
        hpIn.process(x);
        float h = hpIn.hp;
        if (mono) {
            // One-channel PA: comb taps would turn into a fixed comb filter when
            // summed, so only the micro-pitched doubles are kept.
            float d = 0.8f * (dblL.process(h) + dblR.process(h)) * 0.7071f;
            dl.write(h);
            darkL.process(d); darkR.process(d);
            L = R = darkL.lp;
            return;
        }
        dl.write(h);
        // Comb taps (ms) at full spread; spread scales them 0.25x..1x.
        static const float tL[4] = {4.7f, 11.3f, 17.9f, 26.3f}, tR[4] = {6.1f, 9.7f, 15.1f, 23.9f};
        static const float g[4] = {0.5f, -0.4f, 0.32f, -0.26f};
        float s = (0.25f + 0.75f * spreadS.next()) * 0.001f * sr;
        float l = 0.f, r = 0.f;
        for (int i = 0; i < 4; i++) { l += g[i] * dl.read(tL[i] * s); r += g[i] * dl.read(tR[i] * s); }
        float dlr = l, drr = r;
        for (int i = 0; i < 3; i++) { dlr = ap[0][i].process(dlr, 0.6f); drr = ap[1][i].process(drr, 0.6f); }
        l = lerp(l, dlr, diffuse); r = lerp(r, drr, diffuse);
        // Doubles: the main source of "size".
        l += 0.8f * dblL.process(h);
        r += 0.8f * dblR.process(h);
        darkL.process(l); darkR.process(r);
        float m = 0.5f * (darkL.lp + darkR.lp), sd = 0.5f * (darkL.lp - darkR.lp) * width * 1.3f;
        L = m + sd; R = m - sd;
    }
private:
    float sr = 48000.f, diffuse = 0.f, width = 0.f;
    SVF hpIn, darkL, darkR;
    DelayLine<4096 * RASP_SR_SCALE> dl;   // 85 ms @ 48k
    Allpass<1024 * RASP_SR_SCALE> ap[2][3];
    MicroPitch dblL, dblR;
    Smoothed spreadS;
};

// ---------------------------------------------------------------------------
// Plate: 8-line feedback delay network, Householder mix, damped, two lines
// gently modulated so long tails don't ring metallic. Pre-delay + input
// diffusion; RT60 set directly.
// ---------------------------------------------------------------------------
class Plate {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        static const float ms[8] = {23.1f, 27.7f, 31.9f, 35.3f, 39.7f, 43.1f, 47.9f, 53.3f};
        for (int i = 0; i < 8; i++) { len[i] = ms[i] * 0.001f * sr; damp[i].setCutoff(7000.f, sr); }
        static const float apMs[4] = {4.8f, 3.6f, 12.7f, 9.3f};
        for (int i = 0; i < 4; i++) inAp[i].setLen((int)(apMs[i] * 0.001f * sr));
        inHp.set(300.f, 0.707f, sr);
        modInc = 0.7 / sr;
    }
    void setDecay(float rt60) {
        for (int i = 0; i < 8; i++) fb[i] = std::pow(10.f, -3.f * len[i] / (rt60 * sr));
    }
    void process(float x, float& L, float& R) {
        inHp.process(x);
        pre.write(inHp.hp);
        float d = pre.tap((int)(0.025f * sr));                    // 25 ms pre-delay
        for (int i = 0; i < 4; i++) d = inAp[i].process(d, 0.7f);
        modPh += modInc; if (modPh >= 1.0) modPh -= 1.0;
        float mod = 0.0004f * sr * std::sin(2.f * kPi * (float)modPh);  // +-0.4 ms
        float o[8], sum = 0.f;
        for (int i = 0; i < 8; i++) {
            float dd = len[i] + (i == 2 ? mod : (i == 5 ? -mod : 0.f));
            o[i] = damp[i].lp(lines[i].read(dd)) * fb[i];
            sum += o[i];
        }
        sum *= 2.f / 8.f;                                          // Householder: I - 2/N * 11^T
        for (int i = 0; i < 8; i++) lines[i].write(o[i] - sum + ((i & 1) ? -d : d));
        L = o[0] + o[2] + o[4] + o[6];
        R = o[1] + o[3] + o[5] + o[7];
    }
private:
    float sr = 48000.f, fb[8] = {}, len[8] = {};
    double modPh = 0.0, modInc = 0.0;          // tiny per-sample step: keep it double
    DelayLine<8192 * RASP_SR_SCALE> lines[8];   // up to 85 ms @ 96k
    DelayLine<8192 * RASP_SR_SCALE> pre;
    Allpass<2048 * RASP_SR_SCALE> inAp[4];
    OnePole damp[8];
    SVF inHp;
};

// ---------------------------------------------------------------------------
// Flavour: plate + ping-pong delay on a parallel branch, ducked by the
// vocal, with a mix-style return EQ and tilt.
// ---------------------------------------------------------------------------
class Flavour {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        plate.init(sr);
        fbHp.setCutoff(300.f, sr); fbLp.setCutoff(4500.f, sr);
        retLpL.set(8000.f, 0.707f, sr); retLpR.set(8000.f, 0.707f, sr);
        tiltL.setCutoff(1000.f, sr); tiltR.setCutoff(1000.f, sr);
        duckAtt = coefMs(8.f, sr); duckRel = coefMs(300.f, sr);
        delayS.init(sr * 0.25f, sr, 80.f);
    }
    void set(const Params& p) {
        plate.setDecay(expMap(p.tail, 0.5f, 6.f));
        tailBlend = p.tailBlend; delayBlend = p.delayBlend; duck = p.duck;
        repeats = p.repeats * 0.85f;
        float beat = 60.f / clampf(p.bpm, 40.f, 240.f);
        float t = p.delayTime == 0 ? beat * 0.5f : (p.delayTime == 1 ? beat * 0.75f : beat);
        delayS.target = clampf(t * sr, 1.f, (float)(kDelayMax - 4));
        float db = (p.tilt - 0.5f) * 12.f;
        tiltLo = dbToLin(-db * 0.5f); tiltHi = dbToLin(db * 0.5f);
    }
    // key = the dry vocal (drives the ducking).
    void process(float x, float key, float& L, float& R) {
        float a = std::fabs(key);
        env = a + (a > env ? duckAtt : duckRel) * (env - a);
        float dg = 1.f - duck * 0.85f * clampf(env * 6.f, 0.f, 1.f);

        float rL, rR;
        plate.process(x, rL, rR);

        // Ping-pong: input goes left, repeats bounce L -> R -> L, each one darker.
        float ds = delayS.next();
        float dL = dlL.read(ds), dR = dlR.read(ds);
        float fbR = fbLp.lp(fbHp.hp(dR));
        dlL.write(x + fbR * repeats);
        dlR.write(dL * repeats);

        L = dg * (tailBlend * rL * 0.5f + delayBlend * dL);
        R = dg * (tailBlend * rR * 0.5f + delayBlend * dR);
        retLpL.process(L); retLpR.process(R);
        L = retLpL.lp; R = retLpR.lp;
        float lo = tiltL.lp(L); L = lo * tiltLo + (L - lo) * tiltHi;
        lo = tiltR.lp(R);       R = lo * tiltLo + (R - lo) * tiltHi;
    }
private:
    static constexpr int kDelayMax = (1 << 17) * RASP_SR_SCALE;   // 2.7 s @ 48k
    float sr = 48000.f, tailBlend = 0, delayBlend = 0, duck = 0, repeats = 0;
    float env = 0, duckAtt = 0, duckRel = 0, tiltLo = 1, tiltHi = 1;
    Plate plate;
    DelayLine<kDelayMax> dlL, dlR;
    OnePole fbHp, fbLp, tiltL, tiltR;
    SVF retLpL, retLpR;
    Smoothed delayS;
};

// ---------------------------------------------------------------------------
// Mega: a momentary "mega distortion" burst, modelled on a producer-printed
// accent layer (KnockedLoose session, 104 MEGA DIST VOCAL). That layer is
// hard-clipped and band-focused: relative to its 800 Hz octave, 100-200 Hz sit
// 29-36 dB down, 400 Hz -13, 1.6 kHz -7, 3.2 kHz -12, 6.4 kHz -17, 10 kHz -19.
// Run on the same singer's clean main stem, this voicing lands within ~4 dB
// of that from 400 Hz to 18 kHz (lows are tighter still).
// Level: the clipped tone is scaled by the voice's own envelope (fast RMS,
// tracked even while mega is off), like a gated fuzz. Matching wet to dry
// RMS instead would turn mic hiss in the gaps into full-level fizz.
// Pre-filter to the mids, clip hard (4x oversampled, ADAA), filter to the band
// again, then match the input loudness so stomping it changes texture, not
// level. Replaces the control output while held; thicken and flavour follow it.
// ---------------------------------------------------------------------------
class Mega {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        up.init(); down.init();
        preHp1.set(500.f, 0.54f, sr); preHp2.set(500.f, 1.31f, sr);
        prePeak.peaking(750.f, 0.9f, 6.f, sr);
        postHp.set(450.f, 0.707f, sr);
        postLp1.set(12000.f, 0.6f, sr); postLp2.set(12000.f, 0.6f, sr);
        postPeak.peaking(700.f, 1.0f, 9.f, sr);
        envAtt = coefMs(3.f, sr); envRel = coefMs(60.f, sr);
    }
    // Call every sample, engaged or not, so the envelope is ready on the first stomp.
    void track(float x) {
        float e = x * x;
        ms = e + (e > ms ? envAtt : envRel) * (ms - e);
    }
    float process(float x) {
        preHp1.process(x); preHp2.process(preHp1.hp);
        float u0 = prePeak.process(preHp2.hp) * kGain;
        float y = 0.f;
        for (int k = 0; k < 4; k++) {
            float u = up.process(k == 0 ? u0 * 4.f : 0.f);
            y = down.process(clipADAA(u));
        }
        postHp.process(y);
        postLp1.process(postHp.hp); postLp2.process(postLp1.lp);
        float wet = postPeak.process(postLp2.lp);
        return wet * std::sqrt(ms) * kSatInv;
    }
private:
    // First-order ADAA of a hard clip at +-1: F(x) = x^2/2 inside, |x| - 1/2 outside.
    float clipADAA(float c) {
        float a = std::fabs(c);
        float F = a <= 1.f ? 0.5f * c * c : a - 0.5f;
        float dx = c - prevC, y;
        if (std::fabs(dx) > 1e-5f) y = (F - prevF) / dx;
        else y = clampf(0.5f * (c + prevC), -1.f, 1.f);
        prevC = c; prevF = F;
        return y;
    }
    static constexpr float kGain = 60.f;          // +36 dB into the clipper
    static constexpr float kSatInv = 1.f / 2.0f;  // set so mega lands ~1 dB over the normal sound (3 singers)
    float sr = 48000.f, prevC = 0.f, prevF = 0.f, ms = 0.f, envAtt = 0.f, envRel = 0.f;
    OSFilter up, down;
    SVF preHp1, preHp2, postHp, postLp1, postLp2;
    Biquad prePeak, postPeak;
};

// ---------------------------------------------------------------------------
// Throw: a momentary delay send. While held, the voice feeds its own
// quarter-note ping-pong delay with long feedback; on release the send closes
// and the repeats ring out under the next phrase. Separate from the flavour
// delay so a throw never makes the normal delay louder, and it works with the
// flavour section switched off.
// ---------------------------------------------------------------------------
class Throw {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        fbHp.setCutoff(350.f, sr); fbLp.setCutoff(3500.f, sr);
        send.init(0.f, sr, 8.f);
        timeS.init(sr * 0.5f, sr, 80.f);
    }
    void set(bool on, float bpm) {
        send.target = on ? 1.f : 0.f;
        timeS.target = clampf(60.f / clampf(bpm, 40.f, 240.f) * sr, 1.f, (float)(kMax - 4));
    }
    void process(float x, float& L, float& R) {
        float t = timeS.next();
        float dL = dlL.read(t), dR = dlR.read(t);
        dlL.write(x * send.next() + fbLp.lp(fbHp.hp(dR)) * kFeedback);
        dlR.write(dL * kFeedback);
        L = kLevel * dL; R = kLevel * dR;
    }
private:
    static constexpr int kMax = (1 << 17) * RASP_SR_SCALE;      // 2.7 s @ 48k: a quarter note down to 22 bpm
    static constexpr float kFeedback = 0.62f, kLevel = 0.8f;    // ~4 s to -60 dB at 120 bpm
    float sr = 48000.f;
    DelayLine<kMax> dlL, dlR;
    OnePole fbHp, fbLp;
    Smoothed send, timeS;
};

// ---------------------------------------------------------------------------
// Bus: stereo-linked glue compressor (2:1, slow) into a -1 dBFS peak limiter.
// The output level drives the limiter, so turning it up makes the whole
// vocal denser and louder without clipping.
// ---------------------------------------------------------------------------
class Bus {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        rmsC = coefMs(10.f, sr); att = coefMs(15.f, sr); rel = coefMs(200.f, sr); limRel = coefMs(80.f, sr);
    }
    void set(float glue) { amount = glue; thr = -10.f - 10.f * glue; makeup = dbToLin(-kneeCurve(-12.f - thr, 2.f, 8.f) * 0.8f * (glue > 0.001f)); }
    void process(float& L, float& R) {
        if (amount > 0.001f) {
            float m = 0.5f * (L * L + R * R);
            ms = m + rmsC * (ms - m);
            float target = kneeCurve(fastPowDb10(ms) - thr, 2.f, 8.f);
            gr = target + (target < gr ? att : rel) * (gr - target);
            float g = fastDbToLin(gr) * makeup;
            L *= g; R *= g;
        }
        // Peak limiter: instant attack, linked, 80 ms release.
        const float ceil = 0.891f;                                 // -1 dBFS
        float a = std::fmax(std::fabs(L), std::fabs(R));
        lim = a > lim ? a : lim * limRel + a * (1.f - limRel);
        if (lim > ceil) { float g = ceil / lim; L *= g; R *= g; }
    }
    float limiterDb() const { return lim > 0.891f ? linToDb(lim / 0.891f) : 0.f; }
private:
    float sr = 48000.f, amount = 0.5f, thr = -15.f, makeup = 1.f, rmsC = 0, att = 0, rel = 0, limRel = 0;
    float ms = 0.f, gr = 0.f, lim = 0.f;
};

// ---------------------------------------------------------------------------
// ITU BS.1770 K-weighting (pre-filter shelf + RLB high-pass) for any rate.
// ---------------------------------------------------------------------------
struct KWeight {
    Biquad shelf, hp;
    void init(float sr) {
        double K = std::tan(3.14159265358979 * 1681.974450955533 / sr), Q = 0.7071752369554196;
        double Vh = std::pow(10.0, 3.999843853973347 / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        double a0 = 1 + K / Q + K * K;
        float c1[5] = {(float)((Vh + Vb * K / Q + K * K) / a0), (float)(2 * (K * K - Vh) / a0),
                       (float)((Vh - Vb * K / Q + K * K) / a0), (float)(2 * (K * K - 1) / a0),
                       (float)((1 - K / Q + K * K) / a0)};
        shelf.set(c1);
        K = std::tan(3.14159265358979 * 38.13547087602444 / sr); Q = 0.5003270373238773;
        double d = 1 + K / Q + K * K;
        float c2[5] = {1.f, -2.f, 1.f, (float)(2 * (K * K - 1) / d), (float)((1 - K / Q + K * K) / d)};
        hp.set(c2);
    }
    float process(float x) { return hp.process(shelf.process(x)); }
};

// ---------------------------------------------------------------------------
// Level-matched digital bypass. Live, the processed voice measured ~18 dB
// louder than the dry mic, so a plain bypass would make the voice vanish when
// you talk between songs. This keeps measuring both (K-weighted, 400 ms
// blocks, only while you're singing, ~10 s memory) and plays the bypassed
// voice at the effect's loudness, with a 25 ms equal-power crossfade.
// ---------------------------------------------------------------------------
class BypassMatcher {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        kDry.init(sr); kL.init(sr); kR.init(sr);
        blockLen = (int)(0.4f * sr);
        emaC = std::exp(-0.4 / 10.0);                // 10 s memory, in blocks
        fade.init(0.f, sr, 8.f);                       // ~25 ms to settle
        gain.init(1.f, sr, 200.f);
    }
    // dry: the mono voice; L/R: the processed output. Writes the final output.
    void process(float dry, float& L, float& R, bool bypass, bool match) {
        float kd = kDry.process(dry), l = kL.process(L), r = kR.process(R);
        accD += 2.f * kd * kd;                         // dry plays on both channels
        accW += l * l + r * r;
        if (++accN >= blockLen) {
            double ed = accD / accN, ew = accW / accN;
            if (lufs(ed) > -50.0) {                    // only learn while the voice is there
                emaD = emaD > 0 ? emaD * emaC + ed * (1 - emaC) : ed;
                emaW = emaW > 0 ? emaW * emaC + ew * (1 - emaC) : ew;
            }
            accD = accW = 0; accN = 0;
        }
        float gDb = (match && emaD > 0 && emaW > 0) ? clampf((float)(lufs(emaW) - lufs(emaD)), -12.f, 30.f) : 0.f;
        gain.target = dbToLin(gDb);
        fade.target = bypass ? 1.f : 0.f;
        float f = fade.next(), g = gain.next();
        float a = std::cos(0.5f * kPi * f), b = std::sin(0.5f * kPi * f);   // equal power
        float d = safetyClip(dry * g);
        L = a * L + b * d;
        R = a * R + b * d;
    }
    float matchDb() const { return (emaD > 0 && emaW > 0) ? (float)(lufs(emaW) - lufs(emaD)) : 0.f; }
private:
    static double lufs(double ms) { return -0.691 + 10.0 * std::log10(ms + 1e-20); }
    float sr = 48000.f;
    KWeight kDry, kL, kR;
    int blockLen = 19200, accN = 0;
    double accD = 0, accW = 0, emaD = 0, emaW = 0, emaC = 0.96;
    Smoothed fade, gain;
};

// ---------------------------------------------------------------------------
// Radix-2 FFT (in place, complex), for the howl detector.
// ---------------------------------------------------------------------------
template <int N>
struct FFT {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    float cosT[(size_t)N / 2], sinT[(size_t)N / 2];
    int rev[(size_t)N];
    void init() {
        int bits = 0; while ((1 << bits) < N) bits++;
        for (int i = 0; i < N; i++) { int r = 0; for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b); rev[i] = r; }
        for (int i = 0; i < N / 2; i++) { cosT[i] = std::cos(2.0 * 3.14159265358979 * i / N); sinT[i] = -std::sin(2.0 * 3.14159265358979 * i / N); }
    }
    void run(float* re, float* im) const {
        for (int i = 0; i < N; i++) if (i < rev[i]) { std::swap(re[i], re[rev[i]]); std::swap(im[i], im[rev[i]]); }
        for (int len = 2; len <= N; len <<= 1) {
            int half = len >> 1, step = N / len;
            for (int i = 0; i < N; i += len)
                for (int j = 0; j < half; j++) {
                    float wr = cosT[j * step], wi = sinT[j * step];
                    float xr = re[i + j + half] * wr - im[i + j + half] * wi;
                    float xi = re[i + j + half] * wi + im[i + j + half] * wr;
                    re[i + j + half] = re[i + j] - xr; im[i + j + half] = im[i + j] - xi;
                    re[i + j] += xr; im[i + j] += xi;
                }
        }
    }
};

// ---------------------------------------------------------------------------
// HowlGuard: automatic feedback suppression. Feedback is a single pure tone
// that holds still and grows; a voice (even a sustained clean note) has
// harmonics and moves, and a scream is noisy. Every ~21 ms the output
// spectrum is checked for a peak that is
//   - far above the average (> 20 dB), and narrow (> 12 dB over +-3..6 bins),
//   - without harmonics at 2x or 1/2x (> 18 dB below),
//   - at the same frequency for ~250 ms.
// Then a narrow -12 dB notch goes on it (deeper if it comes back), up to 6.
// The notches stay until cleared, the way stage feedback suppressors work.
// ---------------------------------------------------------------------------
class HowlGuard {
public:
    static constexpr int kN = 2048, kHop = 1024, kMaxNotch = 6;
    void init(float sampleRate) {
        sr = sampleRate;
        fft.init();
        for (int i = 0; i < kN; i++) win[i] = 0.5f - 0.5f * std::cos(2.f * kPi * i / (kN - 1));
        clear();
    }
    void clear() {
        for (auto& n : notches) { n.active = false; n.depthDb = 0.f; }
        candBin = -1; candFrames = 0; deployed = 0;
    }
    void analyze(float x) {
        ring[pos] = x; pos = (pos + 1) & (kN - 1);
        if (++hopCount < kHop) return;
        hopCount = 0;
        frame();
    }
    // Apply the active notches (same filters on both channels).
    void process(float& L, float& R) {
        for (int i = 0; i < kMaxNotch; i++) if (notches[i].active) { L = notches[i].l.process(L); R = notches[i].r.process(R); }
    }
    // Don't deploy new notches for now + 0.3 s: mega's clipped, mid-focused tone
    // is tonal enough to read as feedback (one false notch at 645 Hz on a
    // Gojira lead). Existing notches keep working.
    void holdOff() { hold = (int)(0.3f * sr / kHop) + 1; }
    int activeNotches() const { int c = 0; for (const auto& n : notches) c += n.active; return c; }
    int deployedCount() const { return deployed; }
    float notchHz(int i) const { return notches[i].active ? notches[i].hz : 0.f; }
private:
    int hold = 0;
    struct Notch { bool active = false; float hz = 0, depthDb = 0; uint32_t age = 0; Biquad l, r; };
    void frame() {
        if (hold > 0) { hold--; candFrames = 0; candBin = -1; return; }
        for (int i = 0; i < kN; i++) { re[i] = ring[(pos + i) & (kN - 1)] * win[i]; im[i] = 0.f; }
        fft.run(re, im);
        int lo = (int)(150.f / sr * kN), hi = (int)(8000.f / sr * kN);
        float sum = 0.f; int best = lo; float bestP = 0.f;
        for (int k = lo; k <= hi; k++) {
            float pk = re[k] * re[k] + im[k] * im[k];
            mag[k] = pk; sum += pk;
            if (pk > bestP) { bestP = pk; best = k; }
        }
        for (int k = hi + 1; k < kN / 2 && k <= 2 * hi + 4; k++) mag[k] = re[k] * re[k] + im[k] * im[k];
        float mean = sum / (hi - lo + 1);
        bool tonal = false;
        float level = bestP / (0.25f * kN * kN);        // ~ power relative to a full-scale sine
        if (level > 1e-5f && bestP > 100.f * mean) {      // > -50 dBFS-ish and > 20 dB over the average
            float side = 0.f; int ns = 0;
            for (int d = 3; d <= 6; d++) { if (best - d >= lo) { side += mag[best - d]; ns++; } if (best + d <= hi) { side += mag[best + d]; ns++; } }
            side /= ns;
            float h2 = 0.f, h12 = 0.f;
            for (int d = -1; d <= 1; d++) {
                int k2 = 2 * best + d; if (k2 < kN / 2) h2 = std::fmax(h2, mag[k2]);
                int k12 = best / 2 + d; if (k12 >= lo) h12 = std::fmax(h12, mag[k12]);
            }
            tonal = bestP > 16.f * side && bestP > 63.f * h2 && bestP > 63.f * h12;
        }
        if (tonal && candBin >= 0 && std::abs(best - candBin) <= 1) candFrames++;
        else if (tonal) { candBin = best; candFrames = 1; }
        else { candBin = -1; candFrames = 0; }
        if (candFrames >= 12) {                           // ~250 ms at 48 kHz
            // Parabolic interpolation for the exact frequency.
            float a = std::log(mag[best - 1] + 1e-20f), b = std::log(mag[best] + 1e-20f), c = std::log(mag[best + 1] + 1e-20f);
            float off = 0.5f * (a - c) / (a - 2 * b + c + 1e-20f);
            deploy((best + clampf(off, -0.5f, 0.5f)) * sr / kN);
            candBin = -1; candFrames = 0;
        }
        for (auto& n : notches) n.age++;
    }
    void deploy(float hz) {
        for (auto& n : notches)                           // already notched nearby: go deeper
            if (n.active && std::fabs(std::log2(hz / n.hz)) < 1.f / 12.f) { setNotch(n, n.hz, std::fmax(n.depthDb - 6.f, -24.f)); deployed++; return; }
        Notch* slot = nullptr;
        for (auto& n : notches) if (!n.active) { slot = &n; break; }
        if (!slot) { slot = &notches[0]; for (auto& n : notches) if (n.age > slot->age) slot = &n; }   // replace the oldest
        setNotch(*slot, hz, -12.f);
        slot->active = true; slot->age = 0;
        deployed++;
    }
    void setNotch(Notch& n, float hz, float depthDb) {
        n.hz = hz; n.depthDb = depthDb;
        n.l.peaking(hz, 20.f, depthDb, sr); n.r.peaking(hz, 20.f, depthDb, sr);
    }
    float sr = 48000.f;
    FFT<kN> fft;
    float ring[kN] = {}, win[kN], re[kN], im[kN], mag[kN] = {};
    int pos = 0, hopCount = 0, candBin = -1, candFrames = 0, deployed = 0;
    Notch notches[kMaxNotch];
};

// ---------------------------------------------------------------------------
// The whole box. ~1.4 MB of state (delay lines): on Daisy, place it in SDRAM.
// ---------------------------------------------------------------------------
class Processor {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        drive.init(sr); deEss.init(sr); expander.init(sr); chop.init(sr); thicken.init(sr); flavour.init(sr); bus.init(sr);
        bypassM.init(sr); howlG.init(sr); mega.init(sr); throwFx.init(sr);
        gMega.init(0.f, sr, 10.f);
        gIn.init(1.f, sr); gCtl.init(1.f, sr); gThk.init(0.6f, sr); gFlv.init(0.55f, sr); gOut.init(1.f, sr); gMix.init(0.6f, sr);
        setParams(p);
    }

    void setParams(const Params& np) {
        p = np;
        loCut1.set(expMap(p.loCut, 20.f, 600.f), 0.54f, sr);   // 4th-order Butterworth pair
        loCut2.set(expMap(p.loCut, 20.f, 600.f), 1.31f, sr);
        hiCut1.set(expMap(p.hiCut, 2000.f, 20000.f), 0.54f, sr);
        hiCut2.set(expMap(p.hiCut, 2000.f, 20000.f), 1.31f, sr);
        driveHp1.set(expMap(p.loCut, 20.f, 600.f), 0.54f, sr);   // 24 dB/oct, like the lo cut
        driveHp2.set(expMap(p.loCut, 20.f, 600.f), 1.31f, sr);
        drive.set(p.drive, p.brightness);
        voicing.set(p.brightness, p.airDb, sr);
        deEss.set(p.deEss);
        expander.set(p.bleed);
        chop.set(p.chop, p.release);
        thicken.set(p); flavour.set(p); bus.set(p.glue);
        gCtl.target = dbToLin(p.controlGainDb);
        gThk.target = p.thickenOn ? dbToLin(p.thickenGainDb) : 0.f;
        gFlv.target = p.flavourOn ? dbToLin(p.flavourGainDb) : 0.f;
        gOut.target = dbToLin(p.outDb);
        gIn.target = dbToLin(p.inDb);
        gMix.target = p.mix;
        gMega.target = p.mega ? 1.f : 0.f;
        throwFx.set(p.throwOn, p.bpm);
    }

    void process(float in, float& outL, float& outR) {
        // CONTROL (series)
        float x = in * gIn.next();
        const float dryVoice = x;                      // for the level-matched bypass
        float gm = gMix.next(), gc = gCtl.next();
        if (p.controlOn) {
            loCut1.process(x); loCut2.process(loCut1.hp);
            hiCut1.process(loCut2.hp); hiCut2.process(hiCut1.lp);
            x = hiCut2.lp;
            float eg = expander.gain(x);              // keyed on the clean signal
            if (gm > 1e-4f) {                          // skip the heaviest stage when unused
                // Distortion creates new low-frequency difference tones below
                // the lo cut, so the wet path gets its own high-pass.
                driveHp1.process(drive.process(x)); driveHp2.process(driveHp1.hp);
                x = lerp(x, driveHp2.hp, gm);
            }
            x = voicing.process(x);
            x = deEss.process(x);
            x = chop.process(x) * eg * gc;
        }
        // MOMENTARY: mega replaces the voice (crossfaded) before the effects.
        float gmg = gMega.next();
        megaFade = gmg;
        mega.track(x);
        if (gmg > 1e-4f) x = lerp(x, mega.process(x), gmg);
        // THICKEN + FLAVOUR (parallel, fed by the control output)
        float tL = 0, tR = 0, fL = 0, fR = 0;
        float gt = gThk.next(), gf = gFlv.next();
        if (gt > 1e-4f) thicken.process(x, tL, tR, p.mono);
        if (gf > 1e-4f) flavour.process(x, x, fL, fR);
        float dry = p.solo ? 0.f : x;
        float go = gOut.next();
        float wL, wR;
        throwFx.process(x, wL, wR);
        float L = (dry + gt * tL + gf * fL + wL) * go, R = (dry + gt * tR + gf * fR + wR) * go;
        if (p.mono) { float m = 0.5f * (L + R); L = R = m; }
        // BUS
        bus.process(L, R);
        L = safetyClip(L);
        R = safetyClip(R);
        bypassM.process(dryVoice, L, R, p.bypass, p.bypassMatch);
        // Feedback notches last, so they also protect the bypassed voice; the
        // detector listens to exactly what goes to the speakers.
        if (p.howl) {
            if (megaFade > 1e-4f) howlG.holdOff();
            howlG.process(L, R);
            howlG.analyze(0.5f * (L + R));
        }
        outL = L;
        outR = R;
    }

    float gainReductionDb() const { return chop.gainReductionDb(); }
    float limiterDb() const { return bus.limiterDb(); }
    float bleedReductionDb() const { return expander.reductionDb(); }
    float bypassMatchDb() const { return bypassM.matchDb(); }
    int howlNotches() const { return howlG.activeNotches(); }
    int howlEvents() const { return howlG.deployedCount(); }
    float howlNotchHz(int i) const { return howlG.notchHz(i); }
    void clearHowl() { howlG.clear(); }

private:
    float sr = 48000.f;
    Params p;
    SVF loCut1, loCut2, hiCut1, hiCut2, driveHp1, driveHp2;
    Drive drive;
    Voicing voicing;
    DeEsser deEss;
    Expander expander;
    Chop chop;
    Thicken thicken;
    Flavour flavour;
    Bus bus;
    BypassMatcher bypassM;
    HowlGuard howlG;
    Mega mega;
    Throw throwFx;
    Smoothed gIn, gCtl, gThk, gFlv, gOut, gMix, gMega;
    float megaFade = 0.f;
};

} // namespace rasp
