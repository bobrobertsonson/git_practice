// Desktop test harness: run a WAV file through the RASP DSP core.
//   rasp_wav in.wav out.wav [preset=name] [name=value ...]
// Reads 16-bit PCM or 32-bit float WAV (stereo is downmixed), writes 16-bit stereo.
#include "common.h"

int main(int argc, char** argv) {
    flushDenormals();
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s in.wav out.wav [preset=name] [name=value ...]\n"
            "  e.g. %s scream.wav out.wav preset=dark in=8 chop=0.6\n"
            "  presets: huge natural dark bright live raw (put preset= first)\n"
            "  names: locut hicut drive bright(0-2) mix chop release\n"
            "         spread diffuse darken width solo tail tailblend repeats delayblend time(0-2)\n"
            "         bpm tilt duck control thicken flavour (0/1) *gain out (dB)\n"
            "         deess bleed glue in (dB)  bypass bypassmatch mono howl (0/1)\n", argv[0], argv[0]);
        return 1;
    }
    std::vector<float> buf; int sr = 0;
    if (!readWav(argv[1], buf, sr)) { std::fprintf(stderr, "could not read %s\n", argv[1]); return 1; }

    rasp::Params p;
    for (int i = 3; i < argc; ++i)
        if (!setParam(p, argv[i])) { std::fprintf(stderr, "unknown parameter: %s\n", argv[i]); return 1; }

    // Processor holds ~1.6 MB of delay lines: keep it off the stack.
    static rasp::Processor proc;
    proc.init((float)sr);
    proc.setParams(p);
    buf.resize(buf.size() + sr * 2, 0.f);  // 2 s of tail for the reverb/delay
    std::vector<float> L(buf.size()), R(buf.size());
    for (size_t i = 0; i < buf.size(); ++i) proc.process(buf[i], L[i], R[i]);
    writeWav(argv[2], L, R, sr);
    std::printf("processed %zu samples @ %d Hz -> %s (stereo)\n", buf.size(), sr, argv[2]);
    return 0;
}
