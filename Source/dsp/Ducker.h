#pragma once

#include "Filters.h"

#include <algorithm>
#include <cmath>

namespace subforge
{

struct DuckSettings
{
    double depthDb = 8.0;         // max reduction when the key signal hits
    double thresholdDb = -30.0;   // key level where ducking starts; full depth 12 dB above it
    double releaseMs = 120.0;

    // Roll Hold: when key hits come faster than rollWindowMs (double kick, blasts),
    // stop pumping on every hit and hold a steady, lighter reduction instead.
    // The first hit of a run still gets the full duck.
    bool rollHold = false;
    double rollWindowMs = 150.0;
    double rollDepthDb = 2.0;
};

// Turns a key signal (a kick, or SubForge itself) into a gain reduction in dB.
// It listens only below 150 Hz, so the key's click, cymbals and guitars don't
// trigger it. Shared by SubForge (keyed by the kick) and SubForge Duck (keyed
// by SubForge, ducking the bass).
class Ducker
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        keyLowPass.setLowPass (sampleRate, 150.0, 0.7071);
        setSettings (settings);
        reset();
    }

    void reset()
    {
        keyLowPass.reset();
        env = onsetFast = onsetSlow = smoothedDb = 0.0;
        samplesSinceOnset = neverHit;
        inRoll = false;
    }

    void setSettings (const DuckSettings& s)
    {
        settings = s;
        const auto coef = [this] (double seconds) { return std::exp (-1.0 / std::max (1.0, seconds * sampleRate)); };
        releaseCoef = coef (settings.releaseMs * 0.001);
        attackCoef = coef (0.001);
        onsetFastCoef = coef (0.002);
        onsetSlowCoef = coef (0.030);
        smoothFastCoef = coef (0.001);
        smoothSlowCoef = coef (0.008);
        rollWindowSamples = static_cast<int> (settings.rollWindowMs * 0.001 * sampleRate);
        refractorySamples = static_cast<int> (0.025 * sampleRate);
    }

    // Feed one key sample; returns the reduction to apply right now, in dB (>= 0).
    double process (double key)
    {
        const double low = keyLowPass.process (key);
        const double level = std::abs (low);
        env = level + (level > env ? attackCoef : releaseCoef) * (env - level);

        const double amount = std::clamp ((gainToDb (env) - settings.thresholdDb) / 12.0, 0.0, 1.0);
        double targetDb = amount * settings.depthDb;

        if (settings.rollHold)
        {
            // Hit onsets: short-term energy jumps well above the recent average.
            // Comparing fast against slow energy ignores the ripple within one hit.
            const double power = low * low;
            onsetFast = power + onsetFastCoef * (onsetFast - power);
            onsetSlow = power + onsetSlowCoef * (onsetSlow - power);

            const bool onset = samplesSinceOnset > refractorySamples
                               && onsetFast > dbToGain (2.0 * settings.thresholdDb)
                               && onsetFast > 2.5 * onsetSlow;

            if (onset)
            {
                inRoll = samplesSinceOnset < rollWindowSamples;
                samplesSinceOnset = 0;
            }
            else if (samplesSinceOnset < neverHit)
            {
                ++samplesSinceOnset;
            }

            if (samplesSinceOnset > rollWindowSamples)
                inRoll = false;

            if (inRoll)
                targetDb = settings.rollDepthDb;
        }
        else
        {
            inRoll = false;
        }

        // Fast into a duck, smooth everywhere else so roll transitions never step.
        const double smoothCoef = targetDb > smoothedDb && ! inRoll ? smoothFastCoef : smoothSlowCoef;
        smoothedDb = targetDb + smoothCoef * (smoothedDb - targetDb);
        return smoothedDb;
    }

private:
    static constexpr int neverHit = 1 << 30;

    DuckSettings settings;
    double sampleRate = 44100.0;
    Biquad keyLowPass;

    double env = 0.0, attackCoef = 0.0, releaseCoef = 0.0;
    double onsetFast = 0.0, onsetSlow = 0.0, onsetFastCoef = 0.0, onsetSlowCoef = 0.0;
    double smoothedDb = 0.0, smoothFastCoef = 0.0, smoothSlowCoef = 0.0;
    int samplesSinceOnset = neverHit, rollWindowSamples = 0, refractorySamples = 0;
    bool inRoll = false;
};

} // namespace subforge
