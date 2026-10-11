// RASP - extreme vocal processor DSP core.
//
// A channel strip for a vocal that is ALREADY screamed: it doesn't synthesize
// a scream, it makes a real one sit like a produced record. The layout follows
// screamer-style plugins (Ablaze Screamer), as publicly described:
//
//              ┌──────────── dry (control out) ───────────────┐
//   in ─► CONTROL ─┼─► THICKEN  (parallel: stereo comb + diffusion) ─┼─► sum ─► out L/R
//              └─► FLAVOUR  (parallel: reverb + delay, ducked) ─┘
//
//   CONTROL  lo cut / hi cut, drive (Tube-Screamer-style, 3-way brightness),
//            mix, chop + release (compressor into a peak limiter), gain
//   THICKEN  spread (comb tap spacing), diffuse, darken, width, solo, gain
//   FLAVOUR  tail + blend (reverb), repeats + blend (ping-pong delay, 3 times),
//            tilt (EQ on the effect), duck (effect dips while you sing), gain
//
// Header-only C++17, no heap, no dependencies. No lookahead and no block
// processing: the only delay is filter phase delay in the drive path
// (oversampling + tone filters, ~0.15 ms at 3 kHz).
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

// Linear up to 0.8, then rounds off to 1.0. Only touches overs.
inline float safetyClip(float x) {
    float a = std::fabs(x);
    if (a <= 0.8f) return x;
    float y = 0.8f + 0.2f * fastTanh((a - 0.8f) * 5.f);
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

// Biquad, transposed direct form II. c = {b0, b1, b2, a1, a2}.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(const float* c) { b0 = c[0]; b1 = c[1]; b2 = c[2]; a1 = c[3]; a2 = c[4]; }
    float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
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

struct Noise {
    uint32_t s = 0x9E3779B9u;
    float next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(int32_t)s * (1.f / 2147483648.f);
    }
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

// ---------------------------------------------------------------------------
// Parameters. Knobs are 0..1 unless noted.
// ---------------------------------------------------------------------------
struct Params {
    // CONTROL
    bool  controlOn  = true;
    float loCut      = 0.50f;  // 20..600 Hz   (0.5 = 110 Hz)
    float hiCut      = 0.75f;  // 2k..20k Hz   (0.75 = 11 kHz)
    float drive      = 0.35f;  // Tube-Screamer-style gain
    int   brightness = 1;      // 3-way: 0 dark, 1 natural, 2 bright
    float mix        = 0.60f;  // parallel blend of the drive (level-matched)
    float chop       = 0.50f;  // compression amount (threshold + ratio together)
    float release    = 0.40f;  // compressor release 25..600 ms
    float controlGainDb = 0.f;

    // THICKEN (parallel)
    bool  thickenOn  = true;
    float spread     = 0.50f;  // comb tap spacing
    float diffuse    = 0.40f;  // smear the taps into a dense cloud
    float darken     = 0.30f;  // lowpass on the thicken branch
    float width      = 0.80f;  // stereo width of the branch
    bool  solo       = false;  // hear only the thicken branch
    float thickenGainDb = -6.f;

    // FLAVOUR (parallel)
    bool  flavourOn  = true;
    float tail       = 0.45f;  // reverb decay, RT60 0.4..6 s
    float tailBlend  = 0.25f;  // reverb level
    float repeats    = 0.25f;  // delay feedback
    float delayBlend = 0.15f;  // delay level
    int   delayTime  = 0;      // 3-way: 0 = 1/8, 1 = dotted 1/8, 2 = 1/4
    float bpm        = 120.f;
    float tilt       = 0.50f;  // EQ on the effect: 0 dark .. 1 bright (+-6 dB)
    float duck       = 0.60f;  // effect dips while the vocal is present
    float flavourGainDb = -6.f;

    float outDb      = 0.f;
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
        static const float toneHz[3] = {2200.f, 3800.f, 7000.f};
        tone.set(toneHz[brightness < 0 ? 0 : (brightness > 2 ? 2 : brightness)], 0.6f, sr);
    }
    float process(float x) {
        // 4x oversampled TS core.
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
        float dx = c - prevC, y;
        if (std::fabs(dx) > 1e-4f) y = (logCosh(c) - logCosh(prevC)) / dx;
        else y = std::tanh(0.5f * (c + prevC));
        prevC = c;
        return y;
    }
    static constexpr float kClip = 0.3f;
    float prevC = 0.f;
    float sr = 48000.f, gain = 1.f;
    OSFilter up, down;
    OnePole hp720, capLp, rmsIn, rmsOut;
    SVF tone;
    Smoothed level;
};

