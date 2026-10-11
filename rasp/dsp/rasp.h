// RASP - extreme vocal processor DSP core.
// Header-only C++17, no heap, no dependencies: same code on desktop and on a
// Cortex-M7 (Daisy Seed). Zero lookahead - every block is causal, so the
// effect adds no buffering latency of its own (the hardware's audio block
// size and converters are the only latency).
//
// Layout mirrors a "screamer" style plugin:
//
//   in ─► CONTROL ──────────────► THICKEN ──────────► FLAVOUR ─────────► out (stereo)
//         lo/hi cut                spread (doubles)    tail + blend (verb)
//         drive (+ type switch)    diffuse             repeats + blend (delay, 3 times)
//         mix                      darken, width       tilt
//         chop (gate) + release    solo                duck
//         gain                     gain                gain
//
//   "drive" is the scream engine: sub-octave fry + rattle AM, formant throat,
//   2x oversampled two-stage saturation, breath noise, glue compressor.
#pragma once
#include <cmath>
#include <cstdint>

namespace rasp {

constexpr float kPi = 3.14159265358979f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float dbToLin(float db) { return std::pow(10.f, db / 20.f); }
inline float expMap(float t, float lo, float hi) { return lo * std::pow(hi / lo, t); }

// Cheap tanh approximation (good for saturation, fast on MCU).
inline float fastTanh(float x) {
    x = clampf(x, -3.f, 3.f);
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

struct OnePole {
    float a = 0.f, z = 0.f;
    void setCutoff(float hz, float sr) { a = std::exp(-2.f * kPi * hz / sr); }
    float process(float x) { z = x + a * (z - x); return z; }
};

// Smooths a control value per sample to avoid zipper noise (~10 ms).
struct Smoothed {
    float a = 0.f, cur = 0.f, target = 0.f;
    void init(float v, float sr) { a = std::exp(-1.f / (0.01f * sr)); cur = target = v; }
    float next() { cur = target + a * (cur - target); return cur; }
};

// TPT state-variable filter (Zavalishin) - stable under modulation.
struct SVF {
    float g = 0, k = 1, ic1 = 0, ic2 = 0;
    float lp = 0, bp = 0, hp = 0;
    void set(float hz, float q, float sr) {
        hz = clampf(hz, 10.f, sr * 0.45f);
        g = std::tan(kPi * hz / sr);
        k = 1.f / q;
    }
    void process(float x) {
        float v3 = x - ic2;
        float v1 = (g * v3 + ic1) / (1.f + g * (g + k));
        float v2 = ic2 + g * v1;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
};

struct Envelope {
    float att = 0, rel = 0, env = 0;
    void set(float attMs, float relMs, float sr) {
        att = std::exp(-1.f / (attMs * 0.001f * sr));
        rel = std::exp(-1.f / (relMs * 0.001f * sr));
    }
    float process(float x) {
        float a = std::fabs(x);
        env = a + (a > env ? att : rel) * (env - a);
        return env;
    }
};

struct Noise {
    uint32_t s = 0x9E3779B9u;
    float next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(int32_t)s * (1.f / 2147483648.f);
    }
};

// Power-of-two circular delay line with fractional (linear) read.
template <int N>
struct DelayLine {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    float buf[N] = {};
    int w = 0;
    void write(float x) { buf[w] = x; w = (w + 1) & (N - 1); }
    float read(float d) const {  // d = samples behind the last write (>= 1)
        float rp = (float)w - d;
        int i = (int)std::floor(rp);
        float f = rp - (float)i;
        float a = buf[i & (N - 1)], b = buf[(i + 1) & (N - 1)];
        return a + (b - a) * f;
    }
};

// Schroeder allpass with fixed length.
template <int N>
struct Allpass {
    float buf[N] = {};
    int len = N, i = 0;
    void setLen(int n) { len = n < 1 ? 1 : (n > N ? N : n); }
    float process(float x, float g) {
        float d = buf[i];
        float y = d - g * x;
        buf[i] = x + g * y;
        if (++i >= len) i = 0;
        return y;
    }
};

// Lowpass-feedback comb (Freeverb style).
template <int N>
struct Comb {
    float buf[N] = {};
    int len = N, i = 0;
    void setLen(int n) { len = n < 1 ? 1 : (n > N ? N : n); }
    float z = 0;
    float process(float x, float fb, float damp) {
        float y = buf[i];
        z = y + damp * (z - y);
        buf[i] = x + z * fb;
        if (++i >= len) i = 0;
        return y;
    }
};

// ---------------------------------------------------------------------------
// Parameters. Knob values are 0..1 unless noted.
// ---------------------------------------------------------------------------
struct Params {
    // CONTROL
    bool  controlOn = true;
    float loCut     = 0.25f;  // 20..500 Hz
    float hiCut     = 0.80f;  // 2k..20k Hz
    float drive     = 0.60f;
    int   driveType = 0;      // 3-way: 0 Fry, 1 Growl (guttural), 2 Squeal (high)
    float mix       = 1.00f;  // dry/wet of the drive
    float chop      = 0.50f;  // gate threshold: 0 = off, 0.5 = -50 dBFS, 1 = -20 dBFS
    float release   = 0.30f;  // gate release 10..800 ms
    float controlGainDb = 0.f;

    // Scream engine detail (on hardware: shift-layer / menu)
    float fry       = 0.5f;
    float rattleHz  = 45.f;   // 20..90
    float throat    = 0.5f;
    float vowel     = 0.3f;   // dark "oo" .. bright "ee"
    float air       = 0.25f;

    // THICKEN
    bool  thickenOn = true;
    float spread    = 0.5f;   // doubled-voice level + detune
    float diffuse   = 0.3f;   // smear the doubles
    float darken    = 0.3f;   // lowpass on the doubles
    float width     = 0.7f;   // stereo spread of the doubles
    bool  solo      = false;  // hear only the doubles
    float thickenGainDb = 0.f;

    // FLAVOUR
    bool  flavourOn = true;
    float tail      = 0.5f;   // reverb decay
    float tailBlend = 0.2f;   // reverb level
    float repeats   = 0.3f;   // delay feedback
    float delayBlend= 0.15f;  // delay level
    int   delayTime = 1;      // 3-way: 0 slap (110 ms), 1 eighth, 2 quarter
    float bpm       = 120.f;  // for delayTime 1/2 (tap tempo on hardware)
    float tilt      = 0.5f;   // 0 dark .. 0.5 flat .. 1 bright (+-6 dB)
    float duck      = 0.5f;   // duck verb/delay while singing
    float flavourGainDb = 0.f;

    float outDb     = 0.f;
};

// ---------------------------------------------------------------------------
// The scream engine (the "drive" knob's engine).
// ---------------------------------------------------------------------------
class ScreamEngine {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        inEnv.set(1.f, 120.f, sr);
        compEnv.set(3.f, 80.f, sr);
        subLp.set(400.f, 0.707f, sr);
        noiseBp.set(3500.f, 0.8f, sr);
        jitter.setCutoff(8.f, sr);
        upLp.set(sr * 0.45f, 0.707f, sr * 2.f);
        downLp.set(sr * 0.45f, 0.707f, sr * 2.f);
        dcBlock.setCutoff(15.f, sr);
    }

    void set(const Params& np) {
        p = np;
        float f1, f2;
        switch (p.driveType) {
            case 1:  f1 = lerp(250.f, 450.f, p.vowel);  f2 = lerp(600.f, 1200.f, p.vowel);  break;
            case 2:  f1 = lerp(900.f, 1600.f, p.vowel); f2 = lerp(2500.f, 4200.f, p.vowel); break;
            default: f1 = lerp(350.f, 800.f, p.vowel);  f2 = lerp(900.f, 2600.f, p.vowel);  break;
        }
        float q = 1.5f + p.throat * 8.f;
        form1.set(f1, q, sr);
        form2.set(f2, q * 0.8f, sr);
        lowMid.set(p.driveType == 1 ? 180.f : 300.f, 0.9f, sr);
    }

    float process(float x) {
        float env = inEnv.process(x);
        float push = clampf(env * 4.f, 0.f, 1.f);  // how hard the singer is pushing

        // Fry: octave-down flip-flop (false-chord sub) + jittered rattle AM.
        if (x > 0.f && lastX <= 0.f) subFlip = -subFlip;
        lastX = x;
        subLp.process(x * subFlip);
        float rate = p.rattleHz * clampf(1.f + 7.f * jitter.process(noise.next()), 0.5f, 1.5f);
        phase += rate / sr;
        if (phase >= 1.f) phase -= 1.f;
        float lfo = 0.5f + 0.5f * std::sin(2.f * kPi * phase);
        float amDepth = p.fry * (p.driveType == 1 ? 0.9f : 0.6f);
        float fried = x * (1.f - amDepth * lfo) + subLp.lp * p.fry * (p.driveType == 2 ? 0.3f : 1.2f);

        // Throat: formants + chest weight.
        form1.process(fried); form2.process(fried); lowMid.process(fried);
        float throated = fried + p.throat * (2.5f * form1.bp + 1.8f * form2.bp + 0.8f * lowMid.bp);

        // Grit: envelope-driven, 2x oversampled.
        float pre = 1.f + p.drive * 30.f * (0.6f + 0.4f * push);
        float g = saturate2x(throated * pre);
        g -= dcBlock.process(g);

        // Air: breath noise following the voice.
        noiseBp.process(noise.next());
        float wet = g * 0.5f + noiseBp.bp * env * p.air * 6.f;

        // Glue compressor (~4:1 above -18 dBFS).
        float ce = compEnv.process(wet);
        float gr = ce > 0.125f ? std::pow(0.125f / ce, 0.75f) : 1.f;
        return wet * gr * 1.6f;
    }

private:
    float stage(float x) {
        float a = fastTanh(x + 0.25f) - fastTanh(0.25f);            // asymmetric: even harmonics
        float fold = std::sin(a * (1.f + p.drive * 2.5f) * kPi * 0.5f); // folder: torn, fizzy top
        return lerp(a, fold, 0.35f + 0.4f * p.drive);
    }
    float saturate2x(float x) {
        upLp.process(x * 2.f); float a = upLp.lp;
        upLp.process(0.f);     float b = upLp.lp;
        downLp.process(stage(a));
        downLp.process(stage(b));
        return downLp.lp;
    }

    float sr = 48000.f;
    Params p;
    SVF subLp, form1, form2, lowMid, noiseBp, upLp, downLp;
    Envelope inEnv, compEnv;
    OnePole jitter, dcBlock;
    Noise noise;
    float lastX = 0.f, subFlip = 1.f, phase = 0.f;
};

// ---------------------------------------------------------------------------
// Chop: noise gate with hysteresis + hold. The main anti-feedback tool live.
// ---------------------------------------------------------------------------
class Chop {
public:
    void init(float sampleRate) { sr = sampleRate; rmsCoef = std::exp(-1.f / (0.005f * sr)); }
    void set(float chop, float release) {
        enabled = chop > 0.001f;
        openThr = dbToLin(lerp(-80.f, -20.f, chop));
        closeThr = openThr * 0.6f;                         // ~4.4 dB hysteresis
        attCoef = std::exp(-1.f / (0.001f * sr));          // 1 ms open
        relCoef = std::exp(-1.f / (expMap(release, 10.f, 800.f) * 0.001f * sr));
        holdLen = (int)(0.04f * sr);                       // 40 ms hold
    }
    // Detect on `key` (pre-drive), return gain to apply.
    float process(float key) {
        if (!enabled) return 1.f;
        // 5 ms RMS detector: ignores single noise peaks that a peak detector would open on.
        float k2 = key * key;
        ms = k2 + rmsCoef * (ms - k2);
        float e = std::sqrt(ms);
        if (e > openThr) { open = true; hold = holdLen; }
        else if (e < closeThr) { if (hold > 0) --hold; else open = false; }
        float t = open ? 1.f : 0.f;
        gain = t + (t > gain ? attCoef : relCoef) * (gain - t);
        return gain;
    }
    bool isOpen() const { return open; }
private:
    float sr = 48000.f, openThr = 0, closeThr = 0, attCoef = 0, relCoef = 0, gain = 0;
    float rmsCoef = 0, ms = 0;
    int holdLen = 0, hold = 0;
    bool enabled = true, open = false;
};

// ---------------------------------------------------------------------------
// Thicken: two modulated doubles, diffused, darkened and spread in stereo.
// ---------------------------------------------------------------------------
class Thicken {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        float s = sr / 48000.f;
        apA1.setLen((int)(142 * s)); apA2.setLen((int)(379 * s));
        apB1.setLen((int)(107 * s)); apB2.setLen((int)(277 * s));
    }
    void set(const Params& p) {
        spread = p.spread; diffuse = p.diffuse; width = p.width; solo = p.solo;
        darkA.setCutoff(expMap(1.f - p.darken, 1200.f, 16000.f), sr);
        darkB.setCutoff(expMap(1.f - p.darken, 1200.f, 16000.f), sr);
    }
    void process(float m, float& L, float& R) {
        dl.write(m);
        phA += 0.63f / sr; if (phA >= 1.f) phA -= 1.f;
        phB += 0.91f / sr; if (phB >= 1.f) phB -= 1.f;
        float depth = 0.0004f + 0.0016f * spread;              // up to ~2 ms sweep = ~10-15 cents
        float vA = dl.read(sr * (0.012f + depth * std::sin(2.f * kPi * phA)));
        float vB = dl.read(sr * (0.019f + depth * std::sin(2.f * kPi * phB)));
        vA = lerp(vA, apA2.process(apA1.process(vA, 0.6f), 0.6f), diffuse);
        vB = lerp(vB, apB2.process(apB1.process(vB, 0.6f), 0.6f), diffuse);
        vA = darkA.process(vA); vB = darkB.process(vB);
        float lvl = spread * 0.8f;
        float wA = 0.5f + 0.5f * width, wB = 0.5f - 0.5f * width;
        float dryLvl = solo ? 0.f : 1.f;
        L = m * dryLvl + lvl * (vA * wA + vB * wB);
        R = m * dryLvl + lvl * (vB * wA + vA * wB);
    }
private:
    float sr = 48000.f, spread = 0, diffuse = 0, width = 0, phA = 0, phB = 0.37f;
    bool solo = false;
    DelayLine<4096> dl;                                        // 85 ms @ 48k, 42 ms @ 96k
    Allpass<512> apA1, apA2, apB1, apB2;
    OnePole darkA, darkB;
};

