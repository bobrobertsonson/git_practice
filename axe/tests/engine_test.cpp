// Engine unit tests: no audio files, no framework. `make check`.
//
//   bypass / null   bypass is bit-exact in every mode; mix 0 nulls (or is flat
//                   where the dry path is a crossover or oversampling filters)
//   sample rates    44.1..192 kHz: stable, finite, loudness close to 48 kHz
//   blocks          output does not depend on the host block size
//   smoothing       parameter jumps, mode / instrument switches, Trigger and
//                   bypass toggles are click-free (and the tone modes dropout-free)
//   reset           kx:Reset makes two instances bit-identical (dual mono)
//   clock, pitch    tempo grid and swing; pitch tracker accuracy
//   sub             octave-down level, single notes and chords (vs WSOLA)
//   fx              each fx mode does what it says (glitch repeats, laser fires,
//                   dive drops, chip follows pitch, gate chops, shifter shifts,
//                   freeze sustains, feedback sings, swarm scatters, radio band-limits)
//   learn           Tone Match pulls a guitar onto a target; Learn My Playing
//                   measures floor, pick attack and speed, note range; auto gate
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
// Params for comparisons where a mode's own randomness or rhythm would get in
// the way (no auto-glitches, no gating, tone-free lasers).
static axe::Params calmParams(int mode, int inst = axe::kGuitar) {
    axe::Params p = modeParams(mode, inst);
    p.gChaos = 0.f; p.sDepth = 0.f; p.lTone = 0.f; p.rdStatic = 0.f; p.rdFade = 0.f;
    return p;
}
static double maxAbsDiff(const Sig& a, const Sig& b, size_t from = 0) {
    double e = 0; for (size_t i = from; i < a.size(); i++) e = std::max(e, (double)std::fabs(a[i] - b[i])); return e;
}
// Dominant frequency in y[a,b) between lo and hi Hz (scan, then refine).
static float dominantHz(const Sig& y, size_t a, size_t b, float lo, float hi, float sr) {
    float best = lo; double bv = -1e9;
    for (float f = lo; f <= hi; f *= 1.003f) { double v = toneDb(y, a, b, f, sr); if (v > bv) { bv = v; best = f; } }
    for (float f = best * 0.995f; f <= best * 1.005f; f += best * 0.0002f) { double v = toneDb(y, a, b, f, sr); if (v > bv) { bv = v; best = f; } }
    return best;
}

// ---------------------------------------------------------------------------
static void testBypassAndNull() {
    const float sr = 48000.f;
    Sig x = pluckSig(sr, 2.f, {82.4f, 123.5f, 164.8f});
    for (int m = 0; m < axe::kNumModes; m++) {
        for (int inst = 0; inst < 2; inst++) {
            axe::Params p = modeParams(m, inst);
            p.bypass = true; p.trigger = true;
            Sig y = render(p, x, sr);
            CHECK(maxAbsDiff(x, y) == 0.0, "bypass not bit-exact: mode %s inst %d", axe::modeName(m), inst);
        }
    }
    // mix = 0: Sub and every fx mode null exactly (their dry is the input)
    for (int m = 0; m < axe::kNumModes; m++) {
        if (m == axe::kTight || m == axe::kBuzzsaw) continue;
        axe::Params p = modeParams(m); p.mix = 0.f; p.trigger = true;
        Sig y = render(p, x, sr);
        CHECK(maxAbsDiff(x, y) < 1e-6, "mix 0 does not null: %s err %g", axe::modeName(m), maxAbsDiff(x, y));
    }
    { axe::Params p = modeParams(axe::kSub); p.sub = 0.f;
      Sig y = render(p, x, sr);
      CHECK(maxAbsDiff(x, y) < 1e-6, "sub=0 does not null"); }
    // While Triggered with Trigger off: every mode passes the input through
    for (int m = 0; m < axe::kNumModes; m++) {
        axe::Params p = modeParams(m); p.engage = axe::kWhileTriggered; p.trigger = false;
        Sig y = render(p, x, sr);
        double e = maxAbsDiff(x, y);
        // Tight/Buzzsaw: the gate fades the processed path fully out (exact); fx modes fade to the dry
        CHECK(e < 1e-6, "While Triggered, Trigger off: %s is not transparent (err %g)", axe::modeName(m), e);
    }
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
    for (float f : {150.f, 1800.f, 5000.f}) {
        axe::Params p = modeParams(axe::kTight); p.tight = 0.f;
        Sig s = sine(sr, 1.f, f, 0.25f);
        Sig y = render(p, s, sr);
        double g = rmsDb(y, 24000, 48000) - rmsDb(s, 24000, 48000);
        CHECK(std::fabs(g) < 0.5, "Tight at depth 0 not transparent at %.0f Hz: %+.2f dB", f, g);
    }
    // silence in, silence out, in every mode, Trigger on or off
    for (int m = 0; m < axe::kNumModes; m++)
        for (int t = 0; t < 2; t++) {
            Sig z((size_t)sr, 0.f);
            axe::Params p = modeParams(m); p.trigger = t == 1;
            Sig y = render(p, z, sr);
            if (m == axe::kLaser && t == 1) continue;   // the machine gun is a sound source: it fires with no input
            CHECK(rmsDb(y) < -140.0, "%s (trigger %d) makes noise from silence: %.1f dB", axe::modeName(m), t, rmsDb(y));
        }
    { auto& p = procA(); p.init(sr); p.setParams(axe::Params{}); float d = 0.f; p.process(&d, &d, 0); CHECK(d == 0.f, "zero-length call wrote output"); }
}

