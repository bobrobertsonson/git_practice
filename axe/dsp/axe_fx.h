// AxeElevator triggered / tempo modes: Glitch, Laser, Dive Bomb, Chiptune,
// Stutter Gate, Ring Shredder, Freeze, Feedback, Swarm, Radio.
//
// Every mode reads a shared FxContext that the Processor fills once per
// sample: the input history (CaptureBuffer), the tempo clock (step ticks at
// the Rate division, with swing), the Trigger state and its edges, and what
// the input is doing (envelope, pick onsets, pitch).
//
// Trigger and Engage (global controls):
//   Engage = Always           the mode runs; Trigger adds the mode's action
//                             (machine-gun lasers, a held glitch, a dive, ...)
//   Engage = While Triggered  the mode only sounds while Trigger is held
// Each mode's class comment says what its Trigger does.
#pragma once
#include "axe_base.h"

namespace axe {

// Seeded generator: "random" that is the same every time after reset().
struct Rng {
    uint32_t s = 0x9E3779B9u;
    void seed(uint32_t v) { s = v ? v : 0x9E3779B9u; }
    uint32_t nextU() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni() { return (float)(nextU() >> 8) * (1.f / 16777216.f); }     // 0..1
    float bi() { return uni() * 2.f - 1.f; }                                 // -1..1
    int pick(int n) { return (int)(uni() * (float)n) % n; }
};

// The input history every mode can read, by absolute sample index.
// 2^19 samples (~10.9 s at 48 kHz; the plugin's AXE_SR_SCALE 2 keeps ~5.4 s at 192 kHz).
class CaptureBuffer {
public:
    static constexpr int kSize = (1 << 19) * AXE_SR_SCALE;
    void reset() {
        if (dirty) for (auto& v : buf) v = 0.f;
        dirty = false; n = 0;
    }
    void write(float x) { buf[n & (kSize - 1)] = x; n++; dirty = true; }
    uint64_t pos() const { return n; }                   // index of the next write
    // Linear interpolation at an absolute position; anything older than the
    // buffer (or not yet written) reads as silence.
    float read(double p) const {
        if (p < 0.0) return 0.f;
        const uint64_t i = (uint64_t)p;
        if (i + 1 >= n || n - i > (uint64_t)kSize - 2) return 0.f;
        const float f = (float)(p - (double)i);
        const float a = buf[i & (kSize - 1)], b = buf[(i + 1) & (kSize - 1)];
        return a + (b - a) * f;
    }
private:
    float buf[(size_t)kSize] = {};
    uint64_t n = 0;
    bool dirty = false;
};

struct FxContext {
    float sr = 48000.f;
    const CaptureBuffer* cap = nullptr;
    int instrument = kGuitar;
    // clock
    float bpm = 120.f;
    double beats = 0.0;          // position on the grid
    double stepBeats = 0.25;     // one Rate step, in beats
    float stepSamples = 6000.f;
    int step = 0;                // step counter (swing applied)
    bool tick = false;           // a new step starts on this sample
    // trigger
    bool always = true;          // Engage = Always
    bool trig = false;           // Trigger held
    bool trigEdge = false;       // Trigger went on at this sample
    bool trigFall = false;       // Trigger went off at this sample
    // input analysis
    float env = 0.f;             // peak envelope (linear)
    bool onset = false;          // a pick attack at this sample
    float onsetLevel = 0.f;
    float pitch = 0.f;           // Hz, 0 = not known
    float pitchConf = 0.f;
    float beatsToSamples(double b) const { return (float)(b * 60.0 / (double)bpm * (double)sr); }
};

// Band-limited pulse helper (polyBLEP).
inline float polyBlep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
}
// Pulse wave of width w (0..1) at phase t (0..1), increment dt.
inline float pulseWave(float t, float dt, float w) {
    float y = t < w ? 1.f : -1.f;
    y += polyBlep(t, dt);
    float t2 = t - w; if (t2 < 0.f) t2 += 1.f;
    y -= polyBlep(t2, dt);
    return y - (2.f * w - 1.f);   // remove the DC of an uneven pulse
}

// ---------------------------------------------------------------------------
// Pitch tracker: YIN on a ~12 kHz decimated copy, its work spread evenly over
// samples (no spikes for small audio blocks). A new estimate every ~10 ms.
// Ranges: guitar 50..1400 Hz (drop A), bass 28..700 Hz (5-string low B).
// ---------------------------------------------------------------------------
class PitchTracker {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        dec = (int)std::lround(sr / 12000.f); if (dec < 1) dec = 1;
        srD = sr / (float)dec;
        aa[0].lowpass(srD * 0.4f, 0.5412f, sr); aa[1].lowpass(srD * 0.4f, 1.3066f, sr);
        setInstrument(kGuitar);
        reset();
    }
    // The instrument's range, narrowed to [lo, hi] Hz when Learn My Playing
    // has measured yours (0 = no limit): fewer octave mistakes.
    void setInstrument(int inst, float lo = 0.f, float hi = 0.f) {
        float fmin = inst == kBass ? 28.f : 40.f, fmax = inst == kBass ? 700.f : 1400.f;   // guitar down to 8-string / drop-A territory
        if (lo > 0.f && hi > lo) { fmin = std::fmax(fmin, lo); fmax = std::fmin(fmax, hi); if (fmax < fmin * 1.5f) fmax = fmin * 1.5f; }
        tauMax = (int)(srD / fmin); if (tauMax > kW) tauMax = kW;
        tauMin = (int)(srD / fmax); if (tauMin < 2) tauMin = 2;
    }
    void reset() {
        for (auto& v : ring) v = 0.f;
        for (auto& b : aa) b.reset();
        w = 0; ph = 0; hopCount = 0; tauNext = kW + 1; hz = 0.f; conf = 0.f;
    }
    void process(float x) {
        const float xa = aa[1].process(aa[0].process(x));
        if (++ph < dec) return;
        ph = 0;
        ring[w] = xa; w = (w + 1) & (kRing - 1);
        // start a frame every kHop decimated samples
        if (++hopCount >= kHop) {
            hopCount = 0;
            for (int i = 0; i < kFrame; i++) frame[i] = ring[(w - kFrame + i) & (kRing - 1)];
            tauNext = 1;
            dsum = 0.f;
        }
        // spread the difference function over the hop
        const int perSample = (tauMax + kHop - 1) / kHop + 1;
        for (int k = 0; k < perSample && tauNext <= tauMax; k++, tauNext++) {
            float d = 0.f;
            for (int j = 0; j < kW; j++) { float e = frame[j] - frame[j + tauNext]; d += e * e; }
            diff[tauNext] = d;
        }
        if (tauNext == tauMax + 1) { finish(); tauNext = tauMax + 2; }
    }
    float pitchHz() const { return hz; }
    float confidence() const { return conf; }
