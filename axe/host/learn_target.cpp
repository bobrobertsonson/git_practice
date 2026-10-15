// Measures a DI the way Tone Match's Learner does and prints it as a Target
// row for dsp/axe_targets.h, so real DIs can become measured presets.
//   ./learn_target di.wav [more.wav ...]
#include "target_models.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: learn_target di.wav [...]\n"); return 1; }
    for (int a = 1; a < argc; a++) {
        std::vector<float> x; int sr = 0;
        if (!readWav(argv[a], x, sr)) { std::fprintf(stderr, "can't read %s\n", argv[a]); continue; }
        float p[8];
        targets::profile(x, (float)sr, p);
        std::printf("    {");
        for (int b = 0; b < 8; b++) std::printf("%s%.2ff", b ? ", " : "", p[b]);
        std::printf("},   // %s\n", argv[a]);
    }
    return 0;
}