// ---------------------------------------------------------------------------
// Flavour: stereo delay + reverb, ducked by the voice, then tilt EQ.
// ---------------------------------------------------------------------------
class Flavour {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        float s = sr / 44100.f;
        const int cl[4] = {1116, 1188, 1277, 1356};
        for (int i = 0; i < 4; i++) { cL[i].setLen((int)(cl[i] * s)); cR[i].setLen((int)((cl[i] + 23) * s)); }
        aL[0].setLen((int)(556 * s)); aL[1].setLen((int)(441 * s));
        aR[0].setLen((int)(579 * s)); aR[1].setLen((int)(464 * s));
        fbHpL.setCutoff(200.f, sr); fbHpR.setCutoff(200.f, sr);
        fbLpL.setCutoff(4500.f, sr); fbLpR.setCutoff(4500.f, sr);
        duckEnv.set(5.f, 250.f, sr);
        tiltL.setCutoff(900.f, sr); tiltR.setCutoff(900.f, sr);
        verbPre.set(300.f, 0.707f, sr);
    }
    void set(const Params& p) {
        fbVerb = 0.70f + 0.28f * p.tail;
        repeats = p.repeats * 0.9f;
        tailBlend = p.tailBlend; delayBlend = p.delayBlend; duck = p.duck;
        float beat = 60.f / clampf(p.bpm, 40.f, 240.f);
        float t = p.delayTime == 0 ? 0.11f : (p.delayTime == 1 ? beat * 0.5f : beat);
        delaySamples = clampf(t * sr, 1.f, (float)(kDelayMax - 2));
        float db = (p.tilt - 0.5f) * 12.f;
        tiltLo = dbToLin(-db * 0.5f); tiltHi = dbToLin(db * 0.5f);
    }
    void process(float& L, float& R) {
        float d = duckEnv.process(0.5f * (L + R));
        float dg = 1.f - duck * clampf(d * 8.f, 0.f, 1.f);

        // Ping-pong delay with darkening repeats.
        float dL = dlL.read(delaySamples), dR = dlR.read(delaySamples);
        float fL = fbLpL.process(dR - fbHpL.process(dR));      // cross-feed = ping-pong
        float fR = fbLpR.process(dL - fbHpR.process(dL));
        dlL.write(L + fL * repeats);
        dlR.write(fR * repeats);

        // Reverb (Freeverb-lite), high-passed input so the low end stays clean.
        verbPre.process(0.5f * (L + R));
        float in = verbPre.hp * 0.03f;
        float rL = 0, rR = 0;
        for (int i = 0; i < 4; i++) { rL += cL[i].process(in, fbVerb, 0.35f); rR += cR[i].process(in, fbVerb, 0.35f); }
        for (int i = 0; i < 2; i++) { rL = aL[i].process(rL, 0.5f); rR = aR[i].process(rR, 0.5f); }

        L += dg * (delayBlend * dL + tailBlend * rL * 3.f);
        R += dg * (delayBlend * dR + tailBlend * rR * 3.f);

        float lo = tiltL.process(L); L = lo * tiltLo + (L - lo) * tiltHi;
        lo = tiltR.process(R);       R = lo * tiltLo + (R - lo) * tiltHi;
    }
