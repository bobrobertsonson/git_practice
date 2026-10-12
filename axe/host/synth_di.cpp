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
#include "common.h"
#include <cmath>
#include <string>

namespace {

struct Rng {
    uint32_t s = 0x12345678u;
    float next() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFFFF) / 8388608.f - 1.f; }
};

constexpr float kSR = 48000.f;

struct Note {
    float t;          // start time (s)
    float hz;         // pitch
    float dur;        // until damped (s)
    float vel;        // 0..1
    float mute = 0.f; // palm mute 0..1
    float bend = 0.f; // semitones reached at the end of the note
    float vib = 0.f;  // vibrato depth (semitones)
};

struct StringSet {
    float bright;     // 0 dull .. 1 fresh
    float t60;        // open-string decay (s) at 110 Hz
    bool bass;
};

// Renders one note into out (added).
void pluck(std::vector<float>& out, const Note& n, const StringSet& s, Rng& rng) {
    const int start = (int)(n.t * kSR);
    const int len = (int)((n.dur + 0.4f) * kSR);
    const int maxD = 4096;
    std::vector<float> dl(maxD, 0.f);
    float period = kSR / n.hz;
    // excitation: the string's initial shape, a triangle peaking at the pick
    // position (harmonics fall ~12 dB/octave, with the pick-position nulls),
    // softened by pick hardness, plus a little pick noise
    const int exLen = (int)period;
    const float pos = s.bass ? 0.2f : 0.15f;
    float hard = (0.15f + 0.5f * n.vel) * (0.15f + 0.85f * s.bright) * (1.f - 0.6f * n.mute);
    std::vector<float> ex(exLen);
    float lp = 0.f, mean = 0.f;
    for (int i = 0; i < exLen; i++) {
        float u = (float)i / (float)exLen;
        float tri = u < pos ? u / pos : (1.f - u) / (1.f - pos);
        float v = tri + 0.04f * rng.next();
        lp += hard * (v - lp);
        ex[i] = lp; mean += lp;
    }
    mean /= (float)exLen;
    for (int i = 0; i < exLen; i++) dl[i] = (ex[i] - mean) * n.vel;
    // loss filter: a one-pole lowpass in the loop; dull strings lose their
    // highs much faster per period, and decay sooner overall
    float pole = 0.05f + 0.75f * (1.f - s.bright);
    pole += n.mute * (0.85f - pole);
    float t60 = s.t60 * (110.f / n.hz) * 0.6f + s.t60 * 0.4f;
    t60 *= (1.f - 0.93f * n.mute);
    float g = std::pow(10.f, -3.f / (t60 * n.hz));
    // the lowpass also loses a little at the fundamental: compensate
    {
        float w0 = 2.f * axe::kPi * n.hz / kSR;
        float mag = (1.f - pole) / std::sqrt(1.f - 2.f * pole * std::cos(w0) + pole * pole);
        g /= mag;
    }
    int w = exLen % maxD;
    float x1 = 0.f, damp = 1.f;
    for (int i = 0; i < len; i++) {
        float tn = (float)i / kSR;
        float semis = n.bend * std::fmin(tn / std::fmax(n.dur * 0.5f, 0.05f), 1.f) + n.vib * std::sin(2.f * axe::kPi * 5.5f * tn) * std::fmin(tn / 0.4f, 1.f);
        float d = period * std::pow(2.f, -semis / 12.f) - pole / (1.f - pole);   // minus the loop filter delay
        // read d samples back (linear interpolation)
        float rp = (float)w - d;
        while (rp < 0) rp += maxD;
        int i0 = (int)rp; float fr = rp - i0;
        float x = dl[i0 % maxD] * (1.f - fr) + dl[(i0 + 1) % maxD] * fr;
        x1 = x + pole * (x1 - x);
        float y = g * x1;
        if (tn > n.dur) damp *= 0.9985f;     // fret hand releases: fast damping
        y *= (tn > n.dur) ? 0.995f : 1.f;
        dl[w] = y;
        w = (w + 1) % maxD;
        size_t o = (size_t)(start + i);
        if (o < out.size()) out[o] += y * damp;
    }
}

// Magnetic pickup + cable: resonant lowpass, and a DC block.
void pickup(std::vector<float>& x, bool bass, float level) {
    axe::Biquad res, hp;
    res.lowpass(bass ? 2200.f : 3800.f, bass ? 1.2f : 2.2f, kSR);
    hp.highpass(bass ? 25.f : 50.f, 0.7071f, kSR);
    float peak = 1e-9f;
    for (auto& v : x) { v = hp.process(res.process(v)); peak = std::fmax(peak, std::fabs(v)); }
    float g = level / peak;
    for (auto& v : x) v *= g;
}

float midiHz(int m) { return 440.f * std::pow(2.f, (m - 69) / 12.f); }

// Strum a chord: MIDI notes, low to high, 12 ms between strings.
void chord(std::vector<Note>& ns, float t, std::initializer_list<int> notes, float dur, float vel = 0.8f, float mute = 0.f, bool up = false) {
    int i = 0, n = (int)notes.size();
    for (int m : notes) {
        int k = up ? (n - 1 - i) : i;
        ns.push_back({t + 0.012f * k, midiHz(m), dur, vel * (0.85f + 0.15f * ((k * 7) % 3) / 2.f), mute});
        i++;
    }
}

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
