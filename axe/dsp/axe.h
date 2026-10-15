// AxeElevator - a pre-amp sound mangler for guitar and bass. DSP core.
//
// Goes before an amp sim. One mode ("floor") runs at a time:
//
//   in ─► input trim ─► TONE MATCH ─┬─► capture buffer, envelope, pick onsets, pitch ──► (read by the fx modes)
//                                  └─► [ MODE ] ─► mix (phase-aligned dry) ─► gate ─► output ─► bypass xfade ─► out
//
// LEARN: Learn Playing measures your pick attack, picking speed, noise floor
// and note range (for the pick detector, the gate and the pitch tracker) and
// your guitar's tone. Tone Match then EQs that tone toward the Target: one
// you captured with Learn Target, or a preset (axe_targets.h).
//
//   tone   1 TIGHT          3-band compressor: palm mutes stay tight, the lows don't flub
//          2 SUB            octave down: Poly (filter-bank divider, holds chords) or Mono (WSOLA)
//          3 BUZZSAW        HM-2-style drive: big lows and 1 kHz, scooped middle
//   fx     4 GLITCH         beat repeat, rolls, pitch jumps, tape stops, reverse, crush
//          5 LASER          every pick fires a falling "pew"; Trigger = machine gun
//          6 DIVE BOMB      pitch dive and return, timed in beats
//          7 CHIPTUNE       pulse-wave game voice that follows your note; Trigger = arpeggiator
//          8 STUTTER GATE   rhythmic chopping patterns on the Rate grid
//          9 RING SHREDDER  ring mod / frequency shifter; Trigger = carrier jumps
//         10 FREEZE         holds what you played as an endless pad
//         11 FEEDBACK       amp-style feedback swell on a harmonic of your note
//         12 SWARM          granular cloud of what you just played
//         13 RADIO          shortwave broadcast; Trigger = spin the dial
//
// Global: Trigger (footswitch), Engage (Always / While Triggered), Sync (host
// tempo or the Tempo knob), Tempo, Rate (1/1 .. 1/32, triplets, dotted), Swing.
// Every mode has guitar and bass voicings (crossovers, filter corners, pitch ranges).
//
// See axe_base.h for the rules these files keep (no heap, no lookahead,
// zero latency, smoothed parameters, deterministic after reset).
#pragma once
#include "axe_base.h"
#include "axe_tone.h"
#include "axe_fx.h"
#include "axe_targets.h"

