// RASP factory presets. The same set lives in playground/index.html (PRESETS);
// `make check` verifies the two match.
#pragma once
#include "rasp.h"
#include <cstring>

namespace rasp {

constexpr int kNumPresets = 7;
inline const char* presetName(int i) {
    static const char* names[kNumPresets] = {"huge", "natural", "dark", "bright", "live", "raw", "air"};
    return (i >= 0 && i < kNumPresets) ? names[i] : "";
}

// Defaults plus each preset's changes.
inline Params presetParams(int i) {
    Params p;
    switch (i) {
        case 0:  // huge: album sound
            p.drive = 0.45f; p.chop = 0.75f; p.spread = 0.75f; p.thickenGainDb = 3.f; p.tail = 0.45f;
            p.tailBlend = 0.45f; p.delayBlend = 0.22f; p.flavourGainDb = -4.f; p.glue = 0.65f; p.outDb = -3.f;
            // Top end fitted to four producers' lead vocals (tools/presetfit.py:
            // Bloodbath, Gojira, Immortal Disfigurement, KnockedLoose). Every one
            // kept more air than the old 12.6 kHz hi cut; average tone error
            // 4.1 -> 2.7 dB, Gojira 4.6 -> 0.5 dB.
            p.hiCut = 0.95f; p.airDb = 4.f;
            // "Still thin": KnockedLoose (the one mix whose band leaves 250 Hz
            // measurable) had 4.1 dB more 250 Hz on the lead than Huge; +5 dB
            // body brings it within 1 dB. Doubles +3 dB: preferred blind as
            // "fuller" / "larger" (abtest).
            p.bodyDb = 5.f;
            // Output -3 dB (was +3): on Andrew's DI, 29 Mantra settings left a
            // median 4.0 dB of phrase-to-phrase level (in/out slope 0.35); Huge
            // left 2.3 dB (0.09), mostly from driving the bus limiter. -6 dB into
            // the limiter costs only 0.9 LU of loudness (results/drywet).
        case 1:  // natural: the defaults
            break;
        case 2:  // dark: gutturals and lows
            p.brightness = 0; p.drive = 0.5f; p.loCut = 0.36f; p.hiCut = 0.72f; p.darken = 0.5f; p.tilt = 0.4f; p.tail = 0.4f;
            break;
        case 3:  // bright: highs and squeals
            p.brightness = 2; p.drive = 0.35f; p.loCut = 0.58f; p.hiCut = 0.95f; p.darken = 0.1f; p.tilt = 0.62f; p.deEss = 0.65f;
            break;
        case 4:  // live: tighter effects, more duck, bleed reducer higher
            p.tail = 0.35f; p.tailBlend = 0.25f; p.repeats = 0.2f; p.delayBlend = 0.15f; p.duck = 0.75f;
            p.bleed = 0.78f; p.loCut = 0.56f; p.glue = 0.4f;
            break;
        case 5:  // raw: filters and light compression only
            p.mix = 0.f; p.chop = 0.35f; p.thickenOn = false; p.flavourOn = false; p.glue = 0.f;
            break;
        case 6:  // air: huge with a very bright modern-deathcore top (Immortal
                 // Disfigurement lifts 6-10 kHz ~13 dB). The bright voicing + 10 dB
                 // version was picked against blind as "extremely harsh", so it
                 // keeps the natural voicing with a 7 dB shelf and more de-essing.
            p = presetParams(0);
            p.hiCut = 1.f; p.airDb = 5.f; p.deEss = 0.75f;   // 7 -> 5: Mantra on Andrew's DI cut 16 kHz (median -5.6 dB)
            break;
        default: break;
    }
    return p;
}

inline bool presetByName(const char* name, Params& out) {
    for (int i = 0; i < kNumPresets; i++)
        if (!std::strcmp(name, presetName(i))) { out = presetParams(i); return true; }
    return false;
}

} // namespace rasp
