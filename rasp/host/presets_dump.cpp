// Prints the C++ defaults and factory presets as JSON (for tools/check_presets.py).
#include "../dsp/presets.h"
#include <cstdio>

static void dump(const rasp::Params& p) {
#define F(n) std::printf("\"" #n "\": %.6g, ", (double)p.n)
#define B(n) std::printf("\"" #n "\": %s, ", p.n ? "true" : "false")
    std::printf("{");
    B(controlOn); F(loCut); F(hiCut); F(drive); F(brightness); F(mix); F(chop); F(release); F(deEss); F(airDb); F(weight); F(fill); F(smooth); F(bodyDb); F(matchAmount); F(octDown); F(octUp); F(pitch1Semi); F(pitch2Semi); F(harm3Level); F(harm4Level); F(harm3Semi); F(harm4Semi); F(harm1Width); F(harm2Width); F(harm3Width); F(harm4Width); F(harmRandPitch); F(harmRandTime); F(harmLoCut); F(harmHiCut); F(harmWidth); F(bleed);
    F(controlGainDb); B(thickenOn); F(spread); F(diffuse); F(darken); F(width); B(solo); B(doubleTrack); F(thickenGainDb);
    B(flavourOn); F(tail); F(tailBlend); F(repeats); F(delayBlend); F(delayTime); F(bpm); F(tilt); F(duck);
    F(flavourGainDb); F(glue); F(outDb); F(inDb); B(bypass); B(bypassMatch); B(mono); B(mega); B(throwOn); B(stutter); B(glitch); F(stutterDiv); F(glitchAmount); F(glitchPattern); B(tapeStop); F(tapeTime); B(freeze); B(gate); F(gatePattern); F(throwStyle); F(crush); F(crushMix); F(lofi);
    std::printf("\"howl\": %s}", p.howl ? "true" : "false");
#undef F
#undef B
}

int main() {
    std::printf("{\"matchTarget\": [");
    for (int b = 0; b < rasp::kMatchBands; b++) std::printf("%s%.6g", b ? ", " : "", (double)rasp::kMatchTarget[b]);
    std::printf("], \"defaults\": ");
    dump(rasp::Params());
    for (int i = 0; i < rasp::kNumPresets; i++) { std::printf(", \"%s\": ", rasp::presetName(i)); dump(rasp::presetParams(i)); }
    std::printf("}\n");
}