namespace axe {

// The modes that handle Trigger / Engage themselves (with their own tails);
// the tone modes are switched by the Voice when Engage is While Triggered.
inline bool selfGated(int mode) { return mode >= kGlitch; }

// Modes that need the pitch tracker (it only runs for these).
inline bool needsPitch(const Params& p) {
    return (p.mode == kLaser && p.lTrack) || p.mode == kChip || (p.mode == kRing && p.rTrack) || p.mode == kFeedback;
}

enum LearnWhat : int { kLearnNone = 0, kLearnTarget, kLearnPlaying };

// ---------------------------------------------------------------------------
// Voice: one complete mode chain at one instrument setting. The Processor
// keeps two and crossfades when the mode or instrument changes.
// To add a mode: write its class (init/configure/setTargets/snap/reset/
// control/process(x, dryAligned)), add it to Mode and the switch below, add
// its fields to Params and its page to the plugin's parameter table.
// ---------------------------------------------------------------------------
class Voice {
public:
    void init(float sr, const FxContext* ctx) {
        tight.init(sr); sub.init(sr); buzz.init(sr);
        glitch.init(sr, ctx); laser.init(sr, ctx); dive.init(sr, ctx); chip.init(sr, ctx); stutter.init(sr, ctx);
        ring.init(sr, ctx); freeze.init(sr, ctx); feedback.init(sr, ctx); swarm.init(sr, ctx); radio.init(sr, ctx);
        mix.init(1.f, sr);
        gateFade.set(8.f, sr);
    }
    template <class F> void each(F f) {
        switch (mode) {
            case kTight: f(tight); break;
            case kSub: f(sub); break;
            case kBuzzsaw: f(buzz); break;
            case kGlitch: f(glitch); break;
            case kLaser: f(laser); break;
            case kDive: f(dive); break;
            case kChip: f(chip); break;
            case kStutter: f(stutter); break;
            case kRing: f(ring); break;
            case kFreeze: f(freeze); break;
            case kFeedback: f(feedback); break;
            case kSwarm: f(swarm); break;
            case kRadio: f(radio); break;
            default: break;
        }
    }
    void configure(int m, int instrument, const Params& p) {
        mode = m; inst = instrument;
        each([&](auto& md) { md.setTargets(p); md.configure(inst); md.setTargets(p); md.snap(); });
        mix.snap(p.mix);
        engage = p.engage;
    }
    void setTargets(const Params& p) { each([&](auto& md) { md.setTargets(p); }); mix.target = p.mix; engage = p.engage; }
    void snap() { each([](auto& md) { md.snap(); }); mix.snap(); }
    void reset() { each([](auto& md) { md.reset(); }); gateFade.v = (selfGated(mode) || engage == kAlways) ? 1.f : 0.f; }
    void control() { each([](auto& md) { md.control(); }); mix.control(); }
    float process(float x, bool trig) {
        float dry = x, wet = x;
        each([&](auto& md) { wet = md.process(x, dry); });
        const float m = mix.next();
        float y = dry + m * (wet - dry);
        if (!selfGated(mode)) {   // tone modes: While Triggered switches them in and out
            const float g = gateFade.next(engage == kAlways || trig);
            y = x + g * (y - x);
        }
        return y;
    }
    int mode = -1, inst = -1, engage = kAlways;
    Tight tight;
    Sub sub;
    Buzzsaw buzz;
    Glitch glitch;
    Laser laser;
    Dive dive;
    Chiptune chip;
    Stutter stutter;
    Ring ring;
    Freeze freeze;
    Feedback feedback;
    Swarm swarm;
    Radio radio;
    Ramp mix;
    Fade gateFade;
};

// ---------------------------------------------------------------------------
// Processor: the whole plugin, mono in / mono out.
// ---------------------------------------------------------------------------
class Processor {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        ctx = FxContext{};
        ctx.sr = sr;
        ctx.cap = &cap;
        for (auto& v : voices) v.init(sr, &ctx);
        inG.init(1.f, sr); outG.init(1.f, sr);
        pitch.init(sr);
        pitchInst = -1;   // init() set the guitar range: re-apply the instrument
        matchEq.init(sr);
        matchAmt.init(1.f, sr);
        for (auto& g : matchG) g.init(0.f, sr);
        toneLearner.init(sr);
        playLearner.init(sr);
        onsets.init(sr);
        envF.set(2.f, 80.f, sr);
        gateKeyC = coefMs(5.f, sr); gateAtt = coefMs(1.f, sr); gateRel = coefMs(120.f, sr);
        holdLen = (int)(0.05f * sr);
        fadeLen = (int)(0.04f * sr);        // mode / instrument crossfade, 40 ms
        bypLen = (int)(0.02f * sr);         // bypass crossfade, 20 ms
        params = Params{};
        haveParams = false;
        reset();
    }
    void setParams(const Params& p) {
        params = p;
        params.mode = p.mode < 0 ? 0 : (p.mode >= kNumModes ? kNumModes - 1 : p.mode);
        params.instrument = p.instrument == kBass ? kBass : kGuitar;
        params.rate = p.rate < 0 ? 0 : (p.rate >= kNumRates ? kNumRates - 1 : p.rate);
        params.tempo = clampf(p.tempo, 40.f, 300.f);
        inG.target = fastDbToLin(clampf(p.inDb, -24.f, 24.f));
        outG.target = fastDbToLin(clampf(p.outDb, -60.f, 24.f));
        if (p.autoGate) { gateOn = true; gateThr = clampf(p.noiseFloor, -100.f, -30.f) + 8.f + 24.f * clampf(p.gate, 0.f, 1.f); }
        else { gateOn = p.gate > 1e-3f; gateThr = -75.f + 45.f * clampf(p.gate, 0.f, 1.f); }
        onsets.configure(p.pickSense, p.pickHold, std::fmax(-55.f, p.noiseFloor + 12.f));
        voices[active].setTargets(params);
        if (fade > 0) voices[1 - active].setTargets(params);
        matchAmt.target = clampf(p.matchAmount, 0.f, 1.f);
        const int tgt = p.target < 0 ? 0 : (p.target > kNumTargetPresets ? kNumTargetPresets : p.target);
        if (tgt != targetSel) { targetSel = tgt; if (haveParams) rematch(); }   // (the first params set it quietly)
        for (int b = 0; b < kMatchBands; b++) matchG[b].target = clampf(p.match[b], -12.f, 12.f);
        if (pitchInst != params.instrument || std::fabs(pitchLo - p.lowNote) > 0.f || std::fabs(pitchHi - p.highNote) > 0.f) {
            pitch.setInstrument(params.instrument, p.lowNote, p.highNote);
            pitchInst = params.instrument; pitchLo = p.lowNote; pitchHi = p.highNote;
        }
        if (!haveParams) { haveParams = true; applyNow(); }
    }
    // Host transport, once per block: tempo (<= 0: none), and the beat
    // position at the block's first sample when the host is playing.
    void setHostTransport(float bpm, double ppq, bool playing) {
        hostBpm = bpm; hostPpq = ppq; hostPlaying = playing && bpm > 0.f; havePpq = true;
    }
    // kx:Reset / transport reset: clear all state and jump every smoothed
    // value to its target (no glides, no fades).
    void reset() {
        applyNow();
        inG.snap(); outG.snap();
        bypassMix = params.bypass ? 0.f : 1.f;
        gateMs = 0.f; gateGr = 0.f; hold = 0;
        ctl = 0;
        cap.reset();
        pitch.reset(); onsets.reset(); envF.reset();
        ctx.beats = 0.0; ctx.step = -1; ctx.tick = false; ctx.env = 0.f; ctx.pitch = 0.f; ctx.pitchConf = 0.f;
        ctx.onset = false; ctx.onsetLevel = 0.f;
        matchEq.reset();
        matchAmt.snap(clampf(params.matchAmount, 0.f, 1.f));
        for (int b = 0; b < kMatchBands; b++) matchG[b].snap(clampf(params.match[b], -12.f, 12.f));
        { float g[kMatchBands]; for (int b = 0; b < kMatchBands; b++) g[b] = matchG[b].cur; matchEq.set(g, matchAmt.cur, true); }
        lastTrig = params.trigger; ctx.trig = params.trigger; ctx.trigEdge = ctx.trigFall = false;
    }
    void process(const float* in, float* out, int n) {
        // host position: align the grid at the block start
        if (havePpq && hostPlaying && params.sync == 1) ctx.beats = hostPpq;
        havePpq = false;
        for (int i = 0; i < n; i++) {
            if (ctl == 0) controlTick();
            ctl = (ctl + 1) % kCtl;
            const float x = in[i];
            float xi = x * inG.next();
            if (learnWhat != kLearnNone) learnFeed(xi);   // learning hears the guitar before Tone Match
            xi = matchEq.process(xi);
            analyse(xi);
            float y;
            if (fade > 0) {
                const float t = (float)fade / (float)fadeLen;      // 1 -> 0: old voice fading out
                y = t * voices[1 - active].process(xi, ctx.trig) + (1.f - t) * voices[active].process(xi, ctx.trig);
                if (--fade == 0) voices[1 - active].mode = -1;
            } else {
                y = voices[active].process(xi, ctx.trig);
            }
            y *= gate(xi) * outG.next();
            const float bt = params.bypass ? 0.f : 1.f;
            if (std::fabs(bypassMix - bt) > 0.f) bypassMix = clampf(bypassMix + (bt > bypassMix ? 1.f : -1.f) / (float)bypLen, 0.f, 1.f);
            out[i] = x + bypassMix * (safetyClip(y) - x);
        }
        // Never pass NaN/inf on (a broken host buffer, an unstable filter): recover silently.
        for (int i = 0; i < n; i++) if (!std::isfinite(out[i])) { reset(); for (int j = 0; j < n; j++) out[j] = 0.f; break; }
    }