private:
    void finish() {
        // cumulative mean normalised difference, first dip under the threshold
        float run = 0.f, energy = 0.f;
        for (int j = 0; j < kW; j++) energy += frame[j] * frame[j];
        if (energy < 1e-7f) { conf = 0.f; return; }
        int best = -1;
        for (int t = 1; t <= tauMax; t++) {
            run += diff[t];
            cmnd[t] = diff[t] * (float)t / (run + 1e-20f);
        }
        for (int t = tauMin; t < tauMax; t++) {
            if (cmnd[t] < 0.15f) {
                while (t + 1 < tauMax && cmnd[t + 1] < cmnd[t]) t++;
                best = t; break;
            }
        }
        if (best < 0) {   // no dip under the threshold: take the deepest one if it's clear enough
            float m = 0.35f;
            for (int t = tauMin; t < tauMax; t++) if (cmnd[t] < m && cmnd[t] <= cmnd[t - 1] && cmnd[t] <= cmnd[t + 1]) { m = cmnd[t]; best = t; }
            if (best < 0) { conf = 0.f; return; }
        }
        float tau = (float)best;
        if (best > 1 && best < tauMax) {   // parabolic interpolation
            float a = cmnd[best - 1], b = cmnd[best], c = cmnd[best + 1], den = a - 2.f * b + c;
            if (std::fabs(den) > 1e-9f) tau += 0.5f * (a - c) / den;
        }
        hz = srD / tau;
        conf = clampf(1.f - cmnd[best], 0.f, 1.f);
    }
    static constexpr int kW = 512, kFrame = 2 * kW + 4, kRing = 2048, kHop = 120;
    float sr = 48000.f, srD = 12000.f, hz = 0.f, conf = 0.f, dsum = 0.f;
    int dec = 4, ph = 0, w = 0, hopCount = 0, tauMin = 8, tauMax = 200, tauNext = kW + 1;
    Biquad aa[2];
    float ring[kRing] = {}, frame[kFrame] = {}, diff[kW + 2] = {}, cmnd[kW + 2] = {};
};

// Pick attacks: a fast envelope jumping well above a slow one.
// (Learn My Playing sets the jump, the hold time and the floor.)
class OnsetDetector {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        fast.set(0.5f, 20.f, sr); slow.set(30.f, 250.f, sr);
        configure(8.f, 60.f, -55.f);
        reset();
    }
    void configure(float jumpDb, float holdMs, float floorDb) {
        jump = clampf(jumpDb, 3.f, 20.f);
        refractory = (int)(clampf(holdMs, 10.f, 500.f) * 0.001f * sr);
        floor = clampf(floorDb, -100.f, -20.f);
    }
    void reset() { fast.reset(); slow.reset(); hold = 0; }
    bool process(float x, float& level) {
        const float f = fast.process(x), s = slow.process(x);
        if (hold > 0) { hold--; return false; }
        if (f > floor && f - s > jump) { hold = refractory; level = fastDbToLin(f); return true; }
        return false;
    }
private:
    Follower fast, slow;
    float sr = 48000.f, jump = 8.f, floor = -55.f;
    int hold = 0, refractory = 2880;
};

// ---------------------------------------------------------------------------
// Learn My Playing: ~10 s of normal playing. It measures your noise floor
// (the quietest moments), how hard your pick attacks jump above the ringing
// notes, how fast you pick, and the range of notes you play, so the pick
// detector, the gate and the pitch tracker fit you instead of a guess.
// ---------------------------------------------------------------------------
struct PlayingProfile {
    float pickSense = 8.f, pickHold = 60.f, noiseFloor = -70.f, lowNote = 0.f, highNote = 0.f;
};

class PlayingLearner {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        fast.set(0.5f, 20.f, sr); slow.set(30.f, 250.f, sr);
        tick = (int)(0.01f * sr);
        need = (int)(10.f * sr);
        riseDb = 3.f / sr;
    }
    void setSeconds(float s) { need = (int)std::fmin(s * sr, 2.0e9f); }
    void start() {
        active = true; n = 0; waited = 0; t = 0; blockSum = 0.0; armed = true; noiseEst = 0.f; hold = 0; sinceOnset = -1; peakJump = 0.f; peakLeft = 0;
        nJumps = nIois = 0;
        for (auto& h : levelHist) h = 0;
        for (auto& h : noteHist) h = 0;
        fast.reset(); slow.reset();
    }
    void cancel() { active = false; }
    bool learning() const { return active; }
    float progress() const { return active ? (float)n / (float)need : 0.f; }
    bool feed(float x, float pitchHz, float pitchConf) {
        if (!active) return false;
        blockSum += (double)x * x;   // plain 10 ms blocks: a smoothed level would hide short pauses
        const float f = fast.process(x), s = slow.process(x);
        // pick attacks: the biggest jump in the 10 ms after it starts; the next
        // one only counts once the jump has fallen back (one pick, one count)
        if (sinceOnset >= 0) sinceOnset++;
        if (f - s < 0.5f) armed = true;   // fully settled: a low note's envelope ripples for a while
        noiseEst = std::fmin(s, noiseEst + riseDb);   // the quietest level lately (rises 3 dB/s)
        if (peakLeft > 0) {
            peakJump = std::fmax(peakJump, f - s);
            if (--peakLeft == 0 && nJumps < kMax) jumps[nJumps++] = peakJump;
        } else if (hold > 0) {
            hold--;
        } else if (armed && f - s > 5.f && f > noiseEst + 18.f) {   // hiss flickers are not picks
            armed = false;
            if (sinceOnset > 0 && nIois < kMax) iois[nIois++] = (float)sinceOnset / sr * 1000.f;
            sinceOnset = 0; peakJump = f - s; peakLeft = tick; hold = (int)(0.04f * sr);
        }
        if (++t >= tick) {
            t = 0;
            const int b = (int)clampf(fastPowDb10((float)(blockSum / (double)tick)) + 120.f, 0.f, 120.f);
            blockSum = 0.0;
            levelHist[b]++;
            if (pitchHz > 0.f && pitchConf > 0.7f) {
                const int note = (int)std::lround(69.f + 12.f * std::log2(pitchHz / 440.f));
                if (note >= 0 && note < 128) noteHist[note]++;
            }
        }
        // the clock starts at your first pick (press Learn, then pick up the
        // guitar); with no pick at all it gives up after 3x the time
        if (sinceOnset >= 0) ++n; else ++waited;
        if (n >= need || waited >= 3 * need) { active = false; return true; }
        return false;
    }
    void result(PlayingProfile& r) {
        // noise floor: the 5th percentile of the 50 ms level
        r.noiseFloor = clampf(histPercentile(levelHist, 121, 0.05f) - 120.f, -100.f, -30.f);
        r.pickSense = nJumps >= 4 ? clampf(0.5f * median(jumps, nJumps), 4.f, 16.f) : 8.f;
        r.pickHold = nIois >= 4 ? clampf(0.5f * percentile(iois, nIois, 0.1f), 25.f, 250.f) : 60.f;
        int notes = 0; for (int h : noteHist) notes += h;
        if (notes >= 20) {
            const float lo = histPercentile(noteHist, 128, 0.03f), hi = histPercentile(noteHist, 128, 0.97f);
            r.lowNote = 440.f * std::pow(2.f, (lo - 69.f - 2.f) / 12.f);    // a whole tone of headroom
            r.highNote = 440.f * std::pow(2.f, (hi - 69.f + 2.f) / 12.f);
        } else {
            r.lowNote = r.highNote = 0.f;
        }
    }