static void testSampleRates() {
    const float rates[] = {44100.f, 48000.f, 88200.f, 96000.f, 176400.f, 192000.f};
    for (int m = 0; m < axe::kNumModes; m++) {
        for (int inst = 0; inst < 2; inst++) {
            double ref = 0;
            for (float sr : rates) {
                Sig x = inst ? pluckSig(sr, 2.f, {41.2f, 61.7f}) : pluckSig(sr, 2.f, {82.4f, 123.5f, 164.8f});
                axe::Params p = modeParams(m, inst);
                p.gate = 0.3f; p.trigger = m != axe::kGlitch;   // a glitch held from the first sample repeats silence
                Sig y = render(p, x, sr, 256);
                CHECK(allFinite(y), "non-finite output: %s inst %d at %.0f Hz", axe::modeName(m), inst, sr);
                double g = rmsDb(y, (size_t)(0.5f * sr)) - rmsDb(x, (size_t)(0.5f * sr));
                if (sr == 48000.f) ref = g;
                CHECK(std::fabs(g) < 20.0, "%s inst %d at %.0f: gain %+.1f dB", axe::modeName(m), inst, sr, g);
                // the fx modes make random choices on a sample-rate dependent grid: looser
                const double tol = m >= axe::kGlitch ? 3.0 : 1.5;
                if (sr != 44100.f && sr != 48000.f)
                    CHECK(std::fabs(g - ref) < tol, "%s inst %d: loudness at %.0f Hz differs from 48k by %+.2f dB", axe::modeName(m), inst, sr, g - ref);
            }
        }
    }
}

static void testBlockSizes() {
    const float sr = 48000.f;
    Sig x = pluckSig(sr, 1.5f, {82.4f, 110.f, 146.8f});
    for (int m = 0; m < axe::kNumModes; m++) {
        axe::Params p = modeParams(m); p.gate = 0.4f; p.trigger = true;
        Sig ref = render(p, x, sr, 4096);
        for (int b : {1, 7, 64, 333}) {
            Sig y = render(p, x, sr, b);
            CHECK(maxAbsDiff(ref, y) == 0.0, "%s: block size %d changes the output", axe::modeName(m), b);
        }
    }
}

