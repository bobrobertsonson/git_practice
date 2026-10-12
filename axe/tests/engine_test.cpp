// Engine unit tests: no audio files, no framework. `make check`.
//
//   bypass / null   bypass is bit-exact; mix 0 nulls (or is flat where the dry
//                   path is a crossover or the oversampling filters)
//   sample rates    44.1..192 kHz: stable, finite, same loudness as at 48 kHz
//   blocks          output does not depend on the host block size
//   smoothing       every parameter jump, mode and instrument switch and
//                   bypass toggle is click-free and without dropouts
//   reset           kx:Reset makes two instances bit-identical (dual mono)
//   sub             octave-down level, single notes and chords (vs WSOLA)
//   learn           fresh -> dull match recovers the difference
#include "../host/common.h"
#include <cmath>
#include <functional>
#include <string>

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using Sig = std::vector<float>;

struct Lcg { uint32_t s = 1; float next() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFFFF) / 8388608.f - 1.f; } };

// Guitar-ish test signal: plucked harmonic tones (decaying partials), several notes.
static Sig pluckSig(float sr, float seconds, std::initializer_list<float> hz, float amp = 0.3f, float every = 0.5f) {
    Sig x((size_t)(sr * seconds), 0.f);
    int k = 0;
    for (float t0 = 0.f; t0 < seconds; t0 += every, k++) {
        for (float f0 : hz) {
            for (int h = 1; h <= 12; h++) {
                float f = f0 * h * (k % 2 ? 1.f : 1.0005f);
                if (f > sr * 0.45f) break;
                float a = amp / (h * h * 0.5f + 0.5f) / (float)hz.size();
                float dec = 2.f + h * 1.5f;
                for (size_t i = (size_t)(t0 * sr); i < x.size(); i++) {
                    float t = (float)i / sr - t0;
                    if (t > 1.5f) break;
                    x[i] += a * std::exp(-dec * t) * std::sin(2.f * axe::kPi * f * t + h);
                }
            }
        }
    }
    return x;
}
static Sig sine(float sr, float seconds, float hz, float amp) {
    Sig x((size_t)(sr * seconds));
    for (size_t i = 0; i < x.size(); i++) x[i] = amp * std::sin(2.f * axe::kPi * hz * (float)i / sr);
    return x;
}
static double rmsDb(const Sig& x, size_t a = 0, size_t b = 0) {
    if (!b || b > x.size()) b = x.size();
    double s = 0; for (size_t i = a; i < b; i++) s += (double)x[i] * x[i];
    return 10.0 * std::log10(s / std::max<size_t>(1, b - a) + 1e-30);
}
static bool allFinite(const Sig& x) { for (float v : x) if (!std::isfinite(v)) return false; return true; }

static axe::Processor& procA() { static axe::Processor p; return p; }
static axe::Processor& procB() { static axe::Processor p; return p; }

static Sig run(axe::Processor& p, const Sig& in, int block = 128, std::function<void(size_t)> atBlock = nullptr) {
    Sig out(in.size());
    for (size_t i = 0; i < in.size(); i += block) {
        if (atBlock) atBlock(i);
        int n = (int)std::min<size_t>(block, in.size() - i);
        p.process(&in[i], &out[i], n);
    }
    return out;
}
static Sig render(const axe::Params& prm, const Sig& in, float sr, int block = 128) {
    auto& p = procA();
    p.init(sr); p.setParams(prm); p.reset();
    return run(p, in, block);
}

// Click detector: largest second difference in [a,b), relative to the largest
// in a reference window.
static float maxD2(const Sig& y, size_t a, size_t b) {
    float m = 0.f;
    for (size_t i = std::max<size_t>(a, 2); i < b && i < y.size(); i++) m = std::fmax(m, std::fabs(y[i] - 2.f * y[i - 1] + y[i - 2]));
    return m;
}