private:
    static float histPercentile(const int* h, int n, float q) {
        int total = 0; for (int i = 0; i < n; i++) total += h[i];
        if (total == 0) return 0.f;
        const int want = (int)(q * (float)total);
        int acc = 0;
        for (int i = 0; i < n; i++) { acc += h[i]; if (acc > want) return (float)i; }
        return (float)(n - 1);
    }
    static float percentile(float* v, int n, float q) {   // small n: insertion sort in place
        for (int i = 1; i < n; i++) { float k = v[i]; int j = i - 1; while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; } v[j + 1] = k; }
        return v[(int)(q * (float)(n - 1))];
    }
    static float median(float* v, int n) { return percentile(v, n, 0.5f); }
    static constexpr int kMax = 512;
    Follower fast, slow;
    float sr = 48000.f, peakJump = 0.f, noiseEst = 0.f, riseDb = 0.f;
    double blockSum = 0.0;
    bool active = false, armed = true;
    int n = 0, waited = 0, need = 480000, t = 0, tick = 480, hold = 0, sinceOnset = -1, peakLeft = 0, nJumps = 0, nIois = 0;
    float jumps[kMax] = {}, iois[kMax] = {};
    int levelHist[121] = {}, noteHist[128] = {};
};

// Hilbert pair for the frequency shifter: two allpass chains 90 degrees
// apart (Olli Niemitalo's coefficients; the first chain takes the extra
// one-sample delay). Measured: 90 +- 0.5 degrees from 100 Hz to 5 kHz at 48 kHz.
class Hilbert {
public:
    void reset() { for (auto& s : st) s = State{}; d1 = 0.f; }
    void process(float x, float& re, float& im) {
        static const float ka[4] = {0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f};
        static const float kb[4] = {0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f};
        float a = x, b = x;
        for (int i = 0; i < 4; i++) a = ap(st[i], ka[i] * ka[i], a);
        for (int i = 0; i < 4; i++) b = ap(st[4 + i], kb[i] * kb[i], b);
        re = d1; d1 = a; im = b;
    }
private:
    struct State { float x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    static float ap(State& s, float c, float x) {
        float y = c * (x + s.y2) - s.x2;
        s.x2 = s.x1; s.x1 = x; s.y2 = s.y1; s.y1 = y;
        return y;
    }
    State st[8];
    float d1 = 0.f;
};

// Keeps an effect about as loud as its input while you play (slow, and frozen
// in the gaps so it can't creep up and blast the next note).
struct AutoLevel {
    OnePole a, b;
    float g = 1.f, lo = 0.25f, hi = 8.f;
    void init(float sr, float lowest, float highest) { a.setCutoff(3.f, sr); b.setCutoff(3.f, sr); lo = lowest; hi = highest; g = 1.f; }
    void reset() { a.reset(); b.reset(); g = 1.f; }
    float process(float in, float out) {
        const float ei = a.lp(in * in), eo = b.lp(out * out);
        if (ei > 1e-6f) g += 0.0005f * (clampf(std::sqrt(ei / (eo + 1e-12f)), lo, hi) - g);
        return out * g;
    }
};

// A short linear fade, sample by sample (used for every on/off in the fx modes).
struct Fade {
    float v = 0.f, step = 0.002f;
    void set(float ms, float sr) { step = 1.f / clampf(ms * 0.001f * sr, 1.f, 1e9f); }
    float next(bool on) { v = on ? std::fmin(1.f, v + step) : std::fmax(0.f, v - step); return v; }
};

// ---------------------------------------------------------------------------
// GLITCH: a beat-repeat machine. When it engages it grabs the last Rate step
// of audio and replays it; every step it then decides (Chaos) whether to
// repeat, roll (half / quarter-length repeats), jump pitch, tape-stop,
// reverse, or grab fresh audio. Crush adds bit / sample-rate reduction.
// Trigger: Always = glitches on its own now and then (Chaos sets how often),
// Trigger holds a glitch. While Triggered = glitches while held.
// ---------------------------------------------------------------------------
class Glitch {
public:
    void init(float sampleRate, const FxContext* c) { sr = sampleRate; ctx = c; fade.set(3.f, sr); }
    void configure(int) { reset(); }
    void setTargets(const Params& p) {
        chaos = p.gChaos; crush = p.gCrush; jumps = p.gJumps; tapeStop = p.gTapeStop; rev = p.gReverse;
        smoothSmp = expMap(clampf(p.gSmooth, 0.f, 1.f), 0.3f, 8.f) * 0.001f * sr;
    }
    void snap() {}
    void reset() { rng.seed(0xA11CEu); fade.v = 0.f; active = false; autoLeft = 0; held = 0.f; holdCount = 0; phase = 0.0; }
    void control() {}
    float process(float x, float& dry) {
        dry = x;
        const FxContext& c = *ctx;
        // Always: now and then, glitch on its own for 1-4 steps
        if (c.tick) {
            if (autoLeft > 0) autoLeft--;
            else if (c.always && !c.trig && (c.step % 8) == 0 && rng.uni() < chaos * 0.5f) autoLeft = 1 + rng.pick(4);
        }
        const bool want = c.trig || (c.always && autoLeft > 0);
        if (want && !active) { grab(); newStep(true); }
        else if (want && c.tick) newStep(false);
        active = want;
        const float f = fade.next(want);
        if (f <= 0.f) return x;
        // play the slice
        float rate = sliceRate;
        if (tapeStopping) rate = sliceRate * clampf(1.f - (float)phase / (float)len, 0.f, 1.f);
        const double pos = phase;
        phase += rate;
        if (phase >= len) phase -= len;   // a roll loops inside its step
        double rp = reversed ? (double)len - pos : pos;
        float g = c.cap->read(start + rp);
        // de-click at the slice edges
        const float edge = std::fmin((float)pos, (float)(len - pos));
        if (smoothSmp > 1.f) g *= clampf(edge / smoothSmp, 0.f, 1.f);
        if (crush > 0.001f) g = crushSample(g);
        return x + f * (g - x);
    }
private:
    void grab() {
        len = std::fmax(64.0, (double)ctx->stepSamples);
        start = (double)ctx->cap->pos() - len;
    }
    void newStep(bool first) {
        phase = 0.0; sliceRate = 1.f; reversed = false; tapeStopping = false;
        double stepLen = std::fmax(64.0, (double)ctx->stepSamples);
        len = stepLen;
        if (first) return;
        const float r = rng.uni();
        const float total = jumps + tapeStop + rev + 0.6f;   // + rolls and fresh grabs
        const float a = r * total;
        if (rng.uni() >= chaos) return;                      // plain repeat
        if (a < jumps) {
            static const float ratios[4] = {2.f, 0.5f, 1.5f, 0.75f};
            sliceRate = ratios[rng.pick(4)];
        } else if (a < jumps + tapeStop) {
            tapeStopping = true;
        } else if (a < jumps + tapeStop + rev) {
            reversed = true;
        } else if (a < jumps + tapeStop + rev + 0.4f) {
            len = stepLen / (rng.uni() < 0.5f ? 2.0 : 4.0);  // roll
        } else {
            start = (double)ctx->cap->pos() - stepLen;       // fresh audio
        }
    }
    float crushSample(float g) {
        const int every = 1 + (int)(crush * crush * 23.f);
        if (++holdCount >= every) { holdCount = 0; held = g; }
        const float levels = expMap(1.f - crush, 8.f, 32768.f);
        return std::round(held * levels) / levels;
    }
    const FxContext* ctx = nullptr;
    float sr = 48000.f, chaos = 0.4f, crush = 0.f, jumps = 0.4f, tapeStop = 0.3f, rev = 0.3f, smoothSmp = 48.f;
    Rng rng;
    Fade fade;
    bool active = false, reversed = false, tapeStopping = false;
    int autoLeft = 0, holdCount = 0;
    double start = 0.0, len = 6000.0, phase = 0.0;
    float sliceRate = 1.f, held = 0.f;
};

// ---------------------------------------------------------------------------
// LASER: every pick attack fires a "pew": an oscillator (sine .. square)
// falling Drop semitones over Sweep, as loud as you picked. Start pitch is
// fixed, or (Track) two octaves above the note you played.
// Trigger: machine gun, one shot per Rate step. Always = picks fire too;
// While Triggered = picks and the gun only while held.
// ---------------------------------------------------------------------------
class Laser {
public:
    void init(float sampleRate, const FxContext* c) { sr = sampleRate; ctx = c; level.init(0.5f, sr); }
    void configure(int) { reset(); }
    void setTargets(const Params& p) {
        f0 = expMap(clampf(p.lPitch, 0.f, 1.f), 200.f, 6000.f);
        track = p.lTrack;
        drop = lerp(12.f, 60.f, clampf(p.lDrop, 0.f, 1.f));
        sweepMs = expMap(clampf(p.lSweep, 0.f, 1.f), 20.f, 800.f);
        tone = clampf(p.lTone, 0.f, 1.f);
        level.target = p.lLevel;
    }
    void snap() { level.snap(); }
    void reset() { for (auto& v : voice) v = V{}; next = 0; }
    void control() { level.control(); }
    float process(float x, float& dry) {
        dry = x;
        const FxContext& c = *ctx;
        const bool armed = c.always || c.trig;
        if (armed && c.onset) fire(clampf(c.onsetLevel * 2.f, 0.05f, 1.f));
        if (c.trig && (c.tick || c.trigEdge)) fire(clampf(c.env * 2.f + 0.3f, 0.3f, 1.f));
        float y = 0.f;
        for (auto& v : voice) {
            if (!v.on) continue;
            const float dt = v.f / sr;
            float s = std::sin(2.f * kPi * v.ph);
            if (tone > 0.f) s += tone * (pulseWave(v.ph, dt, 0.5f) * 0.7f - s);
            y += s * v.amp;
            v.ph += dt; if (v.ph >= 1.f) v.ph -= 1.f;
            v.f *= v.k;
            v.amp *= v.decay;
            if (v.amp < 1e-4f) v.on = false;
        }
        const float g = level.next();
        return x + 2.f * g * g * y * 0.5f;
    }
private:
    struct V { bool on = false; float ph = 0.f, f = 1000.f, k = 1.f, amp = 0.f, decay = 0.999f; };
    void fire(float amp) {
        V& v = voice[next]; next = (next + 1) % kVoices;
        float start = f0;
        if (track && ctx->pitch > 0.f && ctx->pitchConf > 0.5f) start = clampf(ctx->pitch * 4.f, 200.f, 8000.f);
        const float n = sweepMs * 0.001f * sr;
        v.on = true; v.ph = 0.f; v.f = start; v.amp = amp;
        v.k = std::pow(2.f, -drop / 12.f / n);
        v.decay = std::pow(1e-3f, 1.f / (n * 1.3f));   // -60 dB a little after the sweep
    }
    static constexpr int kVoices = 6;
    const FxContext* ctx = nullptr;
    float sr = 48000.f, f0 = 1000.f, drop = 36.f, sweepMs = 150.f, tone = 0.3f;
    bool track = false;
    V voice[kVoices];
    int next = 0;
    Ramp level;
};

// ---------------------------------------------------------------------------
// DIVE BOMB: pitch drops Depth semitones over Time (beats), wobbles at the
// bottom while held, and climbs back over Return. A WSOLA shifter (RASP's,
// made to glide: downward ratios only, so the read heads never reverse).
// Trigger: Always = every pick dives and comes back by itself, Trigger holds a
// dive. While Triggered = dives while held.
// ---------------------------------------------------------------------------
class DiveShifter {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        grain = (int)(0.04f * sr); half = grain / 2;
        corr = (int)(0.006f * sr); search = (int)(0.012f * sr) & ~1;
        lead = (int)(0.0065f * sr);
        start = (double)(corr + lead + 2);
        reset();
    }
    void reset() { dl.reset(); h[0] = Head{start, 0}; h[1] = Head{start, half}; rho = 0.0; }
    void setRatio(float r) { rho = 1.0 - (double)clampf(r, 0.01f, 1.f); }   // 0 = no shift
    float process(float x) {
        dl.write(x);
        float y = 0.f;
        for (int k = 0; k < 2; k++) {
            Head& hd = h[k];
            const int remaining = grain - hd.age;
            if (remaining == lead) beginSearch(hd, h[1 - k]);
            if (hd.searching && remaining > 0) searchStep(hd, remaining);
            if (hd.age >= grain) { hd.d = start + hd.best; hd.age = 0; hd.searching = false; }
            const float sn = std::sin(kPi * (float)hd.age / (float)grain);
            y += sn * sn * dl.read((float)hd.d);
            hd.d += rho; hd.age++;
        }
        return y;
    }
private:
    struct Head { double d = 0.0; int age = 0; bool searching = false, fine = false; int oR = 0, c = 0, best = 0; double bestScore = 0.0; };
    void beginSearch(Head& hd, const Head& other) {
        hd.searching = true; hd.fine = false; hd.c = 0; hd.best = 0; hd.bestScore = -1e30;
        hd.oR = (int)(other.d + rho * lead);
    }
    void searchStep(Head& hd, int delta) {
        const int c = hd.c;
        if (!hd.fine) {
            score(hd, c, delta);
            hd.c += 2;
            if (hd.c > search) { hd.fine = true; hd.c = hd.best - 1; }
        } else {
            if (c >= 0 && c <= search && c != hd.best) score(hd, c, delta);
            if (c >= hd.best + 1) { hd.searching = false; return; }
            hd.c += (c + 1 == hd.best) ? 2 : 1;
        }
    }
    void score(Head& hd, int c, int delta) {
        const int a0 = hd.oR - delta, b0 = (int)start + c - delta;
        double xy = 0.0, bb = 1e-9;
        for (int m = 0; m < corr; m += 2) { double a = dl.tap(a0 - m), b = dl.tap(b0 - m); xy += a * b; bb += b * b; }
        const double sc = xy / std::sqrt(bb);
        if (sc > hd.bestScore) { hd.bestScore = sc; hd.best = c; }
    }
    float sr = 48000.f;
    double rho = 0.0, start = 600.0;
    int grain = 1920, half = 960, corr = 288, search = 576, lead = 312;
    Head h[2];
    DelayLine<8192 * AXE_SR_SCALE> dl;
};