// ---------------------------------------------------------------------------
// Chop + release: soft-knee compressor into a zero-lookahead peak limiter.
// One knob sets threshold and ratio together, so the scream stays even
// without being flattened.
// ---------------------------------------------------------------------------
class Chop {
public:
    void init(float sampleRate) { sr = sampleRate; att = coefMs(1.5f, sr); limRel = coefMs(60.f, sr); }
    void set(float chop, float release) {
        thr = -6.f - 30.f * chop;                       // dBFS
        ratio = 1.5f + 6.5f * chop;
        rel = coefMs(expMap(release, 25.f, 600.f), sr);
        makeup = dbToLin(-thr * (1.f - 1.f / ratio) * 0.5f);
        amount = chop;
    }
    float process(float x) {
        float out = x;
        if (amount > 0.001f) {
            float lvl = linToDb(std::fabs(x));
            float over = lvl - thr, target = 0.f;
            const float knee = 8.f;
            if (over > knee * 0.5f) target = over * (1.f / ratio - 1.f);
            else if (over > -knee * 0.5f) { float t = over + knee * 0.5f; target = (1.f / ratio - 1.f) * t * t / (2.f * knee); }
            grDb = target + (target < grDb ? att : rel) * (grDb - target);  // gain reduction is negative
            out = x * dbToLin(grDb) * makeup;
        }
        // Peak limiter at -1 dBFS: instant attack, so the output never passes it.
        const float ceil = 0.891f;
        float a = std::fabs(out);
        limEnv = a > limEnv ? a : limEnv * limRel + a * (1.f - limRel);
        if (limEnv > ceil) out *= ceil / limEnv;
        return out;
    }
    float gainReductionDb() const { return -grDb + (limEnv > 0.891f ? linToDb(limEnv / 0.891f) : 0.f); }
private:
    float sr = 48000.f, thr = -20.f, ratio = 4.f, att = 0, rel = 0, makeup = 1.f, amount = 0.5f;
    float grDb = 0.f, limEnv = 0.f, limRel = 0.f;
};

// ---------------------------------------------------------------------------
// Thicken: stereo comb filtering + diffusion (no modulation, so no wobble).
// Four fixed short taps per side at different spacings; the L/R difference
// is what makes it wide. Runs on a parallel branch.
// ---------------------------------------------------------------------------
class Thicken {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        hpIn.set(180.f, 0.707f, sr);
        const float apMs[2][3] = {{3.1f, 5.3f, 7.9f}, {2.7f, 4.9f, 8.3f}};
        for (int c = 0; c < 2; c++) for (int i = 0; i < 3; i++) ap[c][i].setLen((int)(apMs[c][i] * 0.001f * sr));
        spreadS.init(0.5f, sr, 60.f);
    }
    void set(const Params& p) {
        spreadS.target = p.spread;
        diffuse = p.diffuse; width = p.width;
        float hz = expMap(1.f - p.darken, 1500.f, 18000.f);
        darkL.set(hz, 0.6f, sr); darkR.set(hz, 0.6f, sr);
    }
    void process(float x, float& L, float& R) {
        hpIn.process(x);
        dl.write(hpIn.hp);
        // Tap times (ms) at full spread; spread scales them 0.25x..1x.
        static const float tL[4] = {4.7f, 11.3f, 17.9f, 26.3f}, tR[4] = {6.1f, 9.7f, 15.1f, 23.9f};
        static const float g[4] = {0.62f, -0.5f, 0.42f, -0.34f};
        float s = (0.25f + 0.75f * spreadS.next()) * 0.001f * sr;
        float l = 0.f, r = 0.f;
        for (int i = 0; i < 4; i++) { l += g[i] * dl.read(tL[i] * s); r += g[i] * dl.read(tR[i] * s); }
        float dlr = l, drr = r;
        for (int i = 0; i < 3; i++) { dlr = ap[0][i].process(dlr, 0.6f); drr = ap[1][i].process(drr, 0.6f); }
        l = lerp(l, dlr, diffuse); r = lerp(r, drr, diffuse);
        darkL.process(l); darkR.process(r);
        float m = 0.5f * (darkL.lp + darkR.lp), sd = 0.5f * (darkL.lp - darkR.lp) * width * 1.4f;
        L = m + sd; R = m - sd;
    }