// Amplitude of frequency f in y[a,b) (single-bin DFT with a Hann window).
static double toneDb(const Sig& y, size_t a, size_t b, float f, float sr) {
    double re = 0, im = 0, wsum = 0;
    for (size_t i = a; i < b; i++) {
        double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * (double)(i - a) / (double)(b - a));
        double ph = 2.0 * M_PI * f * (double)i / sr;
        re += w * y[i] * std::cos(ph); im += w * y[i] * std::sin(ph); wsum += w;
    }
    return 20.0 * std::log10(2.0 * std::sqrt(re * re + im * im) / wsum + 1e-12);
}

static axe::Params modeParams(int mode, int inst = axe::kGuitar) {
    axe::Params p; p.mode = mode; p.instrument = inst; return p;
}

// ---------------------------------------------------------------------------
static void testBypassAndNull() {
    const float sr = 48000.f;
    Sig x = pluckSig(sr, 2.f, {82.4f, 123.5f, 164.8f});
    for (int m = 0; m < axe::kNumModes; m++) {
        for (int inst = 0; inst < 2; inst++) {
            axe::Params p = modeParams(m, inst);
            p.bypass = true;
            Sig y = render(p, x, sr);
            double maxErr = 0; for (size_t i = 0; i < x.size(); i++) maxErr = std::max(maxErr, (double)std::fabs(y[i] - x[i]));
            CHECK(maxErr == 0.0, "bypass not bit-exact: mode %s inst %d err %g", axe::modeName(m), inst, maxErr);
        }
    }
    // mix = 0 with unity gains: Sub and New Strings null exactly
    for (int m : {axe::kSub, axe::kStrings}) {
        axe::Params p = modeParams(m); p.mix = 0.f;
        Sig y = render(p, x, sr);
        double maxErr = 0; for (size_t i = 0; i < x.size(); i++) maxErr = std::max(maxErr, (double)std::fabs(y[i] - x[i]));
        CHECK(maxErr < 1e-6, "mix 0 does not null: %s err %g", axe::modeName(m), maxErr);
    }
    // Sub at sub 0 and dry 1 is also a null (the dry is untouched)
    { axe::Params p = modeParams(axe::kSub); p.sub = 0.f;
      Sig y = render(p, x, sr);
      double maxErr = 0; for (size_t i = 0; i < x.size(); i++) maxErr = std::max(maxErr, (double)std::fabs(y[i] - x[i]));
      CHECK(maxErr < 1e-6, "sub=0 does not null: err %g", maxErr); }
    // Tight (crossover) and Buzzsaw (oversampling filters) dry paths: flat magnitude
    for (int m : {axe::kTight, axe::kBuzzsaw}) {
        for (float f : {200.f, 500.f, 1000.f, 3000.f, 8000.f, 15000.f}) {
            axe::Params p = modeParams(m); p.mix = 0.f;
            Sig s = sine(sr, 1.f, f, 0.25f);
            Sig y = render(p, s, sr);
            double g = rmsDb(y, 24000, 48000) - rmsDb(s, 24000, 48000);
            CHECK(std::fabs(g) < 0.15, "%s dry path not flat at %.0f Hz: %+.2f dB", axe::modeName(m), f, g);
        }
    }
    // Tight with zero compression depth and flat band gains is that same
    // flat crossover sum (the bands recombine exactly)
    for (float f : {150.f, 1800.f, 5000.f}) {
        axe::Params p = modeParams(axe::kTight); p.tight = 0.f;
        Sig s = sine(sr, 1.f, f, 0.25f);
        Sig y = render(p, s, sr);
        double g = rmsDb(y, 24000, 48000) - rmsDb(s, 24000, 48000);
        CHECK(std::fabs(g) < 0.5, "Tight at depth 0 not transparent at %.0f Hz: %+.2f dB", f, g);
    }
    // silence in, silence out (no self-noise), in every mode
    for (int m = 0; m < axe::kNumModes; m++) {
        Sig z((size_t)sr, 0.f);
        Sig y = render(modeParams(m), z, sr);
        CHECK(rmsDb(y) < -140.0, "%s makes noise from silence: %.1f dB", axe::modeName(m), rmsDb(y));
    }
    // zero-length process calls are harmless
    { auto& p = procA(); p.init(sr); p.setParams(axe::Params{}); float d = 0.f; p.process(&d, &d, 0); CHECK(d == 0.f, "zero-length call wrote output"); }
}