// A parameter change in the middle of a steady tone must not click.
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
    if (maxDip <= 0.0) return;
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
        {"in", axe::kTight, [](axe::Params& p, bool hi) { p.inDb = hi ? 12.f : -12.f; }},
        {"out", axe::kTight, [](axe::Params& p, bool hi) { p.outDb = hi ? 12.f : -24.f; }},
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
        {"gain", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.gain = hi ? 1.f : 0.f; }},
        {"lows", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.lows = hi ? 1.f : 0.f; }},
        {"highs", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.highs = hi ? 1.f : 0.f; }},
        {"scoop", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.scoop = hi ? 1.f : 0.f; }},
        {"level", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.level = hi ? 12.f : -18.f; }},
        {"blend", axe::kBuzzsaw, [](axe::Params& p, bool hi) { p.blend = hi ? 1.f : 0.f; }},
        {"dive dry", axe::kDive, [](axe::Params& p, bool hi) { p.dDry = hi ? 1.f : 0.f; }},
        {"chip dry", axe::kChip, [](axe::Params& p, bool hi) { p.cDry = hi ? 1.f : 0.f; }},
        {"ring freq", axe::kRing, [](axe::Params& p, bool hi) { p.rFreq = hi ? 1.f : 0.f; }},
        {"ring drive", axe::kRing, [](axe::Params& p, bool hi) { p.rDrive = hi ? 1.f : 0.f; }},
        {"feedback level", axe::kFeedback, [](axe::Params& p, bool hi) { p.fbLevel = hi ? 1.f : 0.f; }},
        {"radio band", axe::kRadio, [](axe::Params& p, bool hi) { p.rdBand = hi ? 1.f : 0.f; }},
        {"radio width", axe::kRadio, [](axe::Params& p, bool hi) { p.rdWidth = hi ? 1.f : 0.f; }},
        {"radio warble", axe::kRadio, [](axe::Params& p, bool hi) { p.rdWarble = hi ? 1.f : 0.f; }},
        {"tone match amount", axe::kTight, [](axe::Params& p, bool hi) { for (auto& g : p.match) g = 9.f; p.matchAmount = hi ? 1.f : 0.f; }},
        {"tone match band", axe::kTight, [](axe::Params& p, bool hi) { p.match[2] = hi ? 12.f : -12.f; }},
    };
    for (const auto& j : jumps) {
        for (int dir = 0; dir < 2; dir++) {
            axe::Params a = calmParams(j.mode), b = a;
            j.set(a, dir == 0); j.set(b, dir != 0);
            std::string w = std::string("jump ") + j.name + (dir ? " up" : " down");
            jumpCheck(w.c_str(), a, b, 220.f, 4.f, j.mode >= axe::kGlitch ? 0.0 : 9.0);
        }
    }
    // mode and instrument changes crossfade: no click; the tone modes also no deep dip
    for (int m1 = 0; m1 < axe::kNumModes; m1++)
        for (int m2 = 0; m2 < axe::kNumModes; m2++) {
            if (m1 == m2) continue;
            std::string w = std::string("mode ") + axe::modeName(m1) + " -> " + axe::modeName(m2);
            const bool tone = m1 < axe::kGlitch && m2 < axe::kGlitch;
            jumpCheck(w.c_str(), calmParams(m1), calmParams(m2), 220.f, 6.f, tone ? 15.0 : 0.0);
        }
    for (int m = 0; m < axe::kNumModes; m++) {
        std::string w = std::string("instrument switch in ") + axe::modeName(m);
        jumpCheck(w.c_str(), calmParams(m, axe::kGuitar), calmParams(m, axe::kBass), 220.f, 6.f, m < axe::kGlitch ? 15.0 : 0.0);
    }
    // Trigger on and off, both engage modes, every mode
    for (int m = 0; m < axe::kNumModes; m++)
        for (int e = 0; e < 2; e++) {
            axe::Params a = calmParams(m), b = a;
            a.engage = b.engage = e; b.trigger = true;
            std::string w = std::string("trigger on, ") + axe::modeName(m) + (e ? " (while triggered)" : " (always)");
            jumpCheck(w.c_str(), a, b, 220.f, 8.f, 0.0);
            w = std::string("trigger off, ") + axe::modeName(m) + (e ? " (while triggered)" : " (always)");
            jumpCheck(w.c_str(), b, a, 220.f, 8.f, 0.0);
        }
    { axe::Params a = calmParams(axe::kSub), b = a; b.subType = 1; jumpCheck("sub poly -> mono", a, b, 110.f, 6.f); }
    { axe::Params a = calmParams(axe::kBuzzsaw), b = a; b.bypass = true; jumpCheck("bypass on", a, b, 220.f, 6.f); jumpCheck("bypass off", b, a, 220.f, 6.f); }
    // smoothing reaches its target: after the glide the output matches a static render
    {
        const float sr = 48000.f;
        Sig x = sine(sr, 2.f, 330.f, 0.2f);
        axe::Params a = modeParams(axe::kTight), b = a; a.highDb = -6.f; b.highDb = 6.f; a.tight = b.tight = 0.f;
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
        axe::Params p = modeParams(m); p.gate = 0.3f; p.trigger = true;
        auto& a = procA(); auto& b = procB();
        a.init(sr); b.init(sr); a.setParams(p); b.setParams(p);
        Sig h1 = pluckSig(sr, 1.f, {98.f}), h2 = pluckSig(sr, 0.7f, {155.f, 233.f});
        run(a, h1); run(b, h2);
        a.reset(); b.reset();
        Sig x = pluckSig(sr, 1.f, {82.4f, 123.5f});
        Sig ya = run(a, x, 64), yb = run(b, x, 128);
        CHECK(maxAbsDiff(ya, yb) == 0.0, "%s: two instances differ after reset (dual-mono sync)", axe::modeName(m));
    }
}

