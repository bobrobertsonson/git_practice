// Tests for the live features, on real or synthetic audio.
//
//   live_test bypass   in.wav out.wav [name=value ...]   toggle bypass every 6 s after 10 s
//   live_test feedback in.wav out.wav gain [name=value ...]
//            closed loop: the mic hears the voice plus the speaker through a
//            resonant room (modes at 1.3 and 2.9 kHz, 10 ms away)
//   live_test falsepos in.wav [name=value ...]        how many notches a take triggers
//
// Prints numbers; the WAVs are for listening and for tools that measure them.
#include "common.h"
#include <cmath>

static rasp::Processor proc;   // ~1.5 MB: keep it off the stack

static int usage() {
    std::fprintf(stderr, "usage: live_test bypass|feedback|falsepos ... (see source)\n");
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
