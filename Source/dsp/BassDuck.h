#pragma once

#include "Ducker.h"
#include "Filters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

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
    bool fullBand = false;    // duck the whole signal (e.g. a guitar bus dipping for each hit)
    DuckSettings duck;

    // Look-ahead: start ducking before the key hits, so the low end is already
    // empty when the boom lands. 0 = off. The track is delayed by a fixed
    // maxLookAheadMs whenever this is on; the host compensates for it.
    double lookAheadMs = 0.0;
    double gap = 0.5;   // share of the look-ahead held at full depth before the hit; the rest fades down
};

inline constexpr double maxLookAheadMs = 750.0;

inline int lookAheadLatencySamples (double sampleRate)
{
    return static_cast<int> (std::lround (maxLookAheadMs * 0.001 * sampleRate));
}

// Turns the ducker's reduction into one that starts early. The output runs
// `latency` samples behind the key, so for each output sample the reductions
// coming up in the next `window` samples are already known:
//
//   reduction(n) = max( r(m) over the gap before the hit,
//                       r(m) - slope * distance, over the fade before that )
//
// Both maxima are sliding-window maxima, kept in O(1) per sample with
// monotonic queues (the fade uses r(m) - slope * m, which keeps its order as
// the window slides).
class LookAhead
{
public:
    void prepare (int newLatency)
    {
        latency = newLatency;
        const auto size = static_cast<size_t> (latency + 1);
        history.assign (size, 0.0);
        holdQueue.init (size + 1);
        fadeQueue.init (size + 1);
        reset();
    }

    void reset()
    {
        std::fill (history.begin(), history.end(), 0.0);
        holdQueue.clear();
        fadeQueue.clear();
        now = 0;
        nextHold = nextFade = 0;
    }

    void setShape (int windowSamples, double gap, double depthDb)
    {
        window = std::clamp (windowSamples, 0, latency);
        hold = static_cast<int> (std::lround (window * std::clamp (gap, 0.0, 1.0)));
        const double newSlope = depthDb / std::max (1, window - hold);

        if (std::abs (newSlope - slope) > 1.0e-12)
        {
            // Queued fade values were ordered with the old slope; rebuild them from history.
            slope = newSlope;
            fadeQueue.clear();
            nextFade = std::max<std::int64_t> (0, now - latency + hold + 1);
        }
    }

    // Push the reduction the key calls for now; returns the reduction for the
    // (delayed) output sample being written now.
    double process (double reductionDb)
    {
        history[slot (now)] = reductionDb;

        const std::int64_t hitAt = now - latency;  // key index lined up with this output sample
        const std::int64_t holdEnd = hitAt + hold;
        const std::int64_t fadeEnd = hitAt + window;

        for (; nextFade <= fadeEnd; ++nextFade)
            if (nextFade > holdEnd)
                push (fadeQueue, nextFade, true);
        nextFade = std::max (nextFade, holdEnd + 1);
        while (! fadeQueue.empty() && fadeQueue.front() <= holdEnd)
            fadeQueue.popFront();

        for (; nextHold <= holdEnd; ++nextHold)
            push (holdQueue, nextHold, false);
        while (! holdQueue.empty() && holdQueue.front() < hitAt)
            holdQueue.popFront();

        double out = 0.0;
        if (! holdQueue.empty())
            out = history[slot (holdQueue.front())];
        if (! fadeQueue.empty())
            out = std::max (out, fadeValue (fadeQueue.front()) + slope * static_cast<double> (holdEnd));

        ++now;
        return std::max (0.0, out);
    }

private:
    // Fixed-size ring of sample indices.
    struct IndexQueue
    {
        std::vector<std::int64_t> items;
        size_t head = 0, count = 0;