static void testSampleRates() {
    const float rates[] = {44100.f, 48000.f, 88200.f, 96000.f, 176400.f, 192000.f};
    for (int m = 0; m < axe::kNumModes; m++) {
        for (int inst = 0; inst < 2; inst++) {
            double ref = 0;
            for (float sr : rates) {
                Sig x = pluckSig(sr, 2.f, inst ? std::initializer_list<float>{41.2f, 61.7f} : std::initializer_list<float>{82.4f, 123.5f, 164.8f});
                axe::Params p = modeParams(m, inst);
                p.gate = 0.3f;
                Sig y = render(p, x, sr, 256);
                CHECK(allFinite(y), "non-finite output: %s inst %d at %.0f Hz", axe::modeName(m), inst, sr);
                double g = rmsDb(y, (size_t)(0.5f * sr)) - rmsDb(x, (size_t)(0.5f * sr));
                if (sr == 48000.f) ref = g;
                CHECK(std::fabs(g) < 20.0, "%s inst %d at %.0f: gain %+.1f dB", axe::modeName(m), inst, sr, g);
                if (sr != 44100.f && sr != 48000.f)
                    CHECK(std::fabs(g - ref) < 1.5, "%s inst %d: loudness at %.0f Hz differs from 48k by %+.2f dB", axe::modeName(m), inst, sr, g - ref);
            }
        }
    }
}

static void testBlockSizes() {
    const float sr = 48000.f;
    Sig x = pluckSig(sr, 1.5f, {82.4f, 110.f, 146.8f});
    for (int m = 0; m < axe::kNumModes; m++) {
        axe::Params p = modeParams(m); p.gate = 0.4f;
        Sig ref = render(p, x, sr, 4096);
        for (int b : {1, 7, 64, 333}) {
            Sig y = render(p, x, sr, b);
            bool same = true; for (size_t i = 0; i < x.size(); i++) if (y[i] != ref[i]) { same = false; break; }
            CHECK(same, "%s: block size %d changes the output", axe::modeName(m), b);
        }
    }
}

// A parameter jump in the middle of a steady tone must not click.
static void jumpCheck(const char* what, const axe::Params& a, const axe::Params& b, float hz = 220.f, float limit = 4.f, double maxDip = 9.0) {
    const float sr = 48000.f;
    Sig x = sine(sr, 2.f, hz, 0.2f);
    auto& p = procA();
    p.init(sr); p.setParams(a); p.reset();
    const size_t at = (size_t)(1.0f * sr);
    Sig y = run(p, x, 64, [&](size_t i) { if (i == at) p.setParams(b); });
    float base = std::fmax(maxD2(y, at - 12000, at), maxD2(y, at + 24000, at + 36000));
    float jump = maxD2(y, at, at + 4800);
    CHECK(allFinite(y), "%s: non-finite", what);
    CHECK(jump <= limit * base + 1e-4f, "%s: click at the change (%.2fx the steady signal's 2nd difference)", what, jump / (base + 1e-9f));
    // no dropout: the 5 ms windows through the change stay within 9 dB of the quieter side
    double before = rmsDb(y, at - 9600, at), after = rmsDb(y, at + 24000, at + 33600);
    double lo = std::min(before, after);
    for (size_t w = at; w < at + 4800; w += 240) {
        double r = rmsDb(y, w, w + 240);
        if (r < lo - maxDip) { CHECK(false, "%s: dropout of %.1f dB during the change", what, lo - r); break; }
    }
}

