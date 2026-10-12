// Tests for the live features, on real or synthetic audio.
//
//   live_test bypass   in.wav out.wav [name=value ...]   toggle bypass every 6 s after 10 s
//   live_test feedback in.wav out.wav gain [name=value ...]
//            closed loop: the mic hears the voice plus the speaker through a
//            resonant room (modes at 1.3 and 2.9 kHz, 10 ms away)
//   live_test falsepos in.wav [name=value ...]        how many notches a take triggers
//   live_test momentary in.wav out.wav [name=value ...]
//            hold mega 2-4 s and 6-8 s, hold throw 10-11 s: level jump and
//            click size at each mega edge, and the throw's tail after release
//   live_test stutter in.wav out.wav [name=value ...]
//            hold stutter 2-4 s and glitch 6-9 s: how exactly the loop repeats,
//            the largest sample step while held vs the same run without
////
// Prints numbers; the WAVs are for listening and for tools that measure them.
#include "common.h"
#include <cmath>

static rasp::Processor proc;   // ~1.5 MB: keep it off the stack
static rasp::Processor ref;    // the same run without the momentary effects

static int usage() {
    std::fprintf(stderr, "usage: live_test bypass|feedback|falsepos|momentary ... (see source)\n");
    return 1;
}

int main(int argc, char** argv) {
    flushDenormals();
    if (argc < 3) return usage();
    std::string mode = argv[1];
    std::vector<float> in; int sr = 0;
    if (!readWav(argv[2], in, sr)) { std::fprintf(stderr, "could not read %s\n", argv[2]); return 1; }
    rasp::Params p;
    int first = mode == "falsepos" ? 3 : (mode == "feedback" ? 5 : 4);
    for (int i = first; i < argc; i++)
        if (!setParam(p, argv[i])) { std::fprintf(stderr, "unknown parameter: %s\n", argv[i]); return 1; }
    proc.init((float)sr);
    proc.setParams(p);
    const size_t n = in.size();
    std::vector<float> L(n), R(n);

    if (mode == "momentary") {
        if (argc < 4) return usage();
        ref.init((float)sr); ref.setParams(p);
        std::vector<float> rL(n), rR(n);
        auto held = [&](double t, double a, double b) { return t >= a && t < b; };
        for (size_t i = 0; i < n; i++) {
            double t = (double)i / sr;
            bool mg = held(t, 2, 4) || held(t, 6, 8), th = held(t, 10, 11);
            if (mg != p.mega || th != p.throwOn) { p.mega = mg; p.throwOn = th; proc.setParams(p); }
            proc.process(in[i], L[i], R[i]);
            ref.process(in[i], rL[i], rR[i]);
        }
        auto rmsDb = [&](const std::vector<float>& x, double a, double b) {
            size_t i0 = (size_t)(a * sr), i1 = std::min(n, (size_t)(b * sr));
            double e = 0; for (size_t i = i0; i < i1; i++) e += (double)x[i] * x[i];
            return 10 * std::log10(e / std::max<size_t>(1, i1 - i0) + 1e-20);
        };
        // Clicks: largest sample step near an edge vs the largest step of
        // the reference run in the same place (the voice itself moves too).
        auto step = [&](const std::vector<float>& x, double a, double b) {
            size_t i0 = (size_t)(a * sr) + 1, i1 = std::min(n, (size_t)(b * sr));
            float m = 0; for (size_t i = i0; i < i1; i++) m = std::max(m, std::fabs(x[i] - x[i - 1]));
            return m;
        };
        std::printf("mega edge   level change (0.3 s after vs before)   largest step: with mega / without\n");
        for (double e : {2.0, 4.0, 6.0, 8.0}) {
            if (e * sr >= n) break;
            double d = rmsDb(L, e + 0.03, e + 0.33) - rmsDb(L, e - 0.33, e - 0.03);
            double dr = rmsDb(rL, e + 0.03, e + 0.33) - rmsDb(rL, e - 0.33, e - 0.03);
            std::printf("  %4.1f s    %+5.1f dB (voice itself %+5.1f dB)            %6.1f / %6.1f dBFS\n", e, d, dr,
                        20 * std::log10(step(L, e - 0.01, e + 0.02) + 1e-9), 20 * std::log10(step(rL, e - 0.01, e + 0.02) + 1e-9));
        }
        std::vector<float> dL(n);
        for (size_t i = 0; i < n; i++) dL[i] = L[i] - rL[i];
        std::printf("throw tail after release at 11 s (difference from the no-throw run, per 0.5 s):\n ");
        for (double t = 11.0; t < 15.0 && t * sr < n; t += 0.5) std::printf(" %+6.1f", rmsDb(dL, t, t + 0.5));
        std::printf(" dB\nbefore the throw (9-10 s, should be silent): %+.1f dB\n", rmsDb(dL, 9.0, 10.0));
        writeWav(argv[3], L, R, sr);
        return 0;
    }

    if (mode == "stutter") {
        if (argc < 4) return usage();
        p.flavourOn = false; p.thickenOn = false;   // measure the loop itself, not the effects' tails
        proc.setParams(p);
        ref.init((float)sr); ref.setParams(p);
        std::vector<float> rL(n), rR(n);
        auto held = [&](double t, double a, double b) { return t >= a && t < b; };
        for (size_t i = 0; i < n; i++) {
            double t = (double)i / sr;
            bool st = held(t, 2, 4), gl = held(t, 6, 9);
            if (st != p.stutter || gl != p.glitch) { p.stutter = st; p.glitch = gl; proc.setParams(p); }
            proc.process(in[i], L[i], R[i]);
            ref.process(in[i], rL[i], rR[i]);
        }
        auto maxStep = [&](const std::vector<float>& x, double a, double b) {
            size_t i0 = (size_t)(a * sr) + 1, i1 = std::min(n, (size_t)(b * sr));
            float m = 0; for (size_t i = i0; i < i1; i++) m = std::max(m, std::fabs(x[i] - x[i - 1]));
            return 20 * std::log10(m + 1e-9);
        };
        static const float frac[4] = {1.f, 0.5f, 0.25f, 0.125f};
        const size_t slice = (size_t)(60.0 / p.bpm * frac[std::max(0, std::min(3, p.stutterDiv))] * sr);
        // Loop exactness: correlation of the output with itself one slice later,
        // over the held part after the first slice was recorded.
        size_t a = (size_t)(2.0 * sr) + 2 * slice, b = (size_t)(4.0 * sr) - slice - (size_t)(0.01 * sr);
        double xy = 0, xx = 0, yy = 0;
        for (size_t i = a; i < b && i + slice < n; i++) { xy += (double)L[i] * L[i + slice]; xx += (double)L[i] * L[i]; yy += (double)L[i + slice] * L[i + slice]; }
        std::printf("slice %.0f ms; stutter loop: correlation one slice apart %.3f\n", 1000.0 * slice / sr, xy / std::sqrt(xx * yy + 1e-20));
        std::printf("largest step (dBFS), with / without:  stutter 2-4.1 s %.1f / %.1f   glitch 6-9.1 s %.1f / %.1f\n",
                    maxStep(L, 1.99, 4.1), maxStep(rL, 1.99, 4.1), maxStep(L, 5.99, 9.1), maxStep(rL, 5.99, 9.1));
        double d = 0; size_t i0 = (size_t)(4.1 * sr), i1 = std::min(n, (size_t)(5.5 * sr));
        for (size_t i = i0; i < i1; i++) d = std::max(d, (double)std::fabs(L[i] - rL[i]));
        std::printf("after release (4.1-5.5 s) largest difference from the run without: %.1f dBFS\n", 20 * std::log10(d + 1e-9));
        writeWav(argv[3], L, R, sr);
        return 0;
    }

    if (mode == "bypass") {
        if (argc < 4) return usage();
        size_t learn = 10 * sr, seg = 6 * sr;
        bool byp = false;
        std::printf("segments (s):");
        for (size_t i = 0; i < n; i++) {
            if (i >= learn && (i - learn) % seg == 0) {
                byp = !byp; p.bypass = byp; proc.setParams(p);
                std::printf(" %.1f:%s", (double)i / sr, byp ? "bypass" : "effect");
            }
            proc.process(in[i], L[i], R[i]);
        }
        std::printf("\nlearned match: %+.1f dB\n", proc.bypassMatchDb());
        writeWav(argv[3], L, R, sr);
        return 0;
    }

    if (mode == "feedback") {
        if (argc < 5) return usage();
        float G = (float)std::atof(argv[4]);
        // Room: two resonant modes plus a broad path, 10 ms of travel.
        rasp::Biquad m1, m2, broad;
        auto bandpass = [&](rasp::Biquad& b, float hz, float q) {
            float w = 2.f * rasp::kPi * hz / sr, al = std::sin(w) / (2.f * q), c = std::cos(w);
            b.norm(al, 0.f, -al, 1.f + al, -2.f * c, 1.f - al);
        };
        bandpass(m1, 1300.f, 30.f); bandpass(m2, 2900.f, 25.f); bandpass(broad, 1000.f, 0.5f);
        const size_t D = (size_t)(0.010 * sr);
        std::vector<float> spk(n + D, 0.f);
        size_t firstEvent = 0;
        for (size_t i = 0; i < n; i++) {
            float back = spk[i];                       // what the speaker played D samples ago
            float room = m1.process(back) + 0.7f * m2.process(back) + 0.15f * broad.process(back);
            float mic = in[i] + G * room;
            proc.process(mic, L[i], R[i]);
            spk[i + D] = 0.5f * (L[i] + R[i]);
            if (!firstEvent && proc.howlEvents() > 0) firstEvent = i;
        }
        // Howl energy: output level in the last 5 s, and how much of it is pure tone at the modes.
        size_t a = n > (size_t)(5 * sr) ? n - 5 * sr : 0;
        double e = 0; for (size_t i = a; i < n; i++) e += 0.25 * (L[i] + R[i]) * (L[i] + R[i]);
        e /= (n - a);
        std::printf("loop gain %.3f: notch events %d", G, proc.howlEvents());
        if (firstEvent) std::printf(" (first at %.2f s)", (double)firstEvent / sr);
        std::printf(", notches:");
        for (int i = 0; i < rasp::HowlGuard::kMaxNotch; i++) if (proc.howlNotchHz(i) > 0) std::printf(" %.0f Hz", proc.howlNotchHz(i));
        std::printf("\noutput level, last 5 s: %.1f dBFS RMS\n", 10 * std::log10(e + 1e-20));
        writeWav(argv[3], L, R, sr);
        return 0;
    }

    if (mode == "falsepos") {
        for (size_t i = 0; i < n; i++) proc.process(in[i], L[i], R[i]);
        std::printf("%d notch events", proc.howlEvents());
        for (int i = 0; i < rasp::HowlGuard::kMaxNotch; i++) if (proc.howlNotchHz(i) > 0) std::printf(" %.0f Hz", proc.howlNotchHz(i));
        std::printf("\n");
        return 0;
    }
    return usage();
}
