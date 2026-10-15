// Tests for class Vox (vocoder / robot / talkbox / formant) in dsp/rasp.h.
//   make vox_test && ./vox_test
#include "../dsp/rasp.h"
#include <cstdio>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }
static float rmsDb(const std::vector<float>& x, size_t from) {
    double s = 0; for (size_t i = from; i < x.size(); i++) s += (double)x[i] * x[i];
    return 10.f * (float)std::log10(s / (double)(x.size() - from) + 1e-20);
}

int main() {
    const float sr = 48000.f; const size_t n = (size_t)(3 * sr);
    // A sung-like voice: a 150 Hz saw with 5 Hz vibrato and a syllable envelope.
    std::vector<float> voice(n); double ph = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = i / (double)sr;
        ph += 150.0 * (1.0 + 0.01 * std::sin(2 * M_PI * 5 * t)) / sr; ph -= std::floor(ph);
        voice[i] = (float)((2 * ph - 1) * 0.1 * (0.5 + 0.5 * std::pow(std::sin(2 * M_PI * 2 * t), 2)));
    }
    const float inDb = rmsDb(voice, n / 3);

    bool finite = true, level = true;
    for (int mode = 1; mode <= 4; mode++)
        for (int chord = 0; chord <= 5; chord += 5)
            for (float ring : {0.f, 0.7f}) {
                rasp::Vox v; v.init(sr); v.set(mode, 7, chord, false, 0.3f, ring);
                std::vector<float> y(n);
                for (size_t i = 0; i < n; i++) { y[i] = v.process(voice[i]); finite = finite && std::isfinite(y[i]); }
                const float d = rmsDb(y, n / 3) - inDb;
                if (std::fabs(d) > 3.f) { level = false; std::printf("      mode %d chord %d ring %.1f: %+.1f dB\n", mode, chord, ring, d); }
            }
    check(finite, "every mode / chord / ring setting stays finite");
    check(level, "every mode follows the input's loudness within 3 dB");

    {
        rasp::Vox v; v.init(sr); v.set(1, 7, 0, true, 0.5f, 0.f);
        for (size_t i = 0; i < n; i++) v.process(voice[i]);
        const float hz = v.pitchHz();
        std::printf("      follow: %.1f Hz\n", hz);
        check(std::fabs(hz - 146.83f) < 0.5f, "follow snaps a 150 Hz voice to D3 (146.8 Hz)");
    }
    {
        rasp::Vox v; v.init(sr); v.set(1, 7, 0, false, 0.5f, 0.f);
        for (size_t i = 0; i < n / 4; i++) v.process(voice[i]);
        check(std::fabs(v.pitchHz() - 98.0f) < 0.2f, "a fixed note plays G2 (98 Hz)");
    }
    {
        // Silence in, silence out (no hum or buzz without a voice).
        rasp::Vox v; v.init(sr); v.set(1, 7, 5, false, 0.5f, 0.f);
        float pk = 0.f; for (size_t i = 0; i < n; i++) pk = std::max(pk, std::fabs(v.process(0.f)));
        check(pk < 1e-6f, "no voice, no carrier");
    }
    {
        // Off: the processor's output is unchanged by the vox settings.
        rasp::Params a, b; b.voxNote = 3; b.voxChord = 4; b.voxRing = 0.5f; b.voxFormant = 0.2f;
        static rasp::Processor pa, pb;                  // static: too big for the stack
        pa.init(sr); pb.init(sr); pa.setParams(a); pb.setParams(b);
        bool same = true;
        for (size_t i = 0; i < n / 3; i++) { float l1, r1, l2, r2; pa.process(voice[i], l1, r1); pb.process(voice[i], l2, r2); same = same && l1 == l2 && r1 == r2; }
        check(same, "vox off: output is bit-identical whatever its settings");
    }
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