static void testSmoothing() {
    struct J { const char* name; int mode; std::function<void(axe::Params&, bool)> set; };
    const J jumps[] = {
        {"in", axe::kStrings, [](axe::Params& p, bool hi) { p.inDb = hi ? 12.f : -12.f; }},
        {"out", axe::kStrings, [](axe::Params& p, bool hi) { p.outDb = hi ? 12.f : -24.f; }},
        {"mix", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.mix = hi ? 1.f : 0.f; }},
        {"tight", axe::kTight, [](axe::Params& p, bool hi) { p.tight = hi ? 1.f : 0.f; }},
        {"punch", axe::kTight, [](axe::Params& p, bool hi) { p.punch = hi ? 1.f : 0.f; }},
        {"low", axe::kTight, [](axe::Params& p, bool hi) { p.lowDb = hi ? 9.f : -9.f; }},
        {"high", axe::kTight, [](axe::Params& p, bool hi) { p.highDb = hi ? 9.f : -9.f; }},
        {"sub", axe::kSub, [](axe::Params& p, bool hi) { p.sub = hi ? 1.f : 0.f; }},
        {"dry", axe::kSub, [](axe::Params& p, bool hi) { p.dry = hi ? 1.f : 0.f; }},
        {"subtone", axe::kSub, [](axe::Params& p, bool hi) { p.subTone = hi ? 1.f : 0.f; }},
        {"range", axe::kSub, [](axe::Params& p, bool hi) { p.range = hi ? 1.f : 0.f; }},
        {"grit", axe::kSub, [](axe::Params& p, bool hi) { p.grit = hi ? 1.f : 0.f; }},
        {"bright", axe::kStrings, [](axe::Params& p, bool hi) { p.bright = hi ? 1.f : 0.f; }},
        {"clarity", axe::kStrings, [](axe::Params& p, bool hi) { p.clarity = hi ? 1.f : 0.f; }},
        {"air", axe::kStrings, [](axe::Params& p, bool hi) { p.airDb = hi ? 9.f : 0.f; }},
        {"amount", axe::kStrings, [](axe::Params& p, bool hi) { for (auto& g : p.match) g = 9.f; p.matchAmount = hi ? 1.f : 0.f; }},
        {"gain", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.gain = hi ? 1.f : 0.f; }},
        {"lows", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.lows = hi ? 1.f : 0.f; }},
        {"highs", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.highs = hi ? 1.f : 0.f; }},
        {"scoop", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.scoop = hi ? 1.f : 0.f; }},
        {"level", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.level = hi ? 12.f : -18.f; }},
        {"blend", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.blend = hi ? 1.f : 0.f; }},
    };
    for (const auto& j : jumps) {
        for (int dir = 0; dir < 2; dir++) {
            axe::Params a = modeParams(j.mode), b = a;
            j.set(a, dir == 0); j.set(b, dir != 0);
            std::string w = std::string("jump ") + j.name + (dir ? " up" : " down");
            // a pure tone has a tiny 2nd difference, and gain ramps legitimately
            // raise it; allow 4x (a step would be 50-1000x)
            jumpCheck(w.c_str(), a, b);
        }
    }
    // mode and instrument changes crossfade (no click, no dropout)
    for (int m1 = 0; m1 < axe::kNumModes; m1++)
        for (int m2 = 0; m2 < axe::kNumModes; m2++) {
            if (m1 == m2) continue;
            std::string w = std::string("mode ") + axe::modeName(m1) + " -> " + axe::modeName(m2);
            jumpCheck(w.c_str(), modeParams(m1), modeParams(m2), 220.f, 6.f, 15.0);
        }
    for (int m = 0; m < axe::kNumModes; m++) {
        std::string w = std::string("instrument switch in ") + axe::modeName(m);
        jumpCheck(w.c_str(), modeParams(m, axe::kGuitar), modeParams(m, axe::kBass), 220.f, 6.f, 15.0);
    }
    { axe::Params a = modeParams(axe::kSub), b = a; b.subType = 1; jumpCheck("sub poly -> mono", a, b, 110.f, 6.f); }
    // bypass on/off (Buzzsaw: the most different from the dry signal)
    { axe::Params a = modeParams(axe::kBuzzsaw), b = a; b.bypass = true; jumpCheck("bypass on", a, b, 220.f, 6.f); jumpCheck("bypass off", b, a, 220.f, 6.f); }
    // smoothing reaches its target: after the glide the output matches a static render
    {
        const float sr = 48000.f;
        Sig x = sine(sr, 2.f, 330.f, 0.2f);
        axe::Params a = modeParams(axe::kStrings), b = a; a.airDb = 0.f; b.airDb = 9.f;
        auto& p = procA(); p.init(sr); p.setParams(a); p.reset();
        Sig y = run(p, x, 64, [&](size_t i) { if (i == 48000) p.setParams(b); });
        Sig ref = render(b, x, sr, 64);
        double d = rmsDb(y, 72000, 96000) - rmsDb(ref, 72000, 96000);
        CHECK(std::fabs(d) < 0.05, "smoothed parameter does not settle on its target (%.3f dB off)", d);
    }
}