static void testClockAndPitch() {
    const float sr = 48000.f;
    // 120 BPM, 1/16: 8 steps a second
    {
        auto& p = procA(); p.init(sr);
        axe::Params q = modeParams(axe::kStutter); q.tempo = 120.f; q.rate = 4; q.sync = 0;
        p.setParams(q); p.reset();
        Sig z((size_t)(2 * sr), 0.f), y(z.size());
        int last = -1, ticks = 0;
        for (size_t i = 0; i < z.size(); i++) { p.process(&z[i], &y[i], 1); if (p.step() != last) { ticks++; last = p.step(); } }
        CHECK(ticks >= 16 && ticks <= 17, "120 BPM 1/16 over 2 s: %d steps (want 16)", ticks);
    }
    // swing: the second 16th of each pair starts late
    {
        auto& p = procA(); p.init(sr);
        axe::Params q = modeParams(axe::kStutter); q.tempo = 120.f; q.rate = 4; q.swing = 1.f; q.sync = 0;
        p.setParams(q); p.reset();
        float z = 0.f, y = 0.f; int last = 0; size_t at1 = 0;
        for (size_t i = 0; i < 48000 && !at1; i++) { p.process(&z, &y, 1); if (p.step() == 1 && last == 0) at1 = i; last = p.step(); }
        const float want = 0.125f * 1.5f * sr;   // 75% of the pair
        CHECK(std::fabs((float)at1 - want) < 2.f, "swing 100%%: second step at %zu samples (want %.0f)", at1, want);
    }
    // pitch tracker (through Chiptune, which runs it)
    for (int inst = 0; inst < 2; inst++)
        for (float f : inst ? std::vector<float>{30.9f, 41.2f, 55.f, 98.f, 220.f} : std::vector<float>{55.f, 82.4f, 110.f, 196.f, 440.f, 987.8f}) {
            Sig x((size_t)sr);
            for (size_t i = 0; i < x.size(); i++) {   // a guitar-ish tone: harmonics
                float t = (float)i / sr, v = 0.f;
                for (int h = 1; h <= 6; h++) v += 0.2f / h * std::sin(2.f * axe::kPi * f * h * t + h);
                x[i] = v;
            }
            auto& p = procA(); p.init(sr);
            axe::Params q = modeParams(axe::kChip, inst); p.setParams(q); p.reset();
            Sig y = run(p, x, 128);
            float got = p.detectedPitch();
            CHECK(std::fabs(got / f - 1.f) < 0.01f, "pitch tracker (%s): %.1f Hz read as %.1f", inst ? "bass" : "guitar", f, got);
        }
}

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


// ---------------------------------------------------------------------------
// The fx modes, one functional check each.
// ---------------------------------------------------------------------------
static Sig withTrigger(axe::Params p, const Sig& x, float sr, float on, float off, int block = 64) {
    auto& pr = procA(); pr.init(sr); p.trigger = false; pr.setParams(p); pr.reset();
    const size_t a = (size_t)(on * sr), b = (size_t)(off * sr);
    return run(pr, x, block, [&](size_t i) {
        if (i == a) { p.trigger = true; pr.setParams(p); }
        if (i == b) { p.trigger = false; pr.setParams(p); }
    });
}

