#pragma once

#include "Ducker.h"
#include "Filters.h"

#include <algorithm>
#include <array>

namespace subforge
{

// 4th-order Linkwitz-Riley crossover. Low + high add back up to a flat
// magnitude response, so with no ducking the bass comes out unchanged
// (only a gentle phase shift around the split, like any analog crossover).
class LinkwitzRiley4
{
public:
    void setSplit (double sampleRate, double freq)
    {
        for (auto& f : lows)  f.setLowPass (sampleRate, freq, 0.7071067811865476);
        for (auto& f : highs) f.setHighPass (sampleRate, freq, 0.7071067811865476);
    }

    void reset()
    {
        for (auto& f : lows)  f.reset();
        for (auto& f : highs) f.reset();
    }

    void process (double x, double& low, double& high)
    {
        low = lows[1].process (lows[0].process (x));
        high = highs[1].process (highs[0].process (x));
    }

private:
    std::array<Biquad, 2> lows, highs;
};

struct BassDuckSettings
{
    double splitHz = 100.0;   // only the bass below this is ducked
    DuckSettings duck;
};

// SubForge Duck: put it on the bass, key it with SubForge (or the kick). It turns
// down only the bass's low end while the key plays; the grit above the split is
// never touched.
class BassDuck
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        ducker.prepare (sampleRate);
        splitDirty = true;
        setSettings (settings);
        reset();
    }

    void reset()
    {
        for (auto& x : crossovers)
            x.reset();
        ducker.reset();
    }

    void setSettings (const BassDuckSettings& s)
    {
        splitDirty = splitDirty || std::abs (s.splitHz - settings.splitHz) > 1.0e-9;
        settings = s;
        ducker.setSettings (settings.duck);

        if (splitDirty)
        {
            for (auto& x : crossovers)
                x.setSplit (sampleRate, settings.splitHz);
            splitDirty = false;
        }
    }

    // Processes the bass in place (1 or 2 channels). keyL/keyR may be null.
    void process (float* left, float* right, const float* keyL, const float* keyR, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            double key = 0.0;
            if (keyL != nullptr) key += keyL[i];
            if (keyR != nullptr) key += keyR[i];
            if (keyL != nullptr && keyR != nullptr) key *= 0.5;

            const double reductionDb = (keyL != nullptr || keyR != nullptr) ? ducker.process (key) : 0.0;
            const double lowGain = dbToGain (-reductionDb);
            blockReductionDb = std::max (blockReductionDb, reductionDb);

            float* channels[] = { left, right };
            for (int ch = 0; ch < 2; ++ch)
            {
                if (channels[ch] == nullptr)
                    continue;

                double low = 0.0, high = 0.0;
                crossovers[ch].process (channels[ch][i], low, high);
                channels[ch][i] = static_cast<float> (low * lowGain + high);
            }
        }
    }

    // Deepest reduction since the last call; reading resets it.
    double takeReductionDb()
    {
        const double r = blockReductionDb;
        blockReductionDb = 0.0;
        return r;
    }

private:
    BassDuckSettings settings;
    double sampleRate = 44100.0;
    bool splitDirty = true;

    std::array<LinkwitzRiley4, 2> crossovers;
    Ducker ducker;
    double blockReductionDb = 0.0;
};

} // namespace subforge