    float gainReductionDb(int band) const { return voices[active].mode == kTight ? voices[active].tight.gainReductionDb(band) : 0.f; }
    float gateReductionDb() const { return -gateGr; }
    float bpm() const { return ctx.bpm; }
    int step() const { return ctx.step; }
    bool triggered() const { return ctx.trig; }
    float detectedPitch() const { return ctx.pitchConf > 0.5f ? ctx.pitch : 0.f; }
    int latencySamples() const { return 0; }

    // --- Learn ---------------------------------------------------------------
    void learnTarget() { learnWhat = kLearnTarget; toneLearner.start(); }
    void learnPlaying() { learnWhat = kLearnPlaying; playLearner.start(); toneLearner.start(); }   // also hears your tone
    void cancelLearn() { learnWhat = kLearnNone; toneLearner.cancel(); playLearner.cancel(); }
    void setLearnSeconds(float tone, float playing) { toneLearner.setSeconds(tone); playLearner.setSeconds(playing); }
    int learning() const { return learnWhat; }
    float learnProgress() const { return learnWhat == kLearnPlaying ? playLearner.progress() : toneLearner.progress(); }
    bool hasTarget() const { return haveTarget; }
    const float* targetProfile() const { return target; }
    // match = false when restoring a saved session: keep the bands it saved
    void setTargetProfile(const float* t, bool match = true) { for (int i = 0; i < kMatchBands; i++) target[i] = t[i]; haveTarget = true; if (match && targetSel == 0) rematch(); }
    void syncTarget(int t) { targetSel = t < 0 ? 0 : (t > kNumTargetPresets ? kNumTargetPresets : t); }   // select without re-matching
    // Your guitar's tone, from Learn Playing (saved with the session)
    bool hasMyTone() const { return haveMine; }
    const float* myTone() const { return mine; }
    void setMyTone(const float* t, bool match = true) { for (int i = 0; i < kMatchBands; i++) mine[i] = t[i]; haveMine = true; if (match) rematch(); }
    // Each returns true once, when its learning finished.
    bool learnedMatch(float* gains) {
        if (!matchReady) return false;
        matchReady = false;
        for (int i = 0; i < kMatchBands; i++) gains[i] = matchOut[i];
        return true;
    }
    bool learnedPlaying(PlayingProfile& out) {
        if (!playingReady) return false;
        playingReady = false; out = playingOut;
        return true;
    }

private:
    void learnFeed(float xi) {
        float prof[kMatchBands];
        if (learnWhat == kLearnPlaying) {
            toneLearner.feed(xi, prof);
            if (playLearner.feed(xi, pitch.pitchHz(), pitch.confidence())) {
                playLearner.result(playingOut); playingReady = true; learnWhat = kLearnNone;
                toneLearner.result(prof); toneLearner.cancel();   // whatever it heard while you played
                if (toneLearner.heardSeconds() > 1.f) setMyTone(prof);
            }
            return;
        }
        if (toneLearner.feed(xi, prof)) { learnWhat = kLearnNone; setTargetProfile(prof); }
    }
    // Tone Match bands = Target minus your tone, whenever either changes.
    void rematch() {
        const float* t = targetSel > 0 ? kTargetPresets[targetSel - 1] : (haveTarget ? target : nullptr);
        if (!t || !haveMine) return;
        matchGains(t, mine, matchOut);
        matchReady = true;
    }
    void applyNow() {
        voices[active].configure(params.mode, params.instrument, params);
        voices[active].reset();
        voices[active].snap();
        voices[1 - active].mode = -1;
        fade = 0;
    }
    void controlTick() {
        inG.control(); outG.control();
        ctx.always = params.engage == kAlways;
        ctx.instrument = params.instrument;
        ctx.bpm = (params.sync == 1 && hostBpm > 0.f) ? clampf(hostBpm, 20.f, 400.f) : params.tempo;
        ctx.stepBeats = kRateBeats[params.rate];
        ctx.stepSamples = ctx.beatsToSamples(ctx.stepBeats);
        runPitch = needsPitch(params) || learnWhat == kLearnPlaying;
        // Tone Match: amount and bands glide (~25 ms), the filters follow per block
        matchAmt.control();
        { float g[kMatchBands]; for (int b = 0; b < kMatchBands; b++) { matchG[b].control(); g[b] = matchG[b].cur; } matchEq.set(g, matchAmt.cur, false); }
        Voice& v = voices[active];
        if (fade == 0 && (v.mode != params.mode || v.inst != params.instrument)) {
            active = 1 - active;
            voices[active].configure(params.mode, params.instrument, params);
            voices[active].reset();
            fade = fadeLen;
        }
        voices[active].control();
        if (fade > 0) voices[1 - active].control();
    }
    // Everything the fx modes read, once per sample.
    void analyse(float xi) {
        cap.write(xi);
        // trigger edges; with no playing host, a new trigger restarts the grid
        // so the first step lands on the stomp
        const bool t = params.trigger;
        ctx.trigEdge = t && !lastTrig;
        ctx.trigFall = !t && lastTrig;
        ctx.trig = t;
        lastTrig = t;
        if (ctx.trigEdge && !(hostPlaying && params.sync == 1)) { ctx.beats = 0.0; ctx.step = -1; }
        // clock (swing delays every second step)
        const double sb = ctx.stepBeats, pair = 2.0 * sb;
        const double pairIdx = std::floor(ctx.beats / pair);
        const double inPair = ctx.beats - pairIdx * pair;
        const int st = (int)pairIdx * 2 + (inPair >= sb * (1.0 + 0.5 * clampf(params.swing, 0.f, 1.f)) ? 1 : 0);
        ctx.tick = st != ctx.step;
        ctx.step = st;
        ctx.beats += (double)ctx.bpm / 60.0 / (double)sr;
        // input analysis
        float lvl = 0.f;
        ctx.onset = onsets.process(xi, lvl);
        if (ctx.onset) ctx.onsetLevel = lvl;
        ctx.env = fastDbToLin(envF.process(xi));
        if (runPitch) {
            pitch.process(xi);
            ctx.pitch = pitch.pitchHz();
            ctx.pitchConf = pitch.confidence();
        }
    }
    float gate(float key) {
        if (!gateOn) { gateGr = 0.f; return 1.f; }
        gateMs = key * key + gateKeyC * (gateMs - key * key);
        const float lvl = fastPowDb10(gateMs);
        float want = 0.f;
        if (lvl >= gateThr) hold = holdLen;
        else if (hold > 0) hold--;
        else want = clampf((lvl - gateThr) * 3.f, -60.f, 0.f);    // 1:4 below threshold
        gateGr = want + (want > gateGr ? gateAtt : gateRel) * (gateGr - want);
        return fastDbToLin(gateGr);
    }

