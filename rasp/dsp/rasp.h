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
//
// Header-only C++17, no heap, no dependencies. No lookahead and no block
// processing: the only delay on the dry path is filter phase delay in the
// drive (oversampling + tone filters, ~0.15 ms at 3 kHz).
#pragma once
#include <cmath>
#include <cstdint>

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
    float buf[N] = {};
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
    float buf[N] = {};
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
    float loCut      = 0.50f;  // 20..600 Hz   (0.5 = 110 Hz)
    float hiCut      = 0.85f;  // 2k..20k Hz   (0.85 = 14 kHz)
    float drive      = 0.40f;  // Tube-Screamer-style gain
    int   brightness = 1;      // 3-way tone: 0 dark, 1 natural, 2 bright (drive tone + voicing EQ)
    float mix        = 0.60f;  // parallel blend of the drive (level-matched)
    float chop       = 0.55f;  // compression amount: peak comp + leveler + parallel crush
    float release    = 0.40f;  // leveler release 60..800 ms
    float deEss      = 0.50f;  // de-esser depth (menu)
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
    float tail       = 0.50f;  // plate decay, RT60 0.5..6 s
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
        hp720.setCutoff(720.f, sr * 4.f);
        rmsIn.setCutoff(3.f, sr); rmsOut.setCutoff(3.f, sr);
        level.init(1.f, sr, 50.f);
    }
    void set(float drive, int brightness) {
        float d2 = drive * drive;
        gain = 1.f + 120.f * d2;                                   // TS: 1 + (51k + 500k*pot)/4.7k
        float rf = 51e3f + 500e3f * d2;
        capLp.setCutoff(clampf(1.f / (2.f * kPi * rf * 51e-12f), 1000.f, sr * 1.8f), sr * 4.f);
        static const float toneHz[3] = {2400.f, 4200.f, 7500.f};
        tone.set(toneHz[brightness < 0 ? 0 : (brightness > 2 ? 2 : brightness)], 0.6f, sr);
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
    void set(int brightness, float sr) {
        struct V { float lowHz, lowDb, mudHz, mudDb, presHz, presDb, airHz, airDb; };
        static const V v[3] = {
            {170.f, 2.5f, 420.f, -2.0f, 2600.f, 1.5f, 10000.f, 0.5f},   // dark: weight, gutturals
            {150.f, 1.5f, 450.f, -3.0f, 3000.f, 3.0f, 10000.f, 2.5f},   // natural
            {120.f, 0.5f, 500.f, -3.5f, 3500.f, 4.0f, 11000.f, 4.0f},   // bright: highs, squeals
        };
        const V& c = v[brightness < 0 ? 0 : (brightness > 2 ? 2 : brightness)];
        low.lowShelf(c.lowHz, c.lowDb, sr);
        mud.peaking(c.mudHz, 1.2f, c.mudDb, sr);
        pres.peaking(c.presHz, 0.9f, c.presDb, sr);
        air.highShelf(c.airHz, c.airDb, sr);
    }
    float process(float x) { return air.process(pres.process(mud.process(low.process(x)))); }
private:
    Biquad low, mud, pres, air;
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
    DelayLine<8192> dl;
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
    void process(float x, float& L, float& R) {
        hpIn.process(x);
        float h = hpIn.hp;
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
    DelayLine<4096> dl;                // 85 ms @ 48k
    Allpass<1024> ap[2][3];
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
    DelayLine<8192> lines[8];          // up to 85 ms @ 96k
    DelayLine<8192> pre;
    Allpass<2048> inAp[4];
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
    static constexpr int kDelayMax = 1 << 17;   // 2.7 s @ 48k
    float sr = 48000.f, tailBlend = 0, delayBlend = 0, duck = 0, repeats = 0;
    float env = 0, duckAtt = 0, duckRel = 0, tiltLo = 1, tiltHi = 1;
    Plate plate;
    DelayLine<kDelayMax> dlL, dlR;
    OnePole fbHp, fbLp, tiltL, tiltR;
    SVF retLpL, retLpR;
    Smoothed delayS;
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
// The whole box. ~1.4 MB of state (delay lines): on Daisy, place it in SDRAM.
// ---------------------------------------------------------------------------
class Processor {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        drive.init(sr); deEss.init(sr); expander.init(sr); chop.init(sr); thicken.init(sr); flavour.init(sr); bus.init(sr);
        gCtl.init(1.f, sr); gThk.init(0.6f, sr); gFlv.init(0.55f, sr); gOut.init(1.f, sr); gMix.init(0.6f, sr);
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
        voicing.set(p.brightness, sr);
        deEss.set(p.deEss);
        expander.set(p.bleed);
        chop.set(p.chop, p.release);
        thicken.set(p); flavour.set(p); bus.set(p.glue);
        gCtl.target = dbToLin(p.controlGainDb);
        gThk.target = p.thickenOn ? dbToLin(p.thickenGainDb) : 0.f;
        gFlv.target = p.flavourOn ? dbToLin(p.flavourGainDb) : 0.f;
        gOut.target = dbToLin(p.outDb);
        gMix.target = p.mix;
    }

    void process(float in, float& outL, float& outR) {
        // CONTROL (series)
        float x = in;
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
        // THICKEN + FLAVOUR (parallel, fed by the control output)
        float tL = 0, tR = 0, fL = 0, fR = 0;
        float gt = gThk.next(), gf = gFlv.next();
        if (gt > 1e-4f) thicken.process(x, tL, tR);
        if (gf > 1e-4f) flavour.process(x, x, fL, fR);
        float dry = p.solo ? 0.f : x;
        float go = gOut.next();
        float L = (dry + gt * tL + gf * fL) * go, R = (dry + gt * tR + gf * fR) * go;
        // BUS
        bus.process(L, R);
        outL = safetyClip(L);
        outR = safetyClip(R);
    }

    float gainReductionDb() const { return chop.gainReductionDb(); }
    float limiterDb() const { return bus.limiterDb(); }
    float bleedReductionDb() const { return expander.reductionDb(); }

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
    Smoothed gCtl, gThk, gFlv, gOut, gMix;
};

} // namespace rasp
