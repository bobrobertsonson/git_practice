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

// Measured targets: learn_target rows from real DIs, averaged. Listed after
// the models in the Target menu. The DIs stay on disk (samples/), never in git;
// names describe the sound, never the source.
struct Measured {
    const char* name;
    bool bass;
    float profile[8];
    const char* source;   // what it was measured from (no names)
};
constexpr Measured kMeasured[] = {
    // From 61 DIs out of 12 professional metal sessions (samples/, not in git):
    // learn_target per file, averaged per session, then across the sessions
    // of each group, so a session with six rhythm tracks counts once.
    {"Dark Rhythm", false, {1.70f, 0.97f, -7.50f, -15.09f, -24.55f, -32.04f, -36.80f, -45.92f}, "13 rhythm DIs from 4 pro sessions, drop tunings"},   // widest band spread between sessions 7.2 dB
    {"Modern Rhythm", false, {0.77f, 1.09f, -3.87f, -9.26f, -17.02f, -26.62f, -33.66f, -43.04f}, "15 rhythm DIs from 4 pro sessions"},   // widest band spread between sessions 8.7 dB
    {"Bright Rhythm", false, {2.72f, -1.70f, -4.05f, -4.37f, -1.65f, -5.94f, -14.91f, -33.34f}, "10 rhythm DIs from 2 pro sessions, very bright (likely active pickups)"},   // widest band spread between sessions 6.9 dB
    {"Lead", false, {-3.04f, 1.12f, -0.79f, -5.20f, -14.32f, -27.52f, -35.66f, -45.97f}, "9 lead and solo DIs from 3 pro sessions"},   // widest band spread between sessions 7.0 dB
    {"Clean", false, {2.21f, 0.72f, -8.74f, -14.85f, -18.18f, -26.67f, -41.64f, -55.27f}, "4 clean DIs from 1 pro session"},   // widest band spread between sessions 0.0 dB
    {"Punchy Bass", true, {2.82f, -1.92f, -5.73f, -7.38f, -10.24f, -16.32f, -26.48f, -37.19f}, "5 bass DIs from 5 pro sessions"},   // widest band spread between sessions 14.4 dB
    {"Deep Bass", true, {4.12f, -4.61f, -14.00f, -16.17f, -17.65f, -24.23f, -31.04f, -39.34f}, "3 bass DIs from 3 pro sessions"},   // widest band spread between sessions 15.7 dB
};
constexpr int kNumMeasured = (int)(sizeof(kMeasured) / sizeof(kMeasured[0]));

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