    float sr = 48000.f;
    Params params;
    FxContext ctx;
    CaptureBuffer cap;
    Voice voices[2];
    int active = 0, fade = 0, fadeLen = 1920, bypLen = 960, ctl = 0;
    bool haveParams = false;
    Ramp inG, outG;
    float bypassMix = 1.f;
    // analysis
    PitchTracker pitch;
    OnsetDetector onsets;
    Follower envF;
    bool runPitch = false, lastTrig = false;
    int pitchInst = -1;
    float pitchLo = -1.f, pitchHi = -1.f;
    // tone match and learning
    MatchEQ matchEq;
    Ramp matchAmt, matchG[kMatchBands];
    Learner toneLearner;
    PlayingLearner playLearner;
    int learnWhat = kLearnNone;
    float target[kMatchBands] = {}, mine[kMatchBands] = {}, matchOut[kMatchBands] = {};
    bool haveTarget = false, haveMine = false, matchReady = false, playingReady = false;
    int targetSel = 0;
    PlayingProfile playingOut;
    // host transport
    float hostBpm = 0.f;
    double hostPpq = 0.0;
    bool hostPlaying = false, havePpq = false;
    // gate
    bool gateOn = false;
    float gateThr = -60.f, gateMs = 0.f, gateGr = 0.f, gateKeyC = 0.f, gateAtt = 0.f, gateRel = 0.f;
    int hold = 0, holdLen = 2400;
};

}  // namespace axe
