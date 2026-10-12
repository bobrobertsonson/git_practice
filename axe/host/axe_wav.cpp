// Desktop tool: process a WAV file through the AxeElevator engine.
//   ./axe_wav in.wav out.wav mode=buzzsaw inst=guitar gain=0.7
//   ./axe_wav dull.wav out.wav mode=strings learnfresh=fresh.wav amount=1
#include "common.h"

// Long-term profile of a whole file (the same Learner the plugin uses).
static bool profileOf(const std::vector<float>& x, int sr, float* prof) {
    axe::Learner l;
    l.init((float)sr);
    l.setSeconds(1e6f);
    l.start();
    for (float s : x) l.feed(s, prof);
    l.result(prof);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: axe_wav in.wav out.wav [name=value ...]\n");
        printParams();
        return 1;
    }
    flushDenormals();
    std::vector<float> in;
    int sr = 0;
    if (!readWav(argv[1], in, sr)) { std::fprintf(stderr, "could not read %s\n", argv[1]); return 1; }
    axe::Params p;
    const char* freshPath = nullptr;
    bool amountGiven = false;
    for (int i = 3; i < argc; i++) {
        if (!std::strncmp(argv[i], "learnfresh=", 11)) { freshPath = argv[i] + 11; continue; }
        if (!std::strncmp(argv[i], "amount=", 7)) amountGiven = true;
        if (!setParam(p, argv[i])) { std::fprintf(stderr, "unknown parameter: %s\n", argv[i]); printParams(); return 1; }
    }
    if (freshPath) {
        std::vector<float> fr;
        int fsr = 0;
        if (!readWav(freshPath, fr, fsr)) { std::fprintf(stderr, "could not read %s\n", freshPath); return 1; }
        float fresh[axe::kMatchBands], now[axe::kMatchBands];
        profileOf(fr, fsr, fresh);
        profileOf(in, sr, now);
        axe::matchGains(fresh, now, p.match);
        if (!amountGiven) p.matchAmount = 1.f;
        std::fprintf(stderr, "learned match (dB):");
        for (int b = 0; b < axe::kMatchBands; b++) std::fprintf(stderr, " %.0fHz:%+.1f", axe::kMatchHz[b], p.match[b]);
        std::fprintf(stderr, "\n");
    }
    static axe::Processor proc;   // static: keeps the engine's buffers off the stack
    proc.init((float)sr);
    proc.setParams(p);
    proc.reset();
    std::vector<float> out(in.size());
    const int block = 256;
    for (size_t i = 0; i < in.size(); i += block) {
        int n = (int)std::min<size_t>(block, in.size() - i);
        proc.process(&in[i], &out[i], n);
    }
    writeWav(argv[2], out, sr);
    return 0;
}