private:
    float sr = 48000.f, diffuse = 0.f, width = 0.f;
    SVF hpIn, darkL, darkR;
    DelayLine<4096> dl;                // 85 ms @ 48k
    Allpass<1024> ap[2][3];
    Smoothed spreadS;
};

// ---------------------------------------------------------------------------
// Reverb: 4-line feedback delay network, Hadamard mix, damped, with input
// diffusion and pre-delay. Decay is set as an RT60.
// ---------------------------------------------------------------------------
class Reverb {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        const float ms[4] = {29.7f, 37.1f, 41.1f, 43.7f};
        for (int i = 0; i < 4; i++) { len[i] = (int)(ms[i] * 0.001f * sr); damp[i].setCutoff(6500.f, sr); }
        inAp[0].setLen((int)(0.0048f * sr)); inAp[1].setLen((int)(0.0036f * sr));
        inAp[2].setLen((int)(0.0127f * sr)); inAp[3].setLen((int)(0.0093f * sr));
        inHp.set(250.f, 0.707f, sr);
    }
    void setDecay(float rt60) {
        for (int i = 0; i < 4; i++) fb[i] = std::pow(10.f, -3.f * (float)len[i] / (rt60 * sr));
    }
    void process(float x, float& L, float& R) {
        inHp.process(x);
        pre.write(inHp.hp);
        float d = pre.tap((int)(0.015f * sr));                    // 15 ms pre-delay
        for (int i = 0; i < 4; i++) d = inAp[i].process(d, 0.65f);
        float o[4];
        for (int i = 0; i < 4; i++) o[i] = damp[i].lp(lines[i].tap(len[i])) * fb[i];
        // 4x4 Hadamard / 2: energy-preserving mix.
        float h0 = 0.5f * (o[0] + o[1] + o[2] + o[3]), h1 = 0.5f * (o[0] - o[1] + o[2] - o[3]);
        float h2 = 0.5f * (o[0] + o[1] - o[2] - o[3]), h3 = 0.5f * (o[0] - o[1] - o[2] + o[3]);
        lines[0].write(h0 + d); lines[1].write(h1 + d); lines[2].write(h2 - d); lines[3].write(h3 - d);
        L = o[0] - o[2]; R = o[1] - o[3];
    }
private:
    float sr = 48000.f, fb[4] = {};
    int len[4] = {};
    DelayLine<8192> lines[4];          // up to 85 ms @ 96k
    DelayLine<4096> pre;
    Allpass<1024> inAp[4];
    OnePole damp[4];
    SVF inHp;
};

