// Desktop tool: process a WAV file through the AxeElevator engine.
//   ./axe_wav in.wav out.wav mode=buzzsaw inst=guitar gain=0.8
//   ./axe_wav riff.wav out.wav mode=glitch tempo=140 rate=1/16 trigat=2,4,6,8   (Trigger on 2-4 s and 6-8 s)
#include "common.h"

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
    std::vector<size_t> toggles;   // sample positions where Trigger flips
    for (int i = 3; i < argc; i++) {
        if (!std::strncmp(argv[i], "trigat=", 7)) {
            for (const char* c = argv[i] + 7; *c;) {
                toggles.push_back((size_t)(std::atof(c) * sr));
                while (*c && *c != ',') c++;
                if (*c) c++;
            }
            continue;
        }
        if (!setParam(p, argv[i])) { std::fprintf(stderr, "unknown parameter: %s\n", argv[i]); printParams(); return 1; }
    }
    static axe::Processor proc;   // static: keeps the engine's buffers off the stack
    proc.init((float)sr);
    proc.setParams(p);
    proc.reset();
    std::vector<float> out(in.size());
    const int block = 64;
    size_t nextToggle = 0;
    for (size_t i = 0; i < in.size(); i += block) {
        while (nextToggle < toggles.size() && toggles[nextToggle] <= i) {
            p.trigger = !p.trigger;
            proc.setParams(p);
            nextToggle++;
        }
        int n = (int)std::min<size_t>(block, in.size() - i);
        proc.process(&in[i], &out[i], n);
    }
    writeWav(argv[2], out, sr);
    return 0;
}
