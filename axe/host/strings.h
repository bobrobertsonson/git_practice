// String and pickup models shared by synth_di (test DIs) and make_targets
// (the Tone Match preset targets). See synth_di.cpp for what they model.
#pragma once
#include "common.h"
#include <cmath>
#include <initializer_list>
#include <vector>

namespace strings {

struct Rng {
    uint32_t s = 0x12345678u;
    float next() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFFFF) / 8388608.f - 1.f; }
};

constexpr float kSR = 48000.f;

struct Note {
    float t;          // start time (s)
    float hz;         // pitch
    float dur;        // until damped (s)
    float vel;        // 0..1
    float mute = 0.f; // palm mute 0..1
    float bend = 0.f; // semitones reached at the end of the note
    float vib = 0.f;  // vibrato depth (semitones)
};

struct StringSet {
    float bright;     // 0 dull .. 1 fresh
    float t60;        // open-string decay (s) at 110 Hz
    bool bass;
};

// Renders one note into out (added).
inline void pluck(std::vector<float>& out, const Note& n, const StringSet& s, Rng& rng) {
    const int start = (int)(n.t * kSR);
    const int len = (int)((n.dur + 0.4f) * kSR);
    const int maxD = 4096;
    std::vector<float> dl(maxD, 0.f);
    float period = kSR / n.hz;
    // excitation: the string's initial shape, a triangle peaking at the pick
    // position (harmonics fall ~12 dB/octave, with the pick-position nulls),
    // softened by pick hardness, plus a little pick noise
    const int exLen = (int)period;
    const float pos = s.bass ? 0.2f : 0.15f;
    float hard = (0.15f + 0.5f * n.vel) * (0.15f + 0.85f * s.bright) * (1.f - 0.6f * n.mute);
    std::vector<float> ex(exLen);
    float lp = 0.f, mean = 0.f;
    for (int i = 0; i < exLen; i++) {
        float u = (float)i / (float)exLen;
        float tri = u < pos ? u / pos : (1.f - u) / (1.f - pos);
        float v = tri + 0.04f * rng.next();
        lp += hard * (v - lp);
        ex[i] = lp; mean += lp;
    }
    mean /= (float)exLen;
    for (int i = 0; i < exLen; i++) dl[i] = (ex[i] - mean) * n.vel;
    // loss filter: a one-pole lowpass in the loop; dull strings lose their
    // highs much faster per period, and decay sooner overall
    float pole = 0.05f + 0.75f * (1.f - s.bright);
    pole += n.mute * (0.85f - pole);
    float t60 = s.t60 * (110.f / n.hz) * 0.6f + s.t60 * 0.4f;
    t60 *= (1.f - 0.93f * n.mute);
    float g = std::pow(10.f, -3.f / (t60 * n.hz));
    // the lowpass also loses a little at the fundamental: compensate
    {
        float w0 = 2.f * axe::kPi * n.hz / kSR;
        float mag = (1.f - pole) / std::sqrt(1.f - 2.f * pole * std::cos(w0) + pole * pole);
        g /= mag;
    }
    int w = exLen % maxD;
    float x1 = 0.f, damp = 1.f;
    for (int i = 0; i < len; i++) {
        float tn = (float)i / kSR;
        float semis = n.bend * std::fmin(tn / std::fmax(n.dur * 0.5f, 0.05f), 1.f) + n.vib * std::sin(2.f * axe::kPi * 5.5f * tn) * std::fmin(tn / 0.4f, 1.f);
        float d = period * std::pow(2.f, -semis / 12.f) - pole / (1.f - pole);   // minus the loop filter delay
        // read d samples back (linear interpolation)
        float rp = (float)w - d;
        while (rp < 0) rp += maxD;
        int i0 = (int)rp; float fr = rp - i0;
        float x = dl[i0 % maxD] * (1.f - fr) + dl[(i0 + 1) % maxD] * fr;
        x1 = x + pole * (x1 - x);
        float y = g * x1;
        if (tn > n.dur) damp *= 0.9985f;     // fret hand releases: fast damping
        y *= (tn > n.dur) ? 0.995f : 1.f;
        dl[w] = y;
        w = (w + 1) % maxD;
        size_t o = (size_t)(start + i);
        if (o < out.size()) out[o] += y * damp;
    }
}

// Magnetic pickup + cable: resonant lowpass (the pickup's inductance against
// the cable capacitance), and a DC block. 0 = this set's defaults.
inline void pickup(std::vector<float>& x, bool bass, float level, float resHz = 0.f, float resQ = 0.f) {
    axe::Biquad res, hp;
    res.lowpass(resHz > 0.f ? resHz : (bass ? 2200.f : 3800.f), resQ > 0.f ? resQ : (bass ? 1.2f : 2.2f), kSR);
    hp.highpass(bass ? 25.f : 50.f, 0.7071f, kSR);
    float peak = 1e-9f;
    for (auto& v : x) { v = hp.process(res.process(v)); peak = std::fmax(peak, std::fabs(v)); }
    float g = level / peak;
    for (auto& v : x) v *= g;
}

inline float midiHz(int m) { return 440.f * std::pow(2.f, (m - 69) / 12.f); }

// Strum a chord: MIDI notes, low to high, 12 ms between strings.
inline void chord(std::vector<Note>& ns, float t, std::initializer_list<int> notes, float dur, float vel = 0.8f, float mute = 0.f, bool up = false) {
    int i = 0, n = (int)notes.size();
    for (int m : notes) {
        int k = up ? (n - 1 - i) : i;
        ns.push_back({t + 0.012f * k, midiHz(m), dur, vel * (0.85f + 0.15f * ((k * 7) % 3) / 2.f), mute});
        i++;
    }
}


}  // namespace strings