class Dive {
public:
    void init(float sampleRate, const FxContext* c) { sr = sampleRate; ctx = c; shifter.init(sr); xf.set(6.f, sr); dry.init(0.f, sr); }
    void configure(int) { reset(); }
    void setTargets(const Params& p) {
        depth = clampf(p.dDepth, 1.f, 36.f);
        timeBeats = expMap(clampf(p.dTime, 0.f, 1.f), 0.0625f, 4.f);
        returnBeats = expMap(clampf(p.dReturn, 0.f, 1.f), 0.0625f, 4.f);
        curve = clampf(p.dCurve, 0.f, 1.f); wobble = clampf(p.dWobble, 0.f, 1.f);
        dry.target = p.dDry;
    }
    void snap() { dry.snap(); }
    void reset() { shifter.reset(); semis = 0.f; prog = 0.f; state = kIdle; autoHold = 0; xf.v = 0.f; wob = 0.f; }
    void control() { dry.control(); }
    float process(float x, float& dryOut) {
        dryOut = x;
        const FxContext& c = *ctx;
        if (c.always && c.onset && !c.trig) autoHold = (int)c.beatsToSamples(timeBeats) + (int)(0.05f * sr);
        if (autoHold > 0) autoHold--;
        const bool want = c.trig || (c.always && autoHold > 0);
        if (want && (state == kIdle || state == kReturn)) { state = kDown; prog = semis / -depth; }
        if (!want && (state == kDown || state == kHold)) { state = kReturn; retFrom = semis; prog = 0.f; }
        switch (state) {
            case kDown: {
                prog += 1.f / std::fmax(1.f, c.beatsToSamples(timeBeats));
                if (prog >= 1.f) { prog = 1.f; state = kHold; }
                const float e = expMap(curve, 0.35f, 3.f);   // shape exponent
                semis = -depth * std::pow(prog, e);
                break;
            }
            case kHold: {
                wob += 6.f / sr; if (wob >= 1.f) wob -= 1.f;
                semis = -depth + wobble * 1.5f * std::sin(2.f * kPi * wob);
                break;
            }
            case kReturn: {
                prog += 1.f / std::fmax(1.f, c.beatsToSamples(returnBeats));
                if (prog >= 1.f) { prog = 1.f; state = kIdle; }
                semis = retFrom * (1.f - prog);
                break;
            }
            default: semis = 0.f; break;
        }
        shifter.setRatio(std::pow(2.f, std::fmin(semis, 0.f) / 12.f));
        const float s = shifter.process(x);
        const float w = xf.next(state != kIdle);
        if (w <= 0.f) return x;
        const float d = dry.next();
        return x * (1.f - w) + w * (s + d * x) / (1.f + d * 0.5f);
    }
private:
    enum { kIdle, kDown, kHold, kReturn };
    const FxContext* ctx = nullptr;
    float sr = 48000.f, depth = 12.f, timeBeats = 0.5f, returnBeats = 0.25f, curve = 0.5f, wobble = 0.2f;
    float semis = 0.f, prog = 0.f, retFrom = 0.f, wob = 0.f;
    int state = kIdle, autoHold = 0;
    DiveShifter shifter;
    Fade xf;
    Ramp dry;
};

