// AxeElevator tone modes: Tight, Sub, Buzzsaw.
#pragma once
#include "axe_base.h"

namespace axe {

// ---------------------------------------------------------------------------
// TIGHT: 3-band compressor.
// Linkwitz-Riley 4th-order split (LP+HP of an LR4 pair sums to a 2nd-order
// allpass, so the low band also goes through the high crossover's allpass
// and the three bands sum flat). Each band has a feed-forward compressor
// whose threshold rides that band's own program level. The low band gets the
// fastest attack and the steepest ratio: a palm mute's lows bloom 10-40 ms
// after the pick, and clamping that bloom is what "tight" means; Punch
// lengthens the attack so the pick click itself still gets through.
// ---------------------------------------------------------------------------
class Tight {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        for (auto& p : prog) p.init(sr);
        playing.set(1.f, 300.f, sr);
        tight.init(0.5f, sr); punch.init(0.5f, sr); rel.init(0.4f, sr);
        for (auto& g : bandDb) g.init(0.f, sr);
    }
    void configure(int instrument) {
        inst = instrument;
        const bool bass = inst == kBass;
        float xl = bass ? 90.f : 150.f, xh = bass ? 800.f : 1800.f;
        hpIn[0].highpass(bass ? 28.f : 60.f, 0.7071f, sr); hpIn[1] = hpIn[0];
        for (int i = 0; i < 2; i++) {
            lpL[i].lowpass(xl, 0.7071f, sr); hpL[i].highpass(xl, 0.7071f, sr);
            lpH[i].lowpass(xh, 0.7071f, sr); hpH[i].highpass(xh, 0.7071f, sr);
        }
        apH.allpass(xh, 0.7071f, sr);
        reset();
    }
    void setTargets(const Params& p) {
        tight.target = p.tight; punch.target = p.punch; rel.target = p.release;
        bandDb[0].target = p.lowDb; bandDb[1].target = p.midDb; bandDb[2].target = p.highDb;
    }
    void snap() { tight.snap(); punch.snap(); rel.snap(); for (auto& g : bandDb) g.snap(); first = true; control(); }
    void reset() {
        for (auto* b : {&hpIn[0], &hpIn[1], &lpL[0], &lpL[1], &hpL[0], &hpL[1], &lpH[0], &lpH[1], &hpH[0], &hpH[1], &apH}) b->reset();
        for (auto& p : prog) p.reset();
        for (auto& g : gr) g = 0.f;
        playing.reset();
    }
    void control() {
        tight.control(); punch.control(); rel.control();
        for (auto& g : bandDb) g.control();
        for (int i = 0; i < 3; i++) { offPrev[i] = offsetDb[i]; mkPrev[i] = makeupDb[i]; ratPrev[i] = ratio[i]; }
        if (tight.moving() || punch.moving() || rel.moving() || first) updateCoefs();
        if (first) for (int i = 0; i < 3; i++) { offPrev[i] = offsetDb[i]; mkPrev[i] = makeupDb[i]; ratPrev[i] = ratio[i]; }
        first = false;
        ph = 0;
    }
    float process(float x, float& dry) {
        x = hpIn[1].process(hpIn[0].process(x));
        float lo = lpL[1].process(lpL[0].process(x));
        float rest = hpL[1].process(hpL[0].process(x));
        float mid = lpH[1].process(lpH[0].process(rest));
        float hi = hpH[1].process(hpH[0].process(rest));
        lo = apH.process(lo);
        dry = lo + mid + hi;
        bool on = playing.process(x) > -60.f;
        float b[3] = {lo, mid, hi}, y = 0.f;
        // the dynamics settings glide per sample across the block (a block
        // step in threshold, ratio or makeup would click on a fast attack)
        const float f = (float)(++ph) * (1.f / kCtl);
        for (int i = 0; i < 3; i++) {
            float lvl = prog[i].process(b[i], on);
            float thr = lvl + lerp(offPrev[i], offsetDb[i], f);
            float target = kneeCurve(fastLinToDb(std::fabs(b[i])) - thr, lerp(ratPrev[i], ratio[i], f), 6.f);
            gr[i] = target + (target < gr[i] ? attC[i] : relC[i]) * (gr[i] - target);
            y += b[i] * fastDbToLin(gr[i] + lerp(mkPrev[i], makeupDb[i], f) + bandDb[i].next());
        }
        return y;
    }
    float gainReductionDb(int band) const { return -gr[band]; }
private:
    void updateCoefs() {
        float t = tight.cur, pu = punch.cur;
        // Thresholds sit relative to each band's program level: at Tight 1
        // the low band compresses from 9 dB below its average, 8:1.
        static const float baseRatio[3] = {2.f, 1.5f, 1.3f}, addRatio[3] = {6.f, 2.5f, 1.2f};
        static const float depth[3] = {15.f, 10.f, 8.f};
        float attMs[3] = {expMap(pu, 0.3f, 18.f), expMap(pu, 1.f, 25.f), expMap(pu, 0.5f, 15.f)};
        float relMs = expMap(rel.cur, 30.f, 300.f);
        float relScale[3] = {0.7f, 1.f, 0.8f};
        for (int i = 0; i < 3; i++) {
            ratio[i] = 1.f + (baseRatio[i] - 1.f + addRatio[i]) * t;
            ratio[i] = ratio[i] < 1.01f ? 1.01f : ratio[i];
            offsetDb[i] = 6.f - depth[i] * t;
            attC[i] = coefMs(attMs[i], sr);
            relC[i] = coefMs(relMs * relScale[i], sr);
            // Auto makeup: give back what a band a little above its average level loses.
            makeupDb[i] = -1.0f * kneeCurve(-offsetDb[i] + 6.f, ratio[i], 6.f);
        }
    }
    float sr = 48000.f;
    int inst = kGuitar;
    bool first = true;
    Biquad hpIn[2], lpL[2], hpL[2], lpH[2], hpH[2], apH;
    ProgramLevel prog[3];
    Follower playing;
    Ramp tight, punch, rel, bandDb[3];
    float ratio[3] = {2, 1.5f, 1.3f}, offsetDb[3] = {0, 0, 0}, attC[3] = {}, relC[3] = {}, makeupDb[3] = {}, gr[3] = {};
    float ratPrev[3] = {2, 1.5f, 1.3f}, offPrev[3] = {}, mkPrev[3] = {};
    int ph = 0;
};