static void testReset() {
    const float sr = 48000.f;
    for (int m = 0; m < axe::kNumModes; m++) {
        axe::Params p = modeParams(m); p.gate = 0.3f;
        auto& a = procA(); auto& b = procB();
        a.init(sr); b.init(sr); a.setParams(p); b.setParams(p);
        // different histories
        Sig h1 = pluckSig(sr, 1.f, {98.f}), h2 = pluckSig(sr, 0.7f, {155.f, 233.f});
        run(a, h1); run(b, h2);
        a.reset(); b.reset();
        Sig x = pluckSig(sr, 1.f, {82.4f, 123.5f});
        Sig ya = run(a, x, 64), yb = run(b, x, 128);
        bool same = true; for (size_t i = 0; i < x.size(); i++) if (ya[i] != yb[i]) { same = false; break; }
        CHECK(same, "%s: two instances differ after reset (dual-mono sync)", axe::modeName(m));
    }
}

// Sub: a sine should give its octave below at about the same level; on a
// chord every note should get its own sub, with little else in between.
static void testSub() {
    const float sr = 48000.f;
    for (float f : {82.4f, 110.f, 164.8f, 246.9f, 329.6f}) {
        axe::Params p = modeParams(axe::kSub); p.dry = 0.f; p.sub = std::sqrt(0.5f); p.subTone = 1.f; p.range = 1.f;  // sub gain 1
        Sig x = sine(sr, 2.f, f, 0.2f);
        Sig y = render(p, x, sr);
        double sub = toneDb(y, 48000, 96000, f / 2.f, sr) - toneDb(x, 48000, 96000, f, sr);
        CHECK(std::fabs(sub) < 3.0, "poly sub of %.1f Hz: %+.1f dB re the note (want ~0)", f, sub);
    }
    // chord: E2 G#2 B2 (E major, the low triad), harmonic tones
    {
        const float notes[3] = {82.41f, 103.83f, 123.47f};
        Sig x((size_t)(2.f * sr), 0.f);
        for (float f0 : notes) for (int h = 1; h <= 6; h++)
            for (size_t i = 0; i < x.size(); i++) x[i] += 0.06f / h * std::sin(2.f * axe::kPi * f0 * h * (float)i / sr + h);
        axe::Params p = modeParams(axe::kSub); p.dry = 0.f; p.sub = std::sqrt(0.5f); p.subTone = 0.25f;
        Sig yPoly = render(p, x, sr);
        p.subType = 1;
        Sig yMono = render(p, x, sr);
        auto report = [&](const char* name, const Sig& y, bool must) {
            double worst = 0, rest = 0;
            for (float f0 : notes) worst = std::min(worst, toneDb(y, 48000, 96000, f0 / 2.f, sr) - toneDb(x, 48000, 96000, f0, sr) + 0.0);
            // energy below 140 Hz that is not one of the three subs (or the notes)
            int n = 4096; double tot = 0, want = 0;
            for (int k = 1; k < 120; k++) {
                float f = (float)k * sr / n / 4.f;   // 2.9 Hz steps
                double e = std::pow(10.0, toneDb(y, 48000, 96000, f, sr) / 10.0);
                tot += e;
                for (float f0 : notes) if (std::fabs(f - f0 / 2.f) < 3.f || std::fabs(f - f0) < 3.f) { want += e; break; }
            }
            rest = 10.0 * std::log10((tot - want) / (want + 1e-30) + 1e-30);
            std::printf("  sub on E major low triad, %s: weakest sub %+.1f dB re its note, stray low energy %+.1f dB re the subs\n", name, worst, rest);
            if (must) {
                CHECK(worst > -9.0, "%s: a chord note lost its sub (%.1f dB)", name, worst);
                CHECK(rest < -6.0, "%s: too much stray energy between the subs on a chord (%.1f dB)", name, rest);
            }
        };
        report("poly", yPoly, true);
        report("mono (WSOLA)", yMono, false);
    }
}