// ---------------------------------------------------------------------------
// CHIPTUNE: follows the note you play with a pulse-wave game voice (semitone
// quantised, glide, bit crush), at your playing's level. Mono: the strongest
// note of a chord wins. Trigger runs the arpeggiator (octave, power, major,
// minor, random) at the Rate. While Triggered: the chip voice only while held.
// ---------------------------------------------------------------------------
class Chiptune {
public:
    void init(float sampleRate, const FxContext* c) { sr = sampleRate; ctx = c; fade.set(5.f, sr); dry.init(0.2f, sr); envS.setCutoff(30.f, sr); }
    void configure(int) { reset(); }
    void setTargets(const Params& p) {
        duty = lerp(0.125f, 0.5f, clampf(p.cPulse, 0.f, 1.f));
        crush = clampf(p.cCrush, 0.f, 1.f);
        arp = p.cArp < 0 ? 0 : (p.cArp > 5 ? 5 : p.cArp);
        octave = (p.cOctave < 0 ? 0 : (p.cOctave > 2 ? 2 : p.cOctave)) - 1;
        glideC = p.cGlide <= 0.001f ? 0.f : coefMs(lerp(1.f, 300.f, p.cGlide), sr);
        dry.target = p.cDry;
    }
    void snap() { dry.snap(); }
    void reset() { rng.seed(0xC41Fu); note = -1.f; cur = 60.f; ph = 0.f; arpStep = -1; arpOff = 0.f; held = 0.f; holdCount = 0; envS.reset(); fade.v = 0.f; }
    void control() { dry.control(); }
    float process(float x, float& dryOut) {
        dryOut = x;
        const FxContext& c = *ctx;
        if (c.pitch > 0.f && c.pitchConf > 0.6f) note = 69.f + 12.f * std::log2(c.pitch / 440.f);
        const bool on = c.always || c.trig;
        // arpeggiator offset
        if (arp > 0 && c.trig) {
            if (c.tick || c.trigEdge) {
                static const int pat[5][4] = {{0, 12, 0, 12}, {0, 7, 12, 7}, {0, 4, 7, 12}, {0, 3, 7, 12}, {0, 12, 24, -12}};
                arpStep++;
                arpOff = (float)(arp == 5 ? pat[4][rng.pick(4)] : pat[arp - 1][arpStep % 4]);
            }
        } else {
            arpOff = 0.f; arpStep = -1;
        }
        const float off = arpOff;
        float y = 0.f;
        if (note > 0.f) {
            const float target = std::round(note) + 12.f * (float)octave + off;
            cur = glideC > 0.f ? target + glideC * (cur - target) : target;
            const float f = 440.f * std::pow(2.f, (cur - 69.f) / 12.f);
            const float dt = clampf(f / sr, 0.f, 0.45f);
            y = pulseWave(ph, dt, duty) * 0.5f;
            ph += dt; if (ph >= 1.f) ph -= 1.f;
        }
        y *= std::sqrt(envS.lp(c.env * c.env)) * 2.f;
        if (crush > 0.001f) {
            const int every = 1 + (int)(crush * 15.f);
            if (++holdCount >= every) { holdCount = 0; held = y; }
            const float levels = expMap(1.f - crush, 4.f, 256.f);
            y = std::round(held * levels) / levels;
        }
        const float f = fade.next(on);
        return lerp(x, dry.next() * x + y, f);
    }
private:
    const FxContext* ctx = nullptr;
    float sr = 48000.f, duty = 0.3f, crush = 0.4f, glideC = 0.f, note = -1.f, cur = 60.f, ph = 0.f, held = 0.f, arpOff = 0.f;
    int arp = 1, octave = 0, arpStep = -1, holdCount = 0;
    Rng rng;
    Fade fade;
    Ramp dry;
    OnePole envS;
};

// ---------------------------------------------------------------------------
// STUTTER GATE: rhythmic chopping on the Rate grid. Patterns over 8 steps:
// all, offbeat, trance, gallop, chug, random. Duty = how long each open step
// stays open, Depth = how far closed steps dip, Shape = edge time, Tone =
// closed steps go muffled instead of quiet. Trigger (Always): double-time roll.
// While Triggered: chops only while held.
// ---------------------------------------------------------------------------
class Stutter {
public:
    void init(float sampleRate, const FxContext* c) { sr = sampleRate; ctx = c; fade.set(5.f, sr); lp.setCutoff(500.f, sr); }
    void configure(int inst) { lp.setCutoff(inst == kBass ? 250.f : 500.f, sr); reset(); }
    void setTargets(const Params& p) {
        pattern = p.sPattern < 0 ? 0 : (p.sPattern > 5 ? 5 : p.sPattern);
        duty = lerp(0.1f, 1.f, clampf(p.sDuty, 0.f, 1.f));
        depth = clampf(p.sDepth, 0.f, 1.f);
        edge = coefMs(expMap(clampf(p.sShape, 0.f, 1.f), 0.5f, 20.f), sr);
        randomize = clampf(p.sRandom, 0.f, 1.f);
        tone = clampf(p.sTone, 0.f, 1.f);
    }
    void snap() {}
    void reset() { rng.seed(0x5707u); g = 1.f; lastStep = -1; open = true; fade.v = 0.f; lp.reset(); }
    void control() {}
    float process(float x, float& dryOut) {
        dryOut = x;
        const FxContext& c = *ctx;
        const bool on = c.always || c.trig;
        const double sb = c.stepBeats / ((c.always && c.trig) ? 2.0 : 1.0);
        const double pos = c.beats / sb;
        const long st = (long)std::floor(pos);
        const float frac = (float)(pos - (double)st);
        if (st != lastStep) {
            lastStep = st;
            static const uint8_t masks[5] = {0xFF, 0x55, 0xB6, 0xBB, 0xDA};   // step 0 = MSB
            bool bit = pattern == 5 ? (rng.uni() < 0.6f) : ((masks[pattern] >> (7 - (st & 7))) & 1) != 0;
            if (randomize > 0.f && rng.uni() < randomize * 0.5f) bit = !bit;
            open = bit;
        }
        const float target = (open && frac < duty) ? 1.f : 1.f - depth;
        g = target + edge * (g - target);
        const float l = lp.lp(x);
        const float y = g * x + (1.f - g) * tone * l;
        return lerp(x, y, fade.next(on));
    }
private:
    const FxContext* ctx = nullptr;
    float sr = 48000.f, duty = 0.5f, depth = 1.f, edge = 0.f, randomize = 0.f, tone = 0.f, g = 1.f;
    int pattern = 0;
    long lastStep = -1;
    bool open = true;
    Rng rng;
    Fade fade;
    OnePole lp;
};

