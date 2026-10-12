// What RASP's Learn hears in a take, and the Match EQ it would set.
//   learn_tool take.wav [more.wav ...]      deviation (dB re 1 kHz) per band, then the EQ gains
//   learn_tool --target ref1.wav ref2.wav   the median deviation over whole takes: the kMatchTarget
//                                           table in dsp/rasp.h (the presets' reference tone)
// Takes get the same input gain as takes.py (loud-section RMS to -18 dBFS).
#include "common.h"
#include <algorithm>

static rasp::Learner learner;

static bool measure(const char* path, float seconds, float* dev) {
    std::vector<float> x; int sr = 0;
    if (!readWav(path, x, sr)) { std::fprintf(stderr, "could not read %s\n", path); return false; }
    std::vector<float> lv;
    const size_t n = (size_t)sr / 10;
    for (size_t s = 0; s + n <= x.size(); s += n) {
        double e = 0; for (size_t k = s; k < s + n; k++) e += (double)x[k] * x[k];
        double d = 10 * std::log10(e / n + 1e-12); if (d > -45) lv.push_back((float)d);
    }
    std::sort(lv.begin(), lv.end());
    float g = lv.empty() ? 1.f : std::pow(10.f, (-18.f - lv[lv.size() * 9 / 10]) / 20.f);
    learner.init((float)sr);
    learner.setSeconds(seconds);
    learner.start();
    for (float v : x) if (learner.feed(v * g, dev)) return true;
    learner.result(dev);                       // the take ended first: use what was heard
    return learner.secondsHeard() >= 15.f;
}

int main(int argc, char** argv) {
    flushDenormals();
    if (argc < 2) { std::fprintf(stderr, "usage: learn_tool [--target] take.wav ...\n"); return 1; }
    bool target = std::string(argv[1]) == "--target";
    std::printf("%-28s", "dB re 1 kHz");
    for (float hz : rasp::kMatchHz) std::printf("%7.0f", hz);
    std::printf("\n");
    std::vector<std::vector<float>> all;
    for (int i = target ? 2 : 1; i < argc; i++) {
        float dev[rasp::kMatchBands];
        // --target: the whole take (up to 10 min of singing); otherwise what Learn hears (15 s).
        if (!measure(argv[i], target ? 600.f : 15.f, dev) && !target) {
            std::printf("%-28s less than 15 s of singing\n", argv[i]);
            continue;
        }
        std::string name(argv[i]); name = name.substr(name.find_last_of('/') + 1).substr(0, 27);
        std::printf("%-28s", name.c_str());
        for (float d : dev) std::printf("%7.1f", d);
        std::printf("\n");
        all.push_back(std::vector<float>(dev, dev + rasp::kMatchBands));
        if (!target) {
            float g[rasp::kMatchBands];
            rasp::matchGainsFromDeviation(dev, g);
            std::printf("%-28s", "  -> Match EQ gains");
            for (float v : g) std::printf("%7.1f", v);
            std::printf("\n");
        }
    }
    if (target && !all.empty()) {
        std::printf("\nconstexpr float kMatchTarget[kMatchBands] = {");
        for (int b = 0; b < rasp::kMatchBands; b++) {
            std::vector<float> col; for (auto& r : all) col.push_back(r[b]);
            std::sort(col.begin(), col.end());
            float med = col.size() % 2 ? col[col.size() / 2] : 0.5f * (col[col.size() / 2 - 1] + col[col.size() / 2]);
            std::printf("%s%.1ff", b ? ", " : "", med);
        }
        std::printf("};\n");
    }
    return 0;
}