private:
    static constexpr int kDelayMax = 1 << 17;                 // 2.7 s @ 48k
    float sr = 48000.f, fbVerb = 0.8f, repeats = 0, tailBlend = 0, delayBlend = 0, duck = 0;
    float delaySamples = 1000.f, tiltLo = 1.f, tiltHi = 1.f;
    DelayLine<kDelayMax> dlL, dlR;
    OnePole fbHpL, fbHpR, fbLpL, fbLpR, tiltL, tiltR;
    Comb<2048> cL[4], cR[4];
    Allpass<1024> aL[2], aR[2];
    Envelope duckEnv;
    SVF verbPre;
};

// ---------------------------------------------------------------------------
// The whole box. ~1.6 MB of state (delay lines): on Daisy, place it in SDRAM.
// ---------------------------------------------------------------------------
class Processor {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        scream.init(sr); chop.init(sr); thicken.init(sr); flavour.init(sr);
        gCtl.init(1.f, sr); gThk.init(1.f, sr); gFlv.init(1.f, sr); gOut.init(1.f, sr); gMix.init(1.f, sr);
        setParams(p);
    }

    void setParams(const Params& np) {
        p = np;
        loCut.set(expMap(p.loCut, 20.f, 500.f), 0.707f, sr);
        hiCut.set(expMap(p.hiCut, 2000.f, 20000.f), 0.707f, sr);
        scream.set(p); chop.set(p.chop, p.release); thicken.set(p); flavour.set(p);
        gCtl.target = dbToLin(p.controlGainDb);
        gThk.target = dbToLin(p.thickenGainDb);
        gFlv.target = dbToLin(p.flavourGainDb);
        gOut.target = dbToLin(p.outDb);
        gMix.target = p.mix;
    }

    void process(float in, float& outL, float& outR) {
        // CONTROL
        float x = in;
        float gc = gCtl.next(), gm = gMix.next();
        if (p.controlOn) {
            loCut.process(x); hiCut.process(loCut.hp); x = hiCut.lp;
            float gate = chop.process(x);                       // key on the clean signal
            float wet = scream.process(x);
            x = lerp(x, wet, gm) * gate * gc;
        }
        // THICKEN
        float L = x, R = x;
        float gt = gThk.next();
        if (p.thickenOn) { thicken.process(x, L, R); L *= gt; R *= gt; }
        // FLAVOUR
        float gf = gFlv.next();
        if (p.flavourOn) { flavour.process(L, R); L *= gf; R *= gf; }
        // OUT (soft safety clip)
        float go = gOut.next();
        outL = fastTanh(L * go);
        outR = fastTanh(R * go);
    }

    bool gateOpen() const { return chop.isOpen(); }

private:
    float sr = 48000.f;
    Params p;
    SVF loCut, hiCut;
    ScreamEngine scream;
    Chop chop;
    Thicken thicken;
    Flavour flavour;
    Smoothed gCtl, gThk, gFlv, gOut, gMix;
};

} // namespace rasp