// ---------------------------------------------------------------------------
// RING SHREDDER: ring modulator (x times a sine) or frequency shifter (every
// partial moved by the same Hz, so harmonics go inharmonic: metal). The
// carrier is fixed (Freq) or follows your note (Track, octaves -2..+2).
// Trigger: the carrier jumps (octaves, fifths, fourths) on Rate steps, with
// glide. While Triggered: only while held.
// ---------------------------------------------------------------------------
class Ring {
public:
    void init(float sampleRate, const FxContext* c) { sr = sampleRate; ctx = c; fade.set(5.f, sr); freq.init(0.4f, sr); drive.init(0.2f, sr); }
    void configure(int) { reset(); }
    void setTargets(const Params& p) {
        type = p.rType == 1 ? 1 : 0; track = p.rTrack;
        freq.target = p.rFreq; drive.target = p.rDrive;
        jump = clampf(p.rJump, 0.f, 1.f);
        glideC = coefMs(lerp(0.5f, 300.f, clampf(p.rGlide, 0.f, 1.f)), sr);
    }
    void snap() { freq.snap(); drive.snap(); curHz = baseHz(); }
    void reset() { rng.seed(0x51A6u); hil.reset(); ph = 0.f; mult = 1.f; fade.v = 0.f; curHz = 0.f; }
    void control() { freq.control(); drive.control(); }
    float process(float x, float& dryOut) {
        dryOut = x;
        const FxContext& c = *ctx;
        const bool on = c.always || c.trig;
        if (c.trig && (c.tick || c.trigEdge)) {
            static const float r[6] = {2.f, 0.5f, 1.5f, 2.f / 3.f, 4.f / 3.f, 0.75f};
            if (rng.uni() < jump) mult = clampf(mult * r[rng.pick(6)], 0.25f, 4.f);
            else if (rng.uni() < 0.3f) mult = 1.f;
        }
        if (!c.trig) mult = 1.f;
        const float target = baseHz() * mult;
        curHz = target + glideC * (curHz - target);
        const float dr = drive.next(), k = 1.f + 12.f * dr * dr;
        const float xin = k > 1.01f ? fastTanh(x * k) / std::sqrt(k) : x;
        float y;
        if (type == 0) {
            y = xin * std::sin(2.f * kPi * ph);
        } else {
            float re, im; hil.process(xin, re, im);
            y = re * std::cos(2.f * kPi * ph) + im * std::sin(2.f * kPi * ph);   // upward shift
        }
        ph += curHz / sr; if (ph >= 1.f) ph -= 1.f;
        return lerp(x, y, fade.next(on));
    }
private:
    float baseHz() const {
        const float knob = freq.cur;
        if (track && ctx->pitch > 0.f && ctx->pitchConf > 0.5f) return ctx->pitch * std::pow(2.f, std::round((knob - 0.5f) * 4.f));
        return expMap(clampf(knob, 0.f, 1.f), 20.f, 3000.f);
    }
    const FxContext* ctx = nullptr;
    float sr = 48000.f, jump = 0.4f, glideC = 0.f, ph = 0.f, mult = 1.f, curHz = 0.f;
    int type = 0;
    bool track = false;
    Rng rng;
    Hilbert hil;
    Fade fade;
    Ramp freq, drive;
};

// ---------------------------------------------------------------------------
// FREEZE: grabs the last Size of what you played and holds it as an endless
// pad: four overlapping grains from random places in the capture, slightly
// detuned (Drift), some an octave up (Shimmer). Your playing continues on top.
// Trigger: freeze while held (Attack in, Release out). Always: every pick
// re-freezes, so the last chord rings until the next one.
// ---------------------------------------------------------------------------
class Freeze {
public:
    static constexpr int kMax = 65536 * AXE_SR_SCALE;   // 1.36 s at 48 kHz
    void init(float sampleRate, const FxContext* c) { sr = sampleRate; ctx = c; level.init(0.7f, sr); }
    void configure(int) { reset(); }
    void setTargets(const Params& p) {
        sizeSmp = clampf(expMap(clampf(p.fzSize, 0.f, 1.f), 0.08f, 0.6f) * sr, 512.f, (float)kMax - 4.f);
        attC = 1.f / std::fmax(1.f, expMap(clampf(p.fzAttack, 0.f, 1.f), 5.f, 2000.f) * 0.001f * sr);
        relC = 1.f / std::fmax(1.f, expMap(clampf(p.fzRelease, 0.f, 1.f), 20.f, 4000.f) * 0.001f * sr);
        drift = clampf(p.fzDrift, 0.f, 1.f) * 25.f; shimmer = clampf(p.fzShimmer, 0.f, 1.f);
        level.target = p.fzLevel;
    }
    void snap() { level.snap(); }
    void reset() { rng.seed(0xF2EEu); have = false; gain = 0.f; pending = -1; dip = 0; for (auto& g : gr) g = G{}; len = 0; }
    void control() { level.control(); }
    float process(float x, float& dryOut) {
        dryOut = x;
        const FxContext& c = *ctx;
        const int dipLen = (int)(0.02f * sr);
        auto request = [&] { if (gain > 0.05f) dip = dipLen; else capture(); };
        if (c.trigEdge) request();
        if (c.always && !c.trig && c.onset) pending = (int)(0.09f * sr);   // grab just after the pick
        if (pending >= 0 && --pending < 0) request();
        const bool want = have && (c.trig || c.always);
        if (dip > 0) {   // a re-freeze while sounding: dip out, grab, come back
            gain = std::fmax(0.f, gain - 1.f / (float)dipLen);
            if (--dip == 0) capture();
        } else {
            gain = want ? std::fmin(1.f, gain + attC) : std::fmax(0.f, gain - relC);
        }
        float y = 0.f;
        if (have && gain > 0.f) {
            for (auto& g : gr) {
                if (g.age >= g.len) spawn(g);
                const float w = std::sin(kPi * (float)g.age / (float)g.len);
                double p = g.pos; while (p >= (double)len) p -= (double)len;
                const int i = (int)p; const float f = (float)(p - i);
                const float a = buf[i], b = buf[(i + 1) % len];
                y += w * w * (a + (b - a) * f);
                g.pos += g.rate; g.age++;
            }
            y *= 0.5f;
        }
        const float lv = level.next();
        return x + gain * lv * lv * 1.4f * y;
    }
private:
    struct G { double pos = 0.0; float rate = 1.f; int age = 1, len = 1; };
    void capture() {
        len = (int)sizeSmp;
        const double s = (double)ctx->cap->pos() - len;
        for (int i = 0; i < len; i++) buf[i] = ctx->cap->read(s + i);
        // fade the loop's ends so grain wraps don't click
        const int f = len / 16;
        for (int i = 0; i < f; i++) { const float w = (float)i / (float)f; buf[i] *= w; buf[len - 1 - i] *= w; }
        have = true;
        for (int k = 0; k < kGrains; k++) { spawn(gr[k]); gr[k].age = gr[k].len * k / kGrains; }
    }
    void spawn(G& g) {
        g.len = std::max(256, len / 2);
        g.pos = rng.uni() * (float)(len - 1);
        const float cents = rng.bi() * drift;
        g.rate = std::pow(2.f, cents / 1200.f) * (rng.uni() < shimmer * 0.5f ? 2.f : 1.f);
        g.age = 0;
    }
    static constexpr int kGrains = 4;
    const FxContext* ctx = nullptr;
    float sr = 48000.f, sizeSmp = 9600.f, attC = 0.001f, relC = 0.0005f, drift = 7.f, shimmer = 0.f, gain = 0.f;
    bool have = false;
    int len = 0, pending = -1, dip = 0;
    Rng rng;
    G gr[kGrains];
    Ramp level;
    float buf[(size_t)kMax] = {};
};