// ---------------------------------------------------------------------------
// SUB, poly: a filter-bank frequency divider.
//
// The input is split into narrow bands (1/12 octave apart). In each band a
// cascade of two state-variable filters gives a quadrature pair (bp, lp),
// i.e. the band as a rotating phasor z = A e^{i phi}. Halving the rotation,
// w = sqrt(z), with the sign chosen so w moves continuously, gives that
// band's content an octave down, at the band's own amplitude: out = A cos(phi/2).
// Each note's partials land in different bands, so a chord divides note by
// note instead of fighting over one period (what makes a single-voice
// shifter warble on chords). A square root has two signs, so a partial that
// straddles two bands could come out of them in opposite polarity and cancel;
// each band therefore also lines its polarity up with its lower neighbour
// whenever the two carry the same partial (high short-term coherence).
//
// There is no buffering: the dry DI is untouched and the plugin reports zero
// latency. What you trade instead is bloom time - a narrow band takes time
// to ring up (~2Q/omega): the sub of a low E reaches full level ~40-70 ms
// after the pick, higher notes faster (measured: low E ~45-75 ms, A3 ~35 ms).
// Q is fixed at 8.5: at Q 8 every note of E5, A5, open E, open G, drop-C,
// drop-A and a close low E-G#-B triad gets its own sub within ~2 dB; at Q 6
// the close triad loses a note, and lower Q barely blooms faster.
// ---------------------------------------------------------------------------
class SubPoly {
public:
    static constexpr int kMaxBands = 56;
    // The bank runs decimated to ~6 kHz (everything it touches is below
    // ~1.7 kHz): 1/8 the work at 48 kHz. 4th-order lowpasses at 1.6 kHz
    // before decimation and after the zero-stuffed interpolation.
    void init(float sampleRate) {
        sr = sampleRate;
        dec = (int)std::lround(sr / 6000.f); if (dec < 1) dec = 1;
        srD = sr / (float)dec;
        aa[0].lowpass(1600.f, 0.5412f, sr); aa[1].lowpass(1600.f, 1.3066f, sr);
        ai[0] = aa[0]; ai[1] = aa[1];
    }
    float process(float x) {
        float xa = aa[1].process(aa[0].process(x));
        float u = 0.f;
        if (++phase >= dec) { phase = 0; u = processBank(xa) * (float)dec; }
        return ai[1].process(ai[0].process(u));
    }
    void configure(int instrument) {
        const bool bass = instrument == kBass;
        f0 = bass ? 28.f : 50.f;
        nb = kMaxBands;   // 56 bands at 1/12 octave, 4.6 octaves: guitar 50 Hz..1.3 kHz (drop A), bass 28..720 Hz (low B)
        for (int k = 0; k < nb; k++) {
            fc[k] = f0 * std::pow(2.f, k / 12.f);
            post[k].set(fc[k] * 0.5f, 1.3f, srD);
        }
        curQ = -1.f;
        reset();
    }
    void reset() {
        for (auto* f : {&aa[0], &aa[1], &ai[0], &ai[1]}) f->reset();
        phase = 0;
        for (int k = 0; k < kMaxBands; k++) {
            a[k].reset(); b[k].reset(); post[k].reset();
            wr[k] = 1.f; wi[k] = 0.f; sig[k] = 1.f; sigT[k] = 1.f; cr[k] = ci[k] = cm[k] = 0.f; zre[k] = zim[k] = zm[k] = sm[k] = 0.f;
        }
    }
    // q: analysis sharpness; rangeHz: top of the divided range; weights fade over half an octave.
    void set(float q, float rangeHz) {
        if (std::fabs(q - curQ) > 1e-3f) {
            curQ = q;
            // narrower bands see less of each partial; measured: -4.3 dB per doubling of Q from 4
            cal = 0.6f * dbToLin(4.3f * std::log2(q / 4.f));
            for (int k = 0; k < nb; k++) { a[k].set(fc[k], q, srD); b[k].set(fc[k], q, srD); }
        }
        for (int k = 0; k < nb; k++) {
            float oct = std::log2(rangeHz / fc[k]);
            weight[k] = clampf(oct * 2.f + 1.f, 0.f, 1.f);
        }
        cohC = coefMs(40.f, srD);
        sigC = coefMs(4.f, srD);
        magC = coefMs(4.f, srD);
    }
private:
    float processBank(float x) {
        // pass 1: each band's phasor z, its continuous square root w, and a
        // smoothed magnitude for the dominance weights
        for (int k = 0; k < nb; k++) {
            if (weight[k] <= 0.f) { zm[k] = 0.f; sm[k] = 0.f; continue; }
            a[k].process(x);
            b[k].process(a[k].bp * a[k].k);
            float zr = b[k].bp * b[k].k, zi = b[k].lp * b[k].k;
            float mag = std::sqrt(zr * zr + zi * zi);
            float wr_ = std::sqrt(0.5f * (mag + zr)), wi_ = std::sqrt(clampf(0.5f * (mag - zr), 0.f, 1e30f));
            if (zi < 0.f) wi_ = -wi_;
            if (wr_ * wr[k] + wi_ * wi[k] < 0.f) { wr_ = -wr_; wi_ = -wi_; }   // stay on the continuous branch
            wr[k] = wr_; wi[k] = wi_;
            zre[k] = zr; zim[k] = zi; zm[k] = mag;
            sm[k] = mag + magC * (sm[k] - mag);
        }
        // pass 2: dominance weights (a band speaks for a partial only where
        // it is the strongest of its neighbours: bands between two chord
        // notes beat and would add noise), polarity alignment, divide
        float y = 0.f;
        float prevWr = 0.f, prevWi = 0.f, prevDom = 0.f;
        for (int k = 0; k < nb; k++) {
            if (weight[k] <= 0.f) { prevWr = prevWi = 0.f; prevDom = 0.f; continue; }
            float c = sm[k] * sm[k] * sm[k];
            float l = k > 0 ? sm[k - 1] : 0.f, r = k + 1 < nb ? sm[k + 1] : 0.f;
            float dom = c / (c + l * l * l + r * r * r + 1e-30f);
            // polarity alignment with the lower band when both hold the same partial
            if (k > 0 && zm[k - 1] > 0.f) {
                float pr = zre[k] * zre[k - 1] + zim[k] * zim[k - 1], pi = zim[k] * zre[k - 1] - zre[k] * zim[k - 1];
                cr[k] = pr + cohC * (cr[k] - pr); ci[k] = pi + cohC * (ci[k] - pi);
                float pm = zm[k] * zm[k - 1];
                cm[k] = pm + cohC * (cm[k] - pm);
                float coh = std::sqrt(cr[k] * cr[k] + ci[k] * ci[k]) / (cm[k] + 1e-20f);
                // only between two bands that both speak for one partial: a
                // band holding two chord notes beats, and its coherence with
                // a neighbour drops over the 40 ms window
                if (coh > 0.9f && dom > 0.2f && prevDom > 0.2f && cm[k] > 1e-10f) {
                    float m2 = std::sqrt(pr * pr + pi * pi);
                    float hr = std::sqrt(0.5f * (m2 + pr)), hi = std::sqrt(clampf(0.5f * (m2 - pr), 0.f, 1e30f));
                    if (pi < 0.f) hi = -hi;
                    float qr = wr[k] * prevWr + wi[k] * prevWi, qi = wi[k] * prevWr - wr[k] * prevWi;
                    sigT[k] = (qr * hr + qi * hi) >= 0.f ? 1.f : -1.f;
                }
            }
            sig[k] = sigT[k] + sigC * (sig[k] - sigT[k]);
            // the partial's amplitude from this band and its neighbours (a
            // partial between two bands is seen weaker by both)
            float amp = std::sqrt(sm[k] * sm[k] + l * l + r * r);
            float cosHalf = wr[k] / (std::sqrt(zm[k]) + 1e-15f);
            post[k].process(cosHalf * amp * sig[k] * dom);
            y += weight[k] * post[k].bp * post[k].k;
            float sgn = sig[k] >= 0.f ? 1.f : -1.f;
            prevWr = wr[k] * sgn; prevWi = wi[k] * sgn; prevDom = dom;
        }
        return y * cal;
    }
    float sr = 48000.f, srD = 6000.f, f0 = 50.f, curQ = -1.f, cal = 0.6f, cohC = 0.f, sigC = 0.f, magC = 0.f;
    int nb = kMaxBands, dec = 8, phase = 0;
    Biquad aa[2], ai[2];
    float fc[kMaxBands] = {}, weight[kMaxBands] = {};
    SVF a[kMaxBands], b[kMaxBands], post[kMaxBands];
    float wr[kMaxBands] = {}, wi[kMaxBands] = {}, sig[kMaxBands] = {}, sigT[kMaxBands] = {};
    float cr[kMaxBands] = {}, ci[kMaxBands] = {}, cm[kMaxBands] = {};
    float zre[kMaxBands] = {}, zim[kMaxBands] = {}, zm[kMaxBands] = {}, sm[kMaxBands] = {};
};