static void testLearn() {
    const float sr = 48000.f;
    // fresh = pinkish noise bursts; dull = the same through a high-cut
    Lcg r;
    Sig fresh((size_t)(16.f * sr)), dull(fresh.size());
    axe::Biquad pink, cut1, cut2;
    pink.lowShelf(400.f, 10.f, sr); cut1.highShelf(2500.f, -9.f, sr); cut2.peaking(5000.f, 1.f, -4.f, sr);
    for (size_t i = 0; i < fresh.size(); i++) {
        float env = std::fmod((float)i / sr, 0.5f) < 0.4f ? 0.2f : 0.f;
        float v = pink.process(r.next()) * env;
        fresh[i] = v;
    }
    // dull is a different noise sequence (different "riff"), same pinking, plus the high cut
    Lcg r2; r2.s = 99;
    axe::Biquad pink2; pink2.lowShelf(400.f, 10.f, sr);
    for (size_t i = 0; i < dull.size(); i++) {
        float env = std::fmod((float)i / sr, 0.6f) < 0.45f ? 0.25f : 0.f;
        dull[i] = cut2.process(cut1.process(pink2.process(r2.next()) * env));
    }
    auto& p = procA();
    p.init(sr); p.setLearnSeconds(10.f);
    axe::Params prm = modeParams(axe::kStrings); prm.airDb = 0.f; prm.bright = 0.f; prm.clarity = 0.f;
    p.setParams(prm); p.reset();
    p.learnFresh(); run(p, fresh);
    CHECK(p.hasFresh(), "Learn Fresh did not finish");
    p.learnNow(); run(p, dull);
    float g[axe::kMatchBands] = {};
    CHECK(p.learnedMatch(g), "Learn Now did not produce a match");
    std::printf("  learned match (dB):");
    for (int b = 0; b < axe::kMatchBands; b++) std::printf(" %g:%+.1f", axe::kMatchHz[b], g[b]);
    std::printf("\n");
    // apply it and measure how close dull gets to fresh
    for (int b = 0; b < axe::kMatchBands; b++) prm.match[b] = g[b];
    prm.matchAmount = 1.f;
    Sig fixed = render(prm, dull, sr);
    float pf[axe::kMatchBands], pd[axe::kMatchBands], px[axe::kMatchBands];
    auto prof = [&](const Sig& s, float* o) { axe::Learner l; l.init(sr); l.setSeconds(1e6f); l.start(); for (float v : s) l.feed(v, o); l.result(o); };
    prof(fresh, pf); prof(dull, pd); prof(fixed, px);
    double before = 0, after = 0;
    for (int b = 3; b < axe::kMatchBands; b++) { before = std::max(before, (double)std::fabs(pd[b] - pf[b])); after = std::max(after, (double)std::fabs(px[b] - pf[b])); }
    std::printf("  learn: worst band error above 1.6 kHz, dull vs fresh %.1f dB -> matched %.1f dB\n", before, after);
    CHECK(after < 1.5 && after < before * 0.3, "Learn match leaves %.1f dB (was %.1f)", after, before);
}

int main() {
    flushDenormals();
    std::printf("bypass / null\n");   testBypassAndNull();
    std::printf("sample rates\n");    testSampleRates();
    std::printf("block sizes\n");     testBlockSizes();
    std::printf("smoothing\n");       testSmoothing();
    std::printf("reset\n");           testReset();
    std::printf("sub\n");             testSub();
    std::printf("learn\n");           testLearn();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