// ---------------------------------------------------------------------------
// FEEDBACK: the amp-feedback swell without the volume. A narrow resonator at
// a harmonic of the note you're holding is fed by your guitar and by itself;
// its loop gain rises past 1 over Swell, so it starts singing on its own
// (soft-limited), with a vibrato that blooms in. It keeps singing while held
// even as the string dies away, and fades over Release.
// Trigger: swell now. Always: swells by itself on notes you let ring.
// ---------------------------------------------------------------------------
class Feedback {
public:
    void init(float sampleRate, const FxContext* c) { sr = sampleRate; ctx = c; level.init(0.6f, sr); }
    void configure(int inst) { inst_ = inst; reset(); }
    void setTargets(const Params& p) {
        harmonic = p.fbHarmonic < 0 ? 0 : (p.fbHarmonic > 3 ? 3 : p.fbHarmonic);
        swellC = 1.f / std::fmax(1.f, expMap(clampf(p.fbSwell, 0.f, 1.f), 50.f, 3000.f) * 0.001f * sr);
        relC = 1.f / std::fmax(1.f, expMap(clampf(p.fbRelease, 0.f, 1.f), 20.f, 3000.f) * 0.001f * sr);
        bloom = clampf(p.fbBloom, 0.f, 1.f);
        toneHz = expMap(clampf(p.fbTone, 0.f, 1.f), 1000.f, 10000.f);
        level.target = p.fbLevel;
    }
    void snap() { level.snap(); }
    void reset() { res.reset(); fb = 0.f; out = 0.f; hz = 0.f; ringFor = 0; wob = 0.f; held = 0.f; lp.reset(); }
    void control() {
        level.control();
        lp.setCutoff(toneHz, sr);
        if (hz > 0.f) {
            const float cents = bloom * 30.f * held * std::sin(2.f * kPi * wob);
            res.set(hz * std::pow(2.f, cents / 1200.f), 40.f, sr);
        }
    }
    float process(float x, float& dryOut) {
        dryOut = x;
        const FxContext& c = *ctx;
        // Always: a note left ringing ~0.4 s starts it
        if (c.onset) ringFor = 0; else if (c.env > 0.003f) ringFor++;
        const bool autoOn = c.always && ringFor > (int)(0.4f * sr) && c.env > 0.003f;
        const bool on = c.trig || autoOn;
        if ((c.trigEdge || (autoOn && fb <= 0.f)) && c.pitch > 0.f && c.pitchConf > 0.5f) {
            static const float mult[4] = {1.f, 2.f, 3.f, 4.f};
            hz = clampf(c.pitch * mult[harmonic], 40.f, sr * 0.4f);
            res.reset(); out = 0.f;
            res.set(hz, 40.f, sr);
        }
        if (on && hz > 0.f) fb = std::fmin(1.08f, fb + swellC * 1.08f);
        else fb = std::fmax(0.f, fb - relC * 1.08f);
        held = on ? std::fmin(1.f, held + 1.f / sr) : std::fmax(0.f, held - 2.f / sr);
        wob += 5.5f / sr; if (wob >= 1.f) wob -= 1.f;
        if (fb <= 0.f || hz <= 0.f) { out = 0.f; return x; }
        res.process(x * 0.5f + out * fb);
        out = fastTanh(res.bp * res.k * 1.5f) * 0.6667f;
        const float lv = level.next();
        return x + lv * lv * 1.6f * lp.lp(out) * std::fmin(1.f, fb);
    }
private:
    const FxContext* ctx = nullptr;
    float sr = 48000.f, swellC = 0.001f, relC = 0.001f, bloom = 0.3f, toneHz = 5000.f;
    float fb = 0.f, out = 0.f, hz = 0.f, wob = 0.f, held = 0.f;
    int harmonic = 1, inst_ = kGuitar, ringFor = 0;
    SVF res;
    OnePole lp;
    Ramp level;
};

// ---------------------------------------------------------------------------
// SWARM: granular cloud. Grains (10..250 ms, Hann-windowed) are read from up
// to 1.5 s back in what you played, some shifted by octaves or fifths, some
// reversed, scattered at Density per second. Trigger (Always): a 4x burst.
// While Triggered: swarms while held.
// ---------------------------------------------------------------------------
class Swarm {
public:
    void init(float sampleRate, const FxContext* c) { sr = sampleRate; ctx = c; fade.set(20.f, sr); level.init(0.7f, sr); normC = 1.f - coefMs(60.f, sr); }
    void configure(int) { reset(); }
    void setTargets(const Params& p) {
        densityHz = expMap(clampf(p.swDensity, 0.f, 1.f), 2.f, 120.f);
        sizeSmp = expMap(clampf(p.swSize, 0.f, 1.f), 0.01f, 0.25f) * sr;
        spraySmp = clampf(p.swSpray, 0.f, 1.f) * 1.5f * sr;
        pitchShare = clampf(p.swPitch, 0.f, 1.f); revShare = clampf(p.swReverse, 0.f, 1.f);
        level.target = p.swLevel;
    }
    void snap() { level.snap(); }
    void reset() { rng.seed(0x5EA2u); for (auto& g : gr) g = G{}; fade.v = 0.f; norm = 1.f / std::sqrt(std::fmax(1.f, densityHz * sizeSmp / sr)); }
    void control() { level.control(); }
    float process(float x, float& dryOut) {
        dryOut = x;
        const FxContext& c = *ctx;
        const bool on = c.always || c.trig;
        const float dens = densityHz * ((c.always && c.trig) ? 4.f : 1.f);
        const float f = fade.next(on);
        if (f > 0.f && rng.uni() < dens / sr) spawn();
        float y = 0.f;
        for (auto& g : gr) {
            if (!g.on) continue;
            const float w = std::sin(kPi * (float)g.age / (float)g.len);
            y += w * w * c.cap->read(g.pos);
            g.pos += g.rate; g.age++;
            if (g.age >= g.len) g.on = false;
        }
        const float overlap = std::fmax(1.f, dens * sizeSmp / sr);
        norm += normC * (1.f / std::sqrt(overlap) - norm);    // glides: grains in flight must not jump
        const float lv = level.next();
        return x + f * lv * lv * 1.4f * y * norm;
    }
private:
    struct G { bool on = false; double pos = 0.0; float rate = 1.f; int age = 0, len = 1; };
    void spawn() {
        for (auto& g : gr) {
            if (g.on) continue;
            static const float shifts[4] = {2.f, 0.5f, 1.5f, 0.75f};
            g.rate = rng.uni() < pitchShare ? shifts[rng.pick(4)] : 1.f;
            if (rng.uni() < revShare) g.rate = -g.rate;
            g.len = std::max(64, (int)(sizeSmp * (0.7f + 0.6f * rng.uni())));
            // start far enough back that a forward grain never reads the future
            const double need = (double)g.len * std::fabs(g.rate) + 2.0;
            double back = need + (double)(rng.uni() * spraySmp);
            if (g.rate < 0.f) back -= (double)g.len * std::fabs(g.rate);
            g.pos = (double)ctx->cap->pos() - std::fmax(need, back);
            g.age = 0; g.on = true;
            return;
        }
    }
    static constexpr int kGrains = 24;
    const FxContext* ctx = nullptr;
    float sr = 48000.f, densityHz = 20.f, sizeSmp = 4800.f, spraySmp = 24000.f, pitchShare = 0.3f, revShare = 0.2f, norm = 1.f, normC = 0.001f;
    Rng rng;
    G gr[kGrains];
    Fade fade;
    Ramp level;
};