class Sub {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        poly.init(sr);
        mono.init(sr, 0.5f);
        sub.init(0.5f, sr); dry.init(1.f, sr); tone.init(0.5f, sr); range.init(0.5f, sr); grit.init(0.f, sr);
        rmsA.setCutoff(4.f, sr); rmsB.setCutoff(4.f, sr);
    }
    void configure(int instrument) {
        inst = instrument;
        poly.configure(inst);
        hpOut.highpass(inst == kBass ? 18.f : 28.f, 0.7071f, sr);
        reset();
    }
    void setTargets(const Params& p) {
        type = p.subType;
        sub.target = p.sub; dry.target = p.dry; tone.target = p.subTone; range.target = p.range; grit.target = p.grit;
    }
    void snap() { sub.snap(); dry.snap(); tone.snap(); range.snap(); grit.snap(); first = true; control(); }
    void reset() {
        poly.reset(); mono.reset();
        lp[0].reset(); lp[1].reset(); hpOut.reset(); monoIn[0].reset(); monoIn[1].reset(); rmsA.reset(); rmsB.reset(); gritG = 1.f;
        monoFade = type == 1 ? 1.f : 0.f;
    }
    void control() {
        sub.control(); dry.control(); tone.control(); range.control(); grit.control();
        if (first || tone.moving() || range.moving()) {
            const bool bass = inst == kBass;
            float rHz = bass ? expMap(range.cur, 80.f, 600.f) : expMap(range.cur, 150.f, 1200.f);
            poly.set(kQ, rHz);
            float tHz = expMap(tone.cur, 80.f, 1600.f);
            Biquad t; t.lowpass(tHz, 0.7071f, sr);
            lp[0].glideTo(t, first); lp[1].glideTo(t, first);
            t.lowpass(rHz, 0.7071f, sr);
            monoIn[0].glideTo(t, first); monoIn[1].glideTo(t, first);
        }
        first = false;
        monoStep = (type == 1 ? 1.f : -1.f) * (float)kCtl / (0.03f * sr);
    }
    float process(float x, float& dryOut) {
        dryOut = x;
        // Poly and mono crossfade (30 ms) when the type switches; only the
        // live one(s) run.
        monoFade = clampf(monoFade + monoStep / kCtl, 0.f, 1.f);
        float s = 0.f;
        if (monoFade < 1.f) s += (1.f - monoFade) * poly.process(x);
        if (monoFade > 0.f) s += monoFade * mono.process(monoIn[1].process(monoIn[0].process(x)));
        // grit: a soft clip, level-matched so it changes the tone, not the level
        float gr = grit.next();
        if (gr > 1e-4f) {
            float drive = 1.f + 60.f * gr * gr;
            float c = fastTanh(drive * 8.f * s) * 0.125f;
            float a = rmsA.lp(s * s), b = rmsB.lp(c * c);
            if (a > 1e-12f) gritG += 0.002f * (clampf(std::sqrt(a / (b + 1e-14f)), 0.1f, 50.f) - gritG);
            s = lerp(s, c * gritG, clampf(gr * 4.f, 0.f, 1.f));
        }
        s = hpOut.process(lp[1].process(lp[0].process(s)));
        float sg = sub.next(); sg = 2.f * sg * sg;      // 0..2 (+6 dB), squared taper
        return dry.next() * x + sg * s;
    }
