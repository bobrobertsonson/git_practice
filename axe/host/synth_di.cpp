// Synthetic guitar and bass DIs for testing, until real ones arrive.
//
// Each string is an extended Karplus-Strong model: a delay line of one period
// with a lowpass loss filter (its brightness is what "fresh" vs "dull"
// strings changes), a fractional delay for bends and vibrato, a pick
// excitation shaped by pick hardness and pick position, and palm muting as
// extra damping. The strings go through a magnetic-pickup resonance.
// Not a substitute for a real DI, but it has real chords (several notes
// whose partials interleave), real decays, attacks and palm mutes.
//
//   ./synth_di outdir          -> writes the test set into outdir/
#include "strings.h"
#include <string>

namespace {
using namespace strings;

void render(const std::string& path, const std::vector<Note>& notes, const StringSet& s, float seconds, float level) {
    std::vector<float> out((size_t)(seconds * kSR), 0.f);
    Rng rng;
    for (const auto& n : notes) pluck(out, n, s, rng);
    pickup(out, s.bass, level);
    writeWav(path.c_str(), out, (int)kSR);
    std::printf("wrote %s (%.1f s)\n", path.c_str(), seconds);
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "renders/di";
    const StringSet fresh{0.85f, 3.2f, false}, dull{0.05f, 2.0f, false};
    const StringSet bassFresh{0.75f, 4.0f, true}, bassDull{0.05f, 2.6f, true};
    const float lvl = 0.35f;   // peaks near -9 dBFS, a typical DI

    // Guitar chords (standard tuning): open E, G, C, D, Am, E5, A5, F barre, Cmaj7
    std::vector<Note> ch;
    float t = 0.2f;
    chord(ch, t, {40, 47, 52, 56, 59, 64}, 1.8f); t += 2.f;    // E
    chord(ch, t, {43, 47, 50, 55, 59, 67}, 1.8f); t += 2.f;    // G
    chord(ch, t, {48, 52, 55, 60, 64}, 1.8f); t += 2.f;        // C
    chord(ch, t, {50, 57, 62, 66}, 1.8f); t += 2.f;            // D
    chord(ch, t, {45, 52, 57, 60, 64}, 1.8f); t += 2.f;        // Am
    chord(ch, t, {40, 47, 52}, 1.8f); t += 2.f;                // E5
    chord(ch, t, {45, 52, 57}, 1.8f); t += 2.f;                // A5
    chord(ch, t, {41, 48, 53, 57, 60, 65}, 1.8f); t += 2.f;    // F barre
    chord(ch, t, {48, 52, 55, 59, 64}, 1.8f); t += 2.f;        // Cmaj7
    render(dir + "/guitar_chords.wav", ch, fresh, t + 0.5f, lvl);

    // Palm-muted chugs on the low E with power-chord accents, 16ths at 140 bpm
    std::vector<Note> pm;
    t = 0.2f;
    const float s16 = 60.f / 140.f / 4.f;
    for (int bar = 0; bar < 4; bar++) {
        for (int i = 0; i < 16; i++) {
            bool accent = (i == 0 || i == 6 || i == 12);
            if (accent) chord(pm, t, {40, 47, 52}, s16 * 2.f, 0.95f, 0.f);
            else pm.push_back({t, midiHz(40), s16 * 0.9f, 0.85f, 0.85f});
            t += s16;
        }
    }
    render(dir + "/guitar_palm.wav", pm, fresh, t + 0.5f, lvl);

    // Single notes: a minor-pentatonic line, a bend and a held note with vibrato
    std::vector<Note> sn;
    t = 0.2f;
    for (int m : {52, 55, 57, 59, 62, 64, 62, 59, 57, 55}) { sn.push_back({t, midiHz(m), 0.32f, 0.8f}); t += 0.35f; }
    sn.push_back({t, midiHz(57), 1.2f, 0.85f, 0.f, 2.f}); t += 1.4f;          // whole-step bend
    sn.push_back({t, midiHz(64), 2.0f, 0.85f, 0.f, 0.f, 0.25f}); t += 2.2f;    // vibrato
    sn.push_back({t, midiHz(40), 2.0f, 0.9f}); t += 2.2f;                      // low E
    render(dir + "/guitar_single.wav", sn, fresh, t + 0.5f, lvl);

    // Fresh vs dull strings: the same riff for an A/B, plus a different riff
    // on dull strings (Learn is taught on one riff and matched on another).
    std::vector<Note> riff;
    t = 0.2f;
    for (int rep = 0; rep < 3; rep++) {
        chord(riff, t, {40, 47, 52}, 0.6f); t += 0.7f;
        chord(riff, t, {43, 50, 55}, 0.6f); t += 0.7f;
        for (int m : {52, 55, 57, 59}) { riff.push_back({t, midiHz(m), 0.25f, 0.8f}); t += 0.28f; }
        chord(riff, t, {45, 52, 57, 60, 64}, 1.0f); t += 1.2f;
    }
    render(dir + "/guitar_fresh.wav", riff, fresh, t + 0.5f, lvl);
    render(dir + "/guitar_dull.wav", riff, dull, t + 0.5f, lvl);
    std::vector<Note> riff2;
    t = 0.2f;
    for (int rep = 0; rep < 3; rep++) {
        chord(riff2, t, {43, 47, 50, 55, 59, 67}, 1.0f); t += 1.1f;
        chord(riff2, t, {50, 57, 62, 66}, 1.0f); t += 1.1f;
        for (int m : {62, 64, 67, 69, 67, 64}) { riff2.push_back({t, midiHz(m), 0.22f, 0.75f}); t += 0.25f; }
    }
    render(dir + "/guitar_dull_riff2.wav", riff2, dull, t + 0.5f, lvl);

    // Bass: a line, octaves, double stops, a held low E
    std::vector<Note> bs;
    t = 0.2f;
    for (int m : {28, 28, 31, 33, 28, 40, 35, 33}) { bs.push_back({t, midiHz(m), 0.4f, 0.85f}); t += 0.45f; }
    for (int i = 0; i < 8; i++) { bs.push_back({t, midiHz(28), 0.18f, 0.8f, 0.8f}); t += 0.2f; }   // muted chugs
    chord(bs, t, {28, 35}, 1.5f); t += 1.7f;           // fifth
    chord(bs, t, {33, 45}, 1.5f); t += 1.7f;           // octave
    chord(bs, t, {38, 42, 45}, 1.5f); t += 1.7f;       // D major triad up the neck
    bs.push_back({t, midiHz(28), 2.5f, 0.9f}); t += 2.7f;
    render(dir + "/bass_line.wav", bs, bassFresh, t + 0.5f, lvl);
    render(dir + "/bass_dull.wav", bs, bassDull, t + 0.5f, lvl);
    return 0;
}