// ---------------------------------------------------------------------------
// RADIO: your guitar as a shortwave broadcast: a band-pass "station", slow
// fading, warble (wow and flutter on a short delay), static and crackle.
// Trigger spins the dial: it sweeps to a new station (with a heterodyne
// whistle and a burst of static) over Spin, and keeps re-tuning on Rate steps
// while held. While Triggered: on air only while held.
// ---------------------------------------------------------------------------
class Radio {
public:
    void init(float sampleRate, const FxContext* c) {
        sr = sampleRate; ctx = c; fade.set(10.f, sr);
        band.init(0.5f, sr); width.init(0.4f, sr); stat.init(0.3f, sr); warble.init(0.3f, sr); fadeAmt.init(0.3f, sr);
        noiseHp.setCutoff(900.f, sr); fadeLp.setCutoff(0.4f, sr); wowLp.setCutoff(3.f, sr);
        level.init(sr, 0.5f, 8.f);
    }
    void configure(int inst) { bass = inst == kBass; reset(); }
    void setTargets(const Params& p) {
        band.target = p.rdBand; width.target = p.rdWidth; stat.target = p.rdStatic; warble.target = p.rdWarble; fadeAmt.target = p.rdFade;
        spinSmp = expMap(clampf(p.rdSpin, 0.f, 1.f), 0.05f, 2.f) * sr;
    }
    void snap() { band.snap(); width.snap(); stat.snap(); warble.snap(); fadeAmt.snap(); fc = homeHz(); from = to = fc; kPrev = kNow = 1.f / lerp(6.f, 0.7f, width.cur); kStep = 0.f; }
    void reset() {
        rng.seed(0xAD10u); level.reset(); bp.reset(); dl.reset(); noiseHp.reset(); fadeLp.reset(); wowLp.reset();
        spinPos = 1.f; offset = 1.f; fade.v = 0.f; ph = 0.f; fl = 0.f; whistle = 0.f; crackle = 0.f; fc = from = to = homeHz();
    }
    void control() {
        band.control(); width.control(); stat.control(); warble.control(); fadeAmt.control();
        if (spinPos >= 1.f) { from = to = homeHz() * offset; fc = to; }
        bp.set(fc, lerp(6.f, 0.7f, width.cur), sr);
        kNow = bp.k; kStep = (1.f / lerp(6.f, 0.7f, width.cur) - kPrev) / (float)kCtl; kNow = kPrev;
    }
    float process(float x, float& dryOut) {
        dryOut = x;
        const FxContext& c = *ctx;
        const bool on = c.always || c.trig;
        if (c.trigEdge || (c.trig && c.tick)) {
            from = fc;
            offset = std::pow(2.f, rng.bi() * 1.2f);
            to = clampf(homeHz() * offset, 150.f, 6000.f);
            spinPos = 0.f;
        }
        if (!c.trig && spinPos >= 1.f) offset = 1.f;
        float spinning = 0.f;
        if (spinPos < 1.f) {
            spinPos = std::fmin(1.f, spinPos + 1.f / std::fmax(1.f, spinSmp));
            const float e = spinPos * spinPos * (3.f - 2.f * spinPos);
            fc = from * std::pow(to / from, e) * (1.f + 0.06f * std::sin(spinPos * 18.f));
            spinning = std::sin(kPi * spinPos);
        }
        // warble: wow (slow) + flutter, on a short delay
        dl.write(x);
        ph += (4.5f + 2.f * spinning) / sr; if (ph >= 1.f) ph -= 1.f;
        const float wow = wowLp.lp(rng.bi());
        const float wd = warble.next();
        const float d = (0.004f + wd * (0.0025f * std::sin(2.f * kPi * ph) + 0.03f * wow)) * sr;
        float y = dl.read(std::fmax(1.f, d));
        // the station (its bandwidth glides per sample: a Q step clicks)
        kNow += kStep; kPrev = kNow; bp.k = kNow;
        bp.process(std::tanh(y * 2.f) * 0.5f);
        y = level.process(x, bp.bp * bp.k);
        // fading in and out
        fl = fadeLp.lp(rng.bi());
        const float fa = fadeAmt.next();
        y *= 1.f - fa * clampf(0.5f + fl * 8.f, 0.f, 1.f) * 0.85f;
        // static, crackle, and the whistle while tuning
        const float sAmt = stat.next() + spinning;
        float n = noiseHp.hp(rng.bi()) * 0.04f * sAmt;
        if (rng.uni() < 0.0008f * sAmt) crackle = 0.3f * rng.bi();
        crackle *= 0.97f;
        n += crackle * sAmt;
        whistle += clampf(fc * 1.37f, 200.f, 9000.f) / sr; if (whistle >= 1.f) whistle -= 1.f;
        n += spinning * 0.05f * std::sin(2.f * kPi * whistle);
        const float env = clampf(c.env * 6.f, 0.f, 1.f);   // static rides on your playing (silence stays silent)
        y += n * env;
        return lerp(x, y, fade.next(on));
    }
private:
    float homeHz() const { return bass ? expMap(clampf(band.cur, 0.f, 1.f), 150.f, 1500.f) : expMap(clampf(band.cur, 0.f, 1.f), 300.f, 3000.f); }
    const FxContext* ctx = nullptr;
    float sr = 48000.f, spinSmp = 9600.f, spinPos = 1.f, fc = 1000.f, from = 1000.f, to = 1000.f, offset = 1.f;
    float ph = 0.f, fl = 0.f, whistle = 0.f, crackle = 0.f, kNow = 1.f, kPrev = 1.f, kStep = 0.f;
    bool bass = false;
    Rng rng;
    SVF bp;
    DelayLine<4096 * AXE_SR_SCALE> dl;
    OnePole noiseHp, fadeLp, wowLp;
    AutoLevel level;
    Fade fade;
    Ramp band, width, stat, warble, fadeAmt;
};

}  // namespace axe