private:
    static constexpr float kQ = 8.5f;   // poly band Q: see SubPoly (Q 6 loses notes of close chords)
    float sr = 48000.f, monoFade = 0.f, monoStep = 0.f, gritG = 1.f;
    int inst = kGuitar, type = 0;
    bool first = true;
    SubPoly poly;
    Shifter mono;
    Biquad lp[2], hpOut, monoIn[2];
    OnePole rmsA, rmsB;
    Ramp sub, dry, tone, range, grit;
};

// ---------------------------------------------------------------------------
// BUZZSAW: an HM-2-style heavy drive, our own circuit. What makes that sound
// is the EQ around two hard clipping stages: a lift into the clippers, then
// big resonant boosts near 100 Hz and 1 kHz with the 300-500 Hz region
// scooped between them - "the chainsaw". Bass voicing moves the lifts to
// ~70 Hz / 800 Hz and keeps a clean blend for the fundamental.
//   pre: high-pass, +6 dB bell at ~900 Hz (into the clippers)
//   clip 1: asymmetric soft clip  -> interstage lowpass
//   clip 2: harder symmetric clip (both 4x oversampled, ADAA)
//   post: lows bell, highs bell, scoop, 7 kHz lowpass; auto level, then Level
// ---------------------------------------------------------------------------
class Buzzsaw {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        gain.init(0.65f, sr); lows.init(0.7f, sr); highs.init(0.7f, sr); scoop.init(0.5f, sr); level.init(0.f, sr); blend.init(0.f, sr);
        rmsIn.setCutoff(10.f, sr); rmsOut.setCutoff(10.f, sr);
    }
    void configure(int instrument) {
        inst = instrument;
        const bool bass = inst == kBass;
        hpIn.highpass(bass ? 35.f : 70.f, 0.7071f, sr);
        pre.peaking(bass ? 700.f : 900.f, 0.8f, 6.f, sr);
        inter.setCutoff(bass ? 4000.f : 5000.f, sr * 4.f);
        dcBlock.setCutoff(15.f, sr * 4.f);
        lp[0].lowpass(bass ? 5000.f : 7000.f, 0.7071f, sr); lp[1] = lp[0];
        hpOut.highpass(bass ? 30.f : 45.f, 0.7071f, sr);
        reset();
    }
    void setTargets(const Params& p) {
        gain.target = p.gain; lows.target = p.lows; highs.target = p.highs; scoop.target = p.scoop; level.target = p.level; blend.target = p.blend;
    }
    void snap() { gain.snap(); lows.snap(); highs.snap(); scoop.snap(); level.snap(); blend.snap(); first = true; control(); }
    void reset() {
        up.init(); down.init(); downDry.init();
        hpIn.reset(); pre.reset(); inter.reset(); dcBlock.reset(); lp[0].reset(); lp[1].reset(); hpOut.reset();
        bLow.reset(); bHigh.reset(); bScoop.reset(); rmsIn.reset(); rmsOut.reset();
        p1 = kBias; p1F = logCosh(kBias); p2 = p2F = 0.f;
        autoG = dbToLin(-24.f - 6.f * lows.target - 7.f * highs.target);   // a first guess; it adapts while you play
    }
    void control() {
        gain.control(); lows.control(); highs.control(); scoop.control(); level.control(); blend.control();
        if (first || lows.moving() || highs.moving() || scoop.moving()) {
            const bool bass = inst == kBass;
            Biquad t;
            t.peaking(bass ? 70.f : 100.f, 0.9f, 15.f * lows.cur, sr); bLow.glideTo(t, first);
            t.peaking(bass ? 800.f : 1050.f, 1.1f, 15.f * highs.cur, sr); bHigh.glideTo(t, first);
            t.peaking(bass ? 300.f : 420.f, 0.7f, -12.f * scoop.cur, sr); bScoop.glideTo(t, first);
        }
        first = false;
    }
    float process(float x, float& dryOut) {
        float d = gain.next();
        float g = fastDbToLin(6.f + 42.f * d);
        float pre_ = pre.process(hpIn.process(x));
        float yc = 0.f, yd = 0.f;
        for (int k = 0; k < 4; k++) {
            float u = up.process(k == 0 ? pre_ * 4.f : 0.f);
            // clip 1: asymmetric (a bias, then the DC is removed)
            float c1 = clip(g * u + kBias, p1, p1F) - kBiasOut;
            c1 = dcBlock.hp(c1);
            c1 = inter.lp(c1);
            // clip 2: harder, symmetric
            float c2 = clip(4.f * c1, p2, p2F);
            yc = down.process(c2);
        }
        // the clean blend takes the same oversampling filters, so it can't cancel
        for (int k = 0; k < 4; k++) yd = downDry.process(k == 0 ? x * 4.f : 0.f);
        dryOut = yd;
        float y = bScoop.process(bHigh.process(bLow.process(yc)));
        y = hpOut.process(lp[1].process(lp[0].process(y)));
        // auto level: the drive comes out about as loud as the DI went in
        // (it only adapts while you play: in the gaps it would creep up and
        // blast the next note)
        float ein = rmsIn.lp(x * x), eout = rmsOut.lp(y * y);
        if (ein > 1e-5f) {
            float want = clampf(std::sqrt(ein / (eout + 1e-12f)), 0.01f, 2.f);
            autoG += (want < 0.5f * autoG ? 0.01f : 0.0005f) * (want - autoG);   // too loud: catch up fast
        }
        y *= autoG * fastDbToLin(level.next());
        float bl = blend.next();
        return safetyClip(y * (1.f - 0.5f * bl) + bl * yd);
    }
private:
    static constexpr float kBias = 0.15f, kBiasOut = 0.14888503f;   // tanh(0.15)
    static float logCosh(float x) {
        float a = std::fabs(x);
        if (a > 5.f) return a - 0.69314718f;
        return a + std::log1p(std::exp(-2.f * a)) - 0.69314718f;
    }
    // tanh with first-order antiderivative anti-aliasing (as in RASP's Drive)
    static float clip(float c, float& prevC, float& prevF) {
        float dx = c - prevC, F = logCosh(c), y;
        if (std::fabs(dx) > 1e-4f) y = (F - prevF) / dx;
        else y = std::tanh(0.5f * (c + prevC));
        prevC = c; prevF = F;
        return y;
    }
    float sr = 48000.f, autoG = 1.f;
    int inst = kGuitar;
    bool first = true;
    float p1 = 0, p1F = 0, p2 = 0, p2F = 0;
    OSFilter up, down, downDry;
    Biquad hpIn, pre, lp[2], hpOut, bLow, bHigh, bScoop;
    OnePole inter, dcBlock, rmsIn, rmsOut;
    Ramp gain, lows, highs, scoop, level, blend;
};

}  // namespace axe