        void init (size_t capacity) { items.assign (capacity, 0); clear(); }
        void clear() { head = count = 0; }
        bool empty() const { return count == 0; }
        std::int64_t front() const { return items[head]; }
        std::int64_t back() const { return items[(head + count - 1) % items.size()]; }
        void popFront() { head = (head + 1) % items.size(); --count; }
        void popBack() { --count; }
        void pushBack (std::int64_t i)
        {
            if (count == items.size())
                popFront();
            items[(head + count) % items.size()] = i;
            ++count;
        }
    };

    size_t slot (std::int64_t index) const { return static_cast<size_t> (index % static_cast<std::int64_t> (history.size())); }
    double fadeValue (std::int64_t index) const { return history[slot (index)] - slope * static_cast<double> (index); }

    void push (IndexQueue& q, std::int64_t index, bool fade)
    {
        if (index < 0 || index < now - latency || index > now)
            return;
        const auto value = [&] (std::int64_t i) { return fade ? fadeValue (i) : history[slot (i)]; };
        const double v = value (index);
        while (! q.empty() && value (q.back()) <= v)
            q.popBack();
        q.pushBack (index);
    }

    std::vector<double> history;
    IndexQueue holdQueue, fadeQueue;
    int latency = 0, window = 0, hold = 0;
    double slope = 0.0;
    std::int64_t now = 0, nextHold = 0, nextFade = 0;
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
        latency = lookAheadLatencySamples (sampleRate);
        lookAhead.prepare (latency);
        for (auto& d : delays)
            d.assign (static_cast<size_t> (latency), 0.0f);
        splitDirty = true;
        setSettings (settings);
        reset();
    }

    void reset()
    {
        for (auto& x : crossovers)
            x.reset();
        ducker.reset();
        lookAhead.reset();
        for (auto& d : delays)
            std::fill (d.begin(), d.end(), 0.0f);
        delayPos = 0;
    }

    // Delay this adds while look-ahead is on (report it to the host).
    int latencySamples() const { return lookAheadOn ? latency : 0; }

    void setSettings (const BassDuckSettings& s)
    {
        splitDirty = splitDirty || std::abs (s.splitHz - settings.splitHz) > 1.0e-9;
        settings = s;
        ducker.setSettings (settings.duck);

        const bool wantLookAhead = settings.lookAheadMs > 0.0 && latency > 0;
        if (wantLookAhead != lookAheadOn)
        {
            lookAheadOn = wantLookAhead;
            reset();
        }
        lookAhead.setShape (static_cast<int> (std::lround (settings.lookAheadMs * 0.001 * sampleRate)),
                            settings.gap, settings.duck.depthDb);

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

            double reductionDb = (keyL != nullptr || keyR != nullptr) ? ducker.process (key) : 0.0;
            if (lookAheadOn)
                reductionDb = lookAhead.process (reductionDb);
            const double lowGain = dbToGain (-reductionDb);
            blockReductionDb = std::max (blockReductionDb, reductionDb);

            float* channels[] = { left, right };
            for (int ch = 0; ch < 2; ++ch)
            {
                if (channels[ch] == nullptr)
                    continue;

                double input = channels[ch][i];
                if (lookAheadOn)
                {
                    // Delay the track so the key is heard `latency` samples ahead of it.
                    auto& slot = delays[static_cast<size_t> (ch)][delayPos];
                    input = slot;
                    slot = channels[ch][i];
                }

                double low = 0.0, high = 0.0;
                crossovers[ch].process (input, low, high);
                // Full band keeps the same crossover path so switching modes never changes the tone.
                channels[ch][i] = static_cast<float> (settings.fullBand ? (low + high) * lowGain : low * lowGain + high);
            }

            if (lookAheadOn && ++delayPos == static_cast<size_t> (latency))
                delayPos = 0;
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
    LookAhead lookAhead;
    std::array<std::vector<float>, 2> delays;
    size_t delayPos = 0;
    int latency = 0;
    bool lookAheadOn = false;
    double blockReductionDb = 0.0;
};

} // namespace subforge