// ---------------------------------------------------------------------------
// Flavour: reverb + ping-pong delay on a parallel branch, ducked by the
// vocal, then tilt EQ on the effect only.
// ---------------------------------------------------------------------------
class Flavour {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        verb.init(sr);
        fbHp.setCutoff(250.f, sr); fbLp.setCutoff(5000.f, sr);
        tiltL.setCutoff(1000.f, sr); tiltR.setCutoff(1000.f, sr);
        duckAtt = coefMs(8.f, sr); duckRel = coefMs(250.f, sr);
        delayS.init(sr * 0.25f, sr, 80.f);
    }
    void set(const Params& p) {
        verb.setDecay(expMap(p.tail, 0.4f, 6.f));
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
        float dg = 1.f - duck * 0.9f * clampf(env * 6.f, 0.f, 1.f);

        float rL, rR;
        verb.process(x, rL, rR);

        // Ping-pong: input goes left, repeats bounce L -> R -> L, each one darker.
        float ds = delayS.next();
        float dL = dlL.read(ds), dR = dlR.read(ds);
        float fbR = fbLp.lp(fbHp.hp(dR));
        dlL.write(x + fbR * repeats);
        dlR.write(dL * repeats);

        L = dg * (tailBlend * rL + delayBlend * dL);
        R = dg * (tailBlend * rR + delayBlend * dR);
        float lo = tiltL.lp(L); L = lo * tiltLo + (L - lo) * tiltHi;
        lo = tiltR.lp(R);       R = lo * tiltLo + (R - lo) * tiltHi;
    }
private:
    static constexpr int kDelayMax = 1 << 17;   // 2.7 s @ 48k
    float sr = 48000.f, tailBlend = 0, delayBlend = 0, duck = 0, repeats = 0;
    float env = 0, duckAtt = 0, duckRel = 0, tiltLo = 1, tiltHi = 1;
    Reverb verb;
    DelayLine<kDelayMax> dlL, dlR;
    OnePole fbHp, fbLp, tiltL, tiltR;
    Smoothed delayS;
};

// ---------------------------------------------------------------------------
// The whole box. ~1.8 MB of state (delay lines): on Daisy, place it in SDRAM.
// ---------------------------------------------------------------------------
class Processor {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        drive.init(sr); chop.init(sr); thicken.init(sr); flavour.init(sr);
        gCtl.init(1.f, sr); gThk.init(0.5f, sr); gFlv.init(0.5f, sr); gOut.init(1.f, sr); gMix.init(0.6f, sr);
        setParams(p);
    }

    void setParams(const Params& np) {
        p = np;
        loCut1.set(expMap(p.loCut, 20.f, 600.f), 0.54f, sr);   // 4th-order Butterworth pair
        loCut2.set(expMap(p.loCut, 20.f, 600.f), 1.31f, sr);
        hiCut1.set(expMap(p.hiCut, 2000.f, 20000.f), 0.54f, sr);
        hiCut2.set(expMap(p.hiCut, 2000.f, 20000.f), 1.31f, sr);
        drive.set(p.drive, p.brightness);
        chop.set(p.chop, p.release);
        thicken.set(p); flavour.set(p);
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
            x = lerp(x, drive.process(x), gm);
            x = chop.process(x) * gc;
        }
        // THICKEN + FLAVOUR (parallel, fed by the control output)
        float tL = 0, tR = 0, fL = 0, fR = 0;
        float gt = gThk.next(), gf = gFlv.next();
        if (gt > 1e-4f) thicken.process(x, tL, tR);
        if (gf > 1e-4f) flavour.process(x, x, fL, fR);
        float dry = p.solo ? 0.f : x;
        float go = gOut.next();
        outL = safetyClip((dry + gt * tL + gf * fL) * go);
        outR = safetyClip((dry + gt * tR + gf * fR) * go);
    }

    float gainReductionDb() const { return chop.gainReductionDb(); }

private:
    float sr = 48000.f;
    Params p;
    SVF loCut1, loCut2, hiCut1, hiCut2;
    Drive drive;
    Chop chop;
    Thicken thicken;
    Flavour flavour;
    Smoothed gCtl, gThk, gFlv, gOut, gMix;
};

} // namespace rasp
