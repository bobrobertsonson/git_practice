// RASP - extreme vocal processor DSP core.
// Header-only, no allocations, no dependencies: runs the same on a desktop
// (for testing) and on a Cortex-M7 (Daisy Seed / Teensy 4.x).
//
// Signal chain (per sample):
//   in -> HPF -> gate -> envelope follower
//      -> [fry layer]   subharmonic (octave-down flip-flop) + jittered rattle AM
//      -> [throat]      2 resonant band-passes (vowel/formant) + low-mid push
//      -> [grit]        2 saturation stages (asymmetric tanh -> wavefolder), 2x oversampled
//      -> [air]         envelope-shaped band-passed noise ("breath / false-chord hiss")
//      -> tone tilt -> compressor -> dry/wet -> out
#pragma once
#include <cmath>
#include <cstdint>

namespace rasp {

constexpr float kPi = 3.14159265358979f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float dbToLin(float db) { return std::pow(10.f, db / 20.f); }

// Cheap tanh approximation (accurate enough for saturation, much faster on MCU).
inline float fastTanh(float x) {
    x = clampf(x, -3.f, 3.f);
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

// One-pole smoother / lowpass.
struct OnePole {
    float a = 0.f, z = 0.f;
    void setCutoff(float hz, float sr) { a = std::exp(-2.f * kPi * hz / sr); }
    float process(float x) { z = x + a * (z - x); return z; }
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

// Peak envelope follower with separate attack/release.
struct Envelope {
    float att = 0, rel = 0, env = 0;
    void set(float attMs, float relMs, float sr) {
        att = std::exp(-1.f / (attMs * 0.001f * sr));
        rel = std::exp(-1.f / (relMs * 0.001f * sr));
    }
    float process(float x) {
        float a = std::fabs(x);
        float c = a > env ? att : rel;
        env = a + c * (env - a);
        return env;
    }
};

// xorshift white noise in [-1, 1].
struct Noise {
    uint32_t s = 0x9E3779B9u;
    float next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(int32_t)s * (1.f / 2147483648.f);
    }
};

// User-facing controls (all 0..1 unless noted). Map these to knobs.
struct Params {
    float drive    = 0.6f;  // grit: saturation amount
    float fry      = 0.5f;  // fry/rattle: subharmonic + AM growl
    float rattleHz = 45.f;  // growl modulation rate, Hz (20..90)
    float throat   = 0.5f;  // formant resonance strength
    float vowel    = 0.3f;  // 0 = "oo" (dark/guttural) .. 1 = "ee" (bright/shriek)
    float air      = 0.3f;  // breath / hiss layer
    float tone     = 0.5f;  // tilt EQ: 0 dark .. 1 bright
    float mix      = 1.0f;  // dry/wet
    float gateDb   = -50.f; // gate threshold, dBFS
    float outDb    = 0.f;   // output trim
    int   mode     = 0;     // 0 = Fry, 1 = Growl (guttural), 2 = Pig squeal / high
};

class Processor {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        hpf.set(90.f, 0.707f, sr);
        inEnv.set(1.f, 120.f, sr);
        gateEnv.setCutoff(30.f, sr);
        compEnv.set(3.f, 80.f, sr);
        subLp.set(400.f, 0.707f, sr);
        noiseBp.set(3500.f, 0.8f, sr);
        jitter.setCutoff(8.f, sr);
        // Oversampling half-band-ish filters (run at 2*sr).
        upLp.set(sr * 0.45f, 0.707f, sr * 2.f);
        downLp.set(sr * 0.45f, 0.707f, sr * 2.f);
        dcBlock.setCutoff(15.f, sr);
        setParams(p);
    }

    void setParams(const Params& np) {
        p = np;
        // Mode presets bias the formants and the rattle.
        float f1, f2;
        switch (p.mode) {
            case 1:  f1 = lerp(250.f, 450.f, p.vowel); f2 = lerp(600.f, 1200.f, p.vowel); break; // growl
            case 2:  f1 = lerp(900.f, 1600.f, p.vowel); f2 = lerp(2500.f, 4200.f, p.vowel); break; // squeal
            default: f1 = lerp(350.f, 800.f, p.vowel); f2 = lerp(900.f, 2600.f, p.vowel); break; // fry
        }
        float q = 1.5f + p.throat * 8.f;
        form1.set(f1, q, sr);
        form2.set(f2, q * 0.8f, sr);
        lowMid.set(p.mode == 1 ? 180.f : 300.f, 0.9f, sr);
        tiltLp.set(lerp(1500.f, 6000.f, p.tone), 0.707f, sr);
        gateThresh = dbToLin(p.gateDb);
        outGain = dbToLin(p.outDb);
    }

    float process(float in) {
        // 1. Clean-up + dynamics sensing.
        hpf.process(in);
        float x = hpf.hp;
        float env = inEnv.process(x);
        float gate = gateEnv.process(env > gateThresh ? 1.f : 0.f);
        // Normalised "how hard is the singer pushing" 0..1 - drives intensity.
        float push = clampf(env * 4.f, 0.f, 1.f);

        // 2. Fry layer: octave-down flip-flop (false-chord subharmonic)...
        if ((x > 0.f) != (lastSign > 0.f) && x > 0.f) subFlip = -subFlip;
        lastSign = x;
        subLp.process(x * subFlip);
        float sub = subLp.lp;
        // ...plus jittered amplitude modulation = rattle/growl.
        float rate = p.rattleHz * clampf(1.f + 7.f * jitter.process(noise.next()), 0.5f, 1.5f);
        phase += rate / sr;
        if (phase >= 1.f) phase -= 1.f;
        float lfo = 0.5f + 0.5f * std::sin(2.f * kPi * phase);
        float amDepth = p.fry * (p.mode == 1 ? 0.9f : 0.6f);
        float fried = x * (1.f - amDepth * lfo) + sub * p.fry * (p.mode == 2 ? 0.3f : 1.2f);

        // 3. Throat: formants + low-mid chest weight.
        form1.process(fried); form2.process(fried); lowMid.process(fried);
        float throated = fried + p.throat * (2.5f * form1.bp + 1.8f * form2.bp + 0.8f * lowMid.bp);

        // 4. Grit: 2 stages of saturation at 2x oversampling.
        float pre = 1.f + p.drive * 30.f * (0.6f + 0.4f * push);
        float g0 = saturate2x(throated * pre);
        g0 = dcBlockProcess(g0);

        // 5. Air: breath/hiss that follows the voice.
        noiseBp.process(noise.next());
        float air = noiseBp.bp * env * p.air * 6.f;

        float wet = g0 * 0.5f + air;

        // 6. Tone tilt: crossfade between lowpassed and full signal.
        tiltLp.process(wet);
        wet = lerp(tiltLp.lp, wet, p.tone);

        // 7. Glue compressor (fixed ratio ~4:1, threshold -18 dB).
        float ce = compEnv.process(wet);
        float gr = ce > 0.125f ? std::pow(0.125f / ce, 0.75f) : 1.f;
        wet *= gr * 1.6f;

        float out = (lerp(in, wet, p.mix)) * gate * outGain;
        return fastTanh(out); // final safety clipper
    }

private:
    static float lerp(float a, float b, float t) { return a + (b - a) * t; }

    float stage(float x) {
        // Stage A: asymmetric tube-ish clip (even harmonics = "throat" crunch).
        float a = fastTanh(x + 0.25f) - fastTanh(0.25f);
        // Stage B: sine wavefolder (odd, fizzy, "torn" upper harmonics).
        float fold = std::sin(a * (1.f + p.drive * 2.5f) * kPi * 0.5f);
        return lerp(a, fold, 0.35f + 0.4f * p.drive);
    }

    float saturate2x(float x) {
        // Zero-stuff upsample -> LPF -> nonlinearity -> LPF -> decimate.
        upLp.process(x * 2.f);  float a = upLp.lp;
        upLp.process(0.f);      float b = upLp.lp;
        downLp.process(stage(a));
        downLp.process(stage(b));
        return downLp.lp;
    }

    float dcBlockProcess(float x) { return x - dcBlock.process(x); }

    float sr = 48000.f;
    Params p;
    SVF hpf, subLp, form1, form2, lowMid, noiseBp, tiltLp, upLp, downLp;
    Envelope inEnv, compEnv;
    OnePole gateEnv, jitter, dcBlock;
    Noise noise;
    float lastSign = 0.f, subFlip = 1.f, phase = 0.f;
    float gateThresh = 0.f, outGain = 1.f;
};

} // namespace rasp