static void testFx() {
    const float sr = 48000.f;
    // GLITCH: Chaos 0 = a clean beat repeat: while held the output repeats
    // with the step period (an upward sweep never repeats on its own)
    {
        Sig x((size_t)(3 * sr));
        double ph = 0; for (size_t i = 0; i < x.size(); i++) { double f = 200.0 + 600.0 * i / x.size(); ph += f / sr; x[i] = 0.3f * (float)std::sin(2 * M_PI * ph); }
        axe::Params p = modeParams(axe::kGlitch); p.gChaos = 0.f; p.tempo = 120.f; p.sync = 0; p.rate = 3;   // 1/8 = 12000 samples
        Sig y = withTrigger(p, x, sr, 1.f, 2.5f);
        const size_t step = 12000, a = 60000;
        double num = 0, da = 0, db = 0;
        for (size_t i = a; i < a + step; i++) { num += (double)y[i] * y[i + step]; da += (double)y[i] * y[i]; db += (double)y[i + step] * y[i + step]; }
        double corr = num / std::sqrt(da * db + 1e-30);
        std::printf("  glitch: correlation one step apart while held %.3f\n", corr);
        CHECK(corr > 0.95, "glitch at chaos 0 does not repeat the step (corr %.3f)", corr);
        CHECK(maxAbsDiff(Sig(x.begin(), x.begin() + 47000), Sig(y.begin(), y.begin() + 47000)) < 1e-6, "glitch changes the sound before the trigger");
    }
    // LASER: a pick fires a falling tone (energy that was not in the input)
    {
        Sig x = pluckSig(sr, 1.f, {110.f}, 0.3f, 2.f);
        axe::Params p = modeParams(axe::kLaser); p.lTone = 0.f; p.lPitch = 0.6f; p.lLevel = 0.7f;   // starts at ~1.5 kHz
        p.lSweep = 1.f; p.lDrop = 0.25f;                                                              // 24 semitones over 800 ms
        Sig y = render(p, x, sr);
        Sig d(x.size()); for (size_t i = 0; i < x.size(); i++) d[i] = y[i] - x[i];
        float f1 = dominantHz(d, 0, 4800, 300.f, 4000.f, sr), f2 = dominantHz(d, 14400, 19200, 300.f, 4000.f, sr);
        std::printf("  laser: %.0f Hz over the first 100 ms, %.0f Hz at 300-400 ms\n", f1, f2);
        CHECK(rmsDb(d, 0, 9600) - rmsDb(x, 0, 9600) > -20.0 && f2 < f1 * 0.8f, "laser did not fire a falling tone (%.0f -> %.0f Hz)", f1, f2);
    }
    // DIVE BOMB: an octave down after the dive time, back after the return
    {
        Sig x = sine(sr, 3.f, 440.f, 0.25f);
        axe::Params p = modeParams(axe::kDive); p.dDepth = 12.f; p.dWobble = 0.f; p.tempo = 120.f; p.sync = 0;
        p.dTime = 0.5f; p.dReturn = 0.3f;   // 0.5 beat = 250 ms
        Sig y = withTrigger(p, x, sr, 0.5f, 2.f);
        float fDown = dominantHz(y, (size_t)(1.0f * sr), (size_t)(1.8f * sr), 100.f, 600.f, sr);
        float fBack = dominantHz(y, (size_t)(2.6f * sr), (size_t)(2.95f * sr), 100.f, 600.f, sr);
        std::printf("  dive: %.1f Hz at the bottom, %.1f Hz after the return\n", fDown, fBack);
        CHECK(std::fabs(fDown / 220.f - 1.f) < 0.02f, "dive did not reach an octave down (%.1f Hz)", fDown);
        CHECK(std::fabs(fBack / 440.f - 1.f) < 0.01f, "dive did not come back (%.1f Hz)", fBack);
    }
    // CHIPTUNE: follows the note, an octave up when asked, as a pulse wave (odd harmonics)
    {
        Sig x((size_t)(1.5f * sr));
        for (size_t i = 0; i < x.size(); i++) { float t = (float)i / sr; x[i] = 0.2f * std::sin(2.f * axe::kPi * 220.f * t) + 0.05f * std::sin(2.f * axe::kPi * 440.f * t); }
        axe::Params p = modeParams(axe::kChip); p.cDry = 0.f; p.cCrush = 0.f; p.cGlide = 0.f; p.cOctave = 2; p.cPulse = 1.f;   // square: 3rd harmonic at 1/3
        Sig y = render(p, x, sr);
        float f = dominantHz(y, 36000, 72000, 100.f, 1000.f, sr);
        double h3 = toneDb(y, 36000, 72000, 3.f * f, sr) - toneDb(y, 36000, 72000, f, sr);
        std::printf("  chiptune: 220 Hz in, %.1f Hz out (octave +1), 3rd harmonic %.1f dB\n", f, h3);
        CHECK(std::fabs(f / 440.f - 1.f) < 0.01f && h3 > -20.0, "chiptune does not follow the note as a pulse wave (%.1f Hz, h3 %.1f dB)", f, h3);
    }
    // STUTTER GATE: offbeat pattern, full depth: alternate steps open and shut
    {
        Sig x = sine(sr, 2.f, 220.f, 0.25f);
        axe::Params p = modeParams(axe::kStutter); p.sPattern = 1; p.sDepth = 1.f; p.sDuty = 1.f; p.tempo = 120.f; p.sync = 0; p.rate = 4;
        Sig y = render(p, x, sr);
        const size_t st = 6000;
        double closed = rmsDb(y, 8 * st + 500, 9 * st - 500), open = rmsDb(y, 9 * st + 500, 10 * st - 500);
        std::printf("  stutter: open step %.1f dB, closed step %.1f dB\n", open - rmsDb(x), closed - rmsDb(x));
        CHECK(open - closed > 30.0, "stutter gate does not chop (open %.1f vs closed %.1f dB)", open, closed);
    }
    // RING SHREDDER: frequency shifter moves 1 kHz by +100 Hz, image suppressed; ring mod makes both sidebands
    {
        Sig x = sine(sr, 1.f, 1000.f, 0.25f);
        axe::Params p = modeParams(axe::kRing); p.rType = 1; p.rDrive = 0.f;
        p.rFreq = std::log(100.f / 20.f) / std::log(150.f);   // 100 Hz carrier
        Sig y = render(p, x, sr);
        double up = toneDb(y, 12000, 48000, 1100.f, sr), img = toneDb(y, 12000, 48000, 900.f, sr), orig = toneDb(y, 12000, 48000, 1000.f, sr);
        std::printf("  shifter: 1100 Hz %.1f dB, image 900 Hz %.1f dB, 1000 Hz %.1f dB\n", up, img, orig);
        CHECK(up - img > 30.0 && up - orig > 30.0, "frequency shifter is not clean (image %.1f, orig %.1f dB re shifted)", img - up, orig - up);
        p.rType = 0;
        y = render(p, x, sr);
        up = toneDb(y, 12000, 48000, 1100.f, sr); img = toneDb(y, 12000, 48000, 900.f, sr); orig = toneDb(y, 12000, 48000, 1000.f, sr);
        CHECK(std::fabs(up - img) < 1.0 && up - orig > 30.0, "ring modulator sidebands wrong (%.1f / %.1f / carrier leak %.1f)", up, img, orig);
    }
    // FREEZE: holds the note after the input stops
    {
        Sig x = pluckSig(sr, 4.f, {196.f}, 0.3f, 100.f);
        for (size_t i = (size_t)(0.8f * sr); i < x.size(); i++) x[i] = 0.f;   // string muted at 0.8 s
        axe::Params p = modeParams(axe::kFreeze); p.engage = axe::kWhileTriggered; p.fzAttack = 0.f;
        Sig y = withTrigger(p, x, sr, 0.3f, 3.f);
        double held = rmsDb(y, (size_t)(1.5f * sr), (size_t)(2.5f * sr)), note = rmsDb(x, (size_t)(0.25f * sr), (size_t)(0.3f * sr));
        double after = rmsDb(y, (size_t)(3.9f * sr), (size_t)(4.f * sr));
        std::printf("  freeze: %.1f dB re the note 1 s after the string stopped; %.1f dB after release\n", held - note, after - note);
        CHECK(held - note > -12.0 && after < held - 30.0, "freeze does not hold / release (held %.1f, after %.1f dB)", held - note, after - note);
    }
    // FEEDBACK: holds a note, Trigger: a sustained octave grows and outlives the string
    {
        Sig x((size_t)(3 * sr));
        for (size_t i = 0; i < x.size(); i++) { float t = (float)i / sr; x[i] = 0.2f * std::exp(-1.5f * t) * (std::sin(2.f * axe::kPi * 220.f * t) + 0.3f * std::sin(2.f * axe::kPi * 440.f * t)); }
        axe::Params p = modeParams(axe::kFeedback); p.engage = axe::kWhileTriggered; p.fbHarmonic = 1; p.fbSwell = 0.2f; p.fbBloom = 0.f;
        Sig y = withTrigger(p, x, sr, 0.3f, 3.f);
        double oct = toneDb(y, (size_t)(2.0f * sr), (size_t)(2.5f * sr), 440.f, sr), inOct = toneDb(x, (size_t)(2.0f * sr), (size_t)(2.5f * sr), 440.f, sr);
        std::printf("  feedback: octave at 2 s %.1f dB above the decayed string's\n", oct - inOct);
        CHECK(oct - inOct > 15.0, "feedback did not sing on the octave (%.1f dB)", oct - inOct);
    }
    // SWARM: grains add sound that is not just the input, at a sane level
    {
        Sig x = pluckSig(sr, 2.f, {147.f, 220.f});
        axe::Params p = modeParams(axe::kSwarm);
        Sig y = render(p, x, sr);
        Sig d(x.size()); for (size_t i = 0; i < x.size(); i++) d[i] = y[i] - x[i];
        double rel = rmsDb(d, 24000) - rmsDb(x, 24000);
        std::printf("  swarm: grains %.1f dB re the input\n", rel);
        CHECK(rel > -20.0 && rel < 6.0, "swarm level out of range (%.1f dB)", rel);
    }
    // RADIO: band-limits noise around the station
    {
        Sig x((size_t)sr); Lcg r; for (auto& v : x) v = 0.2f * r.next();
        axe::Params p = modeParams(axe::kRadio); p.rdStatic = 0.f; p.rdFade = 0.f; p.rdWarble = 0.f; p.rdWidth = 0.f; p.rdBand = 0.5f;   // ~950 Hz, Q 6
        Sig y = render(p, x, sr);
        const float fc = 300.f * std::sqrt(10.f);
        double in = toneDb(y, 12000, 48000, fc, sr), out = 0.5 * (toneDb(y, 12000, 48000, fc * 4.f, sr) + toneDb(y, 12000, 48000, fc / 4.f, sr));
        std::printf("  radio: station %.0f Hz is %.1f dB above two octaves off\n", fc, in - out);
        CHECK(in - out > 15.0, "radio does not band-limit (%.1f dB)", in - out);
    }
}

