// Prints the C++ defaults and factory presets as JSON (for tools/check_presets.py).
#include "../dsp/presets.h"
#include <cstdio>

static void dump(const rasp::Params& p) {
#define F(n) std::printf("\"" #n "\": %.6g, ", (double)p.n)
#define B(n) std::printf("\"" #n "\": %s, ", p.n ? "true" : "false")
    std::printf("{");
    B(controlOn); F(loCut); F(hiCut); F(drive); F(brightness); F(mix); F(chop); F(release); F(deEss); F(airDb); F(octDown); F(octUp); F(bleed);
    F(controlGainDb); B(thickenOn); F(spread); F(diffuse); F(darken); F(width); B(solo); F(thickenGainDb);
    B(flavourOn); F(tail); F(tailBlend); F(repeats); F(delayBlend); F(delayTime); F(bpm); F(tilt); F(duck);
    F(flavourGainDb); F(glue); F(outDb); F(inDb); B(bypass); B(bypassMatch); B(mono); B(mega); B(throwOn);
    std::printf("\"howl\": %s}", p.howl ? "true" : "false");
#undef F
#undef B
}

int main() {
    std::printf("{\"defaults\": ");
    dump(rasp::Params());
    for (int i = 0; i < rasp::kNumPresets; i++) { std::printf(", \"%s\": ", rasp::presetName(i)); dump(rasp::presetParams(i)); }
    std::printf("}\n");
}
