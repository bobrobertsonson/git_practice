// Stage fail-safes for the RASP pedal, kept free of libDaisy so they can be
// tested on a computer (host/failsafe_test.cpp).
//
//  1. Health check per audio block. A non-finite sample, or the output stuck
//     silent for a second while a voice is coming in (and no momentary effect
//     that is silent on purpose is held), counts as a fault. On a fault the
//     block is muted and healthy() turns false, which stops the relay
//     heartbeat: the hard-wired bypass puts the plain mic on the outputs.
//     The main loop then re-initializes the engine and calls recovered().
//  2. Load shedding. When the DSP load stays above 90% the heaviest optional
//     layers are switched off a step at a time (harmony pairs 3-4 and freeze,
//     then all harmony and double-track, then the thicken section) and come
//     back one step at a time once it has stayed below 65% for 5 s.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "../dsp/rasp.h"

namespace rasp_pedal {

class HealthCheck {
public:
    void init(float sampleRate, size_t blockSize) {
        blocksPerSec = (int)(sampleRate / (float)(blockSize ? blockSize : 1));
    }
    // in: the mono input; L/R: the processed output (muted here on a fault).
    void check(const float* in, float* L, float* R, size_t n, bool silenceIsExpected) {
        float ein = 0.f, eout = 0.f;
        bool bad = false;
        for (size_t i = 0; i < n; i++) {
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) bad = true;
            ein += in[i] * in[i];
            eout += L[i] * L[i] + R[i] * R[i];
        }
        if (!std::isfinite(eout)) bad = true;
        const float rmsIn = ein / (float)(n ? n : 1), rmsOut = eout / (float)(2 * (n ? n : 1));
        // A voice is coming in (> -40 dBFS RMS) and nothing comes out (< -80 dBFS).
        if (!silenceIsExpected && rmsIn > 1e-4f && rmsOut < 1e-8f) {
            if (++silentBlocks > blocksPerSec) bad = true;
        } else {
            silentBlocks = 0;
        }
        if (bad) faulted = true;
        if (faulted) for (size_t i = 0; i < n; i++) L[i] = R[i] = 0.f;
    }
    bool healthy() const { return !faulted; }
    bool needsReset() const { return faulted; }
    void recovered() { faulted = false; silentBlocks = 0; faults++; }
    int faultCount() const { return faults; }
private:
    volatile bool faulted = false;
    int silentBlocks = 0, blocksPerSec = 1000, faults = 0;
};

class LoadShedder {
public:
    // Call from the main loop with the smoothed DSP load (0..1) and the time.
    void update(float load, uint32_t nowMs) {
        if (load > 0.9f) {
            calmSince = nowMs;
            if (lvl < 3 && nowMs - hotSince > 500) { lvl++; hotSince = nowMs; }
        } else {
            hotSince = nowMs;
            if (load < 0.65f && lvl > 0 && nowMs - calmSince > 5000) { lvl--; calmSince = nowMs; }
            if (load >= 0.65f) calmSince = nowMs;
        }
    }
    int level() const { return lvl; }
    // The parameters actually sent to the engine.
    rasp::Params apply(rasp::Params p) const {
        if (lvl >= 1) { p.harm3Level = 0.f; p.harm4Level = 0.f; p.freeze = false; }
        if (lvl >= 2) { p.octDown = 0.f; p.octUp = 0.f; p.doubleTrack = false; }
        if (lvl >= 3) { p.thickenOn = false; }
        return p;
    }
private:
    int lvl = 0;
    uint32_t hotSince = 0, calmSince = 0;
};

} // namespace rasp_pedal