// ---------------------------------------------------------------------------
// Learn: Tone Match and Learn My Playing
// ---------------------------------------------------------------------------
static void testLearn() {
    const float sr = 48000.f;
    // TONE MATCH: target = pinkish noise bursts; current guitar = a different
    // sequence through a high cut. Match Now should pull it onto the target.
    {
        Lcg r; Sig target((size_t)(12.f * sr)), dull(target.size());
        axe::Biquad pink, pink2, cut1, cut2;
        pink.lowShelf(400.f, 10.f, sr); pink2.lowShelf(400.f, 10.f, sr);
        cut1.highShelf(2500.f, -9.f, sr); cut2.peaking(5000.f, 1.f, -4.f, sr);
        for (size_t i = 0; i < target.size(); i++) target[i] = pink.process(r.next()) * (std::fmod((float)i / sr, 0.5f) < 0.4f ? 0.2f : 0.f);
        Lcg r2; r2.s = 99;
        for (size_t i = 0; i < dull.size(); i++) dull[i] = cut2.process(cut1.process(pink2.process(r2.next()) * (std::fmod((float)i / sr, 0.6f) < 0.45f ? 0.25f : 0.f)));
        auto& p = procA(); p.init(sr); p.setLearnSeconds(8.f, 8.f);
        axe::Params prm = modeParams(axe::kTight); prm.tight = 0.f;
        p.setParams(prm); p.reset();
        p.learnTarget(); run(p, target);
        CHECK(p.hasTarget(), "Learn Target did not finish");
        p.matchNow(); run(p, dull);
        float g[axe::kMatchBands] = {};
        CHECK(p.learnedMatch(g), "Match Now did not produce a match");
        std::printf("  tone match (dB):");
        for (int b = 0; b < axe::kMatchBands; b++) std::printf(" %g:%+.1f", axe::kMatchHz[b], g[b]);
        std::printf("\n");
        for (int b = 0; b < axe::kMatchBands; b++) prm.match[b] = g[b];
        Sig fixed = render(prm, dull, sr);
        float pf[axe::kMatchBands], pd[axe::kMatchBands], px[axe::kMatchBands];
        auto prof = [&](const Sig& s, float* o) { axe::Learner l; l.init(sr); l.setSeconds(1e6f); l.start(); for (float v : s) l.feed(v, o); l.result(o); };
        prof(target, pf); prof(dull, pd); prof(fixed, px);
        double before = 0, after = 0;
        for (int b = 3; b < axe::kMatchBands; b++) { before = std::max(before, (double)std::fabs(pd[b] - pf[b])); after = std::max(after, (double)std::fabs(px[b] - pf[b])); }
        std::printf("  tone match: worst band error above 1.6 kHz %.1f dB -> %.1f dB\n", before, after);
        CHECK(after < 1.5 && after < before * 0.3, "Tone Match leaves %.1f dB (was %.1f)", after, before);
    }
    // LEARN MY PLAYING: phrases of plucked notes from E2 to A3, a pick every
    // 250 ms, pauses between phrases, over hiss at about -66 dBFS
    {
        Sig x((size_t)(12.f * sr), 0.f);
        Lcg r;
        for (auto& v : x) v = 0.00071f * r.next();                 // uniform +-0.00071: -66 dBFS RMS
        const float notes[8] = {82.4f, 98.f, 110.f, 123.5f, 146.8f, 164.8f, 196.f, 220.f};
        int k = 0;
        for (float t0 = 0.f; t0 < 11.5f; t0 += 0.25f) {
            if (std::fmod(t0, 2.5f) >= 1.9f) continue;              // pause after each phrase
            const float f0 = notes[k++ % 8];
            for (size_t i = (size_t)(t0 * sr); i < x.size() && i < (size_t)((t0 + 0.24f) * sr); i++) {
                float t = (float)i / sr - t0, v = 0.f;
                for (int h = 1; h <= 6; h++) v += 0.25f / (float)h * std::sin(2.f * axe::kPi * f0 * (float)h * t);
                x[i] += v * std::exp(-6.f * t) * std::fmin(1.f, t * 2000.f);
            }
        }
        auto& p = procA(); p.init(sr); p.setLearnSeconds(8.f, 11.f);
        axe::Params prm = modeParams(axe::kTight); p.setParams(prm); p.reset();
        p.learnPlaying(); run(p, x);
        axe::PlayingProfile pp;
        CHECK(p.learnedPlaying(pp), "Learn Playing did not finish");
        std::printf("  learn playing: floor %.1f dBFS, pick sense %.1f dB, pick hold %.0f ms, notes %.0f-%.0f Hz\n",
                    pp.noiseFloor, pp.pickSense, pp.pickHold, pp.lowNote, pp.highNote);
        CHECK(std::fabs(pp.noiseFloor + 66.f) < 5.f, "noise floor read as %.1f dBFS (want ~-66)", pp.noiseFloor);
        CHECK(std::fabs(pp.pickHold - 125.f) < 40.f, "pick hold %.0f ms (want ~125: half the fastest picking)", pp.pickHold);
        CHECK(pp.pickSense >= 4.f && pp.pickSense <= 16.f, "pick sense %.1f dB out of range", pp.pickSense);
        CHECK(pp.lowNote > 65.f && pp.lowNote < 82.f && pp.highNote > 220.f && pp.highNote < 280.f,
              "note range %.0f-%.0f Hz (want just around 82-220)", pp.lowNote, pp.highNote);
        // Auto Gate from the learned floor: the hiss between phrases goes, the notes stay
        axe::Params g = modeParams(axe::kTight); g.tight = 0.f; g.autoGate = true; g.gate = 0.f; g.noiseFloor = pp.noiseFloor;
        Sig y = render(g, x, sr);
        g.autoGate = false;
        Sig y0 = render(g, x, sr);
        const size_t p0 = (size_t)(2.3f * sr), p1 = (size_t)(2.45f * sr), n0 = (size_t)(0.5f * sr), n1 = (size_t)(0.6f * sr);
        const double hissCut = rmsDb(y0, p0, p1) - rmsDb(y, p0, p1), noteChange = rmsDb(y, n0, n1) - rmsDb(y0, n0, n1);
        std::printf("  auto gate: hiss in the pauses down %.1f dB, notes changed %+.2f dB\n", hissCut, noteChange);
        CHECK(hissCut > 18.0 && std::fabs(noteChange) < 0.5, "auto gate (hiss down %.1f dB, notes %+.2f dB)", hissCut, noteChange);
    }
}

int main() {
    flushDenormals();
    std::printf("bypass / null\n");   testBypassAndNull();
    std::printf("sample rates\n");    testSampleRates();
    std::printf("block sizes\n");     testBlockSizes();
    std::printf("smoothing\n");       testSmoothing();
    std::printf("reset\n");           testReset();
    std::printf("clock and pitch\n"); testClockAndPitch();
    std::printf("sub\n");             testSub();
    std::printf("fx\n");              testFx();
    std::printf("learn\n");           testLearn();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
