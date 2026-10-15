// The Tone Match preset targets, as models: each is a set of strings through
// a pickup resonance, playing a standard phrase. make_targets runs Tone
// Match's own Learner over each and writes dsp/axe_targets.h; engine_test
// re-runs it to check that table is current.
//
// These are modelled, not measured from anyone's gear: a magnetic pickup is
// mostly a resonant lowpass (its inductance against the cable), so pickup
// types differ mainly in where that resonance sits and how sharp it is.
// Measured targets from real DIs can replace or join them later (learn_target).
#pragma once
#include "strings.h"
#include "../dsp/axe_base.h"

namespace targets {

struct Model {
    const char* name;
    bool bass;
    float bright;     // string brightness, 0 dull .. 1 fresh
    float resHz, resQ; // pickup resonance
};

// Order = the Target menu after "My Target". Names describe a kind of pickup
// or string, never a product.
constexpr Model kModels[] = {
    {"Single Coil",       false, 0.75f, 5200.f, 2.6f},   // bright, glassy: high, sharp resonance
    {"Vintage Humbucker", false, 0.60f, 3400.f, 2.0f},
    {"Hot Humbucker",     false, 0.60f, 2300.f, 1.6f},   // more turns: lower, darker, thicker mids
    {"Active Pickups",    false, 0.70f, 7500.f, 0.9f},   // buffered: flat and extended
    {"Warm Neck",         false, 0.35f, 2000.f, 1.2f},   // neck position, older strings
    {"Active Bass",       true,  0.75f, 8000.f, 0.8f},
    {"Passive Bass",      true,  0.55f, 2600.f, 1.4f},
    {"Flatwound Bass",    true,  0.10f, 1600.f, 1.0f},   // flatwounds: thump, little top
};
constexpr int kNum = (int)(sizeof(kModels) / sizeof(kModels[0]));

// The phrase each model plays: chords, single notes and palm mutes on guitar;
// a line, octaves and a held note on bass.
inline std::vector<float> render(const Model& m) {
    using namespace strings;
    std::vector<Note> ns;
    float t = 0.2f;
    if (!m.bass) {
        for (int rep = 0; rep < 2; rep++) {
            chord(ns, t, {40, 47, 52, 56, 59, 64}, 1.2f); t += 1.3f;   // E
            chord(ns, t, {45, 52, 57, 60, 64}, 1.2f); t += 1.3f;       // Am
            for (int i = 0; i < 8; i++) { ns.push_back({t, midiHz(40), 0.1f, 0.85f, 0.85f}); t += 0.12f; }   // chugs
            chord(ns, t, {40, 47, 52}, 0.8f, 0.95f); t += 0.9f;        // E5
            for (int mm : {52, 55, 57, 59, 62, 64, 67, 69}) { ns.push_back({t, midiHz(mm), 0.3f, 0.8f}); t += 0.32f; }
            ns.push_back({t, midiHz(64), 1.2f, 0.85f, 0.f, 0.f, 0.25f}); t += 1.4f;
        }
    } else {
        for (int rep = 0; rep < 2; rep++) {
            for (int mm : {28, 28, 31, 33, 28, 40, 35, 33}) { ns.push_back({t, midiHz(mm), 0.4f, 0.85f}); t += 0.45f; }
            for (int i = 0; i < 6; i++) { ns.push_back({t, midiHz(28), 0.18f, 0.8f, 0.8f}); t += 0.2f; }
            chord(ns, t, {33, 45}, 1.2f); t += 1.4f;
            ns.push_back({t, midiHz(28), 1.8f, 0.9f}); t += 2.0f;
        }
    }
    const StringSet s{m.bright, m.bass ? 3.5f : 3.0f, m.bass};
    std::vector<float> out((size_t)((t + 0.5f) * kSR), 0.f);
    Rng rng;
    for (const auto& n : ns) pluck(out, n, s, rng);
    pickup(out, m.bass, 0.35f, m.resHz, m.resQ);
    return out;
}

// What Tone Match's Learner hears from a signal: dB per band re 250 Hz-1 kHz.
inline void profile(const std::vector<float>& x, float sr, float* out) {
    axe::Learner l;
    l.init(sr);
    l.setSeconds(1e6f);
    l.start();
    for (float v : x) l.feed(v, out);
    l.result(out);
}

}  // namespace targets
