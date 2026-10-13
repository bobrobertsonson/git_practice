#pragma once

#include "Filters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace subforge
{

struct ImpactSettings
{
    double level = 0.0;       // 0 = off
    double toneHz = 4000.0;   // low-pass on the noise: darker (1 kHz) to brighter (12 kHz)
    double lengthMs = 400.0;  // time for the burst to fall 60 dB
    double space = 0.4;       // 0 = dry burst, 1 = big room
};

// Small stereo room (Freeverb-style: parallel damped combs, then allpasses).
// Left and right use slightly different delay lengths so the tail is wide.
class Room
{
public:
    void prepare (double sampleRate)
    {
        static constexpr int combTunings[] = { 1116, 1188, 1277, 1356 };
        static constexpr int allpassTunings[] = { 556, 441 };
        static constexpr int stereoSpread = 23;
        const double scale = sampleRate / 44100.0;

        for (int ch = 0; ch < 2; ++ch)
        {
            for (size_t i = 0; i < combs[ch].size(); ++i)
                combs[ch][i].setSize (static_cast<int> ((combTunings[i] + ch * stereoSpread) * scale));
            for (size_t i = 0; i < allpasses[ch].size(); ++i)
                allpasses[ch][i].setSize (static_cast<int> ((allpassTunings[i] + ch * stereoSpread) * scale));
        }
    }

    void reset()
    {
        for (auto& channel : combs)
            for (auto& c : channel)
                c.clear();
        for (auto& channel : allpasses)
            for (auto& a : channel)
                a.clear();
    }

    void setSize (double size) { feedback = 0.70 + 0.18 * std::clamp (size, 0.0, 1.0); }

    void process (double in, double& left, double& right)
    {
        double out[2] = {};
        for (int ch = 0; ch < 2; ++ch)
        {
            for (auto& c : combs[ch])
                out[ch] += c.process (in, feedback, 0.3);
            for (auto& a : allpasses[ch])
                out[ch] = a.process (out[ch]);
        }
        left = out[0] * 0.25;
        right = out[1] * 0.25;
    }

private:
    struct Comb
    {
        std::vector<double> buffer;
        size_t pos = 0;
        double store = 0.0;

        void setSize (int n) { buffer.assign (static_cast<size_t> (std::max (1, n)), 0.0); pos = 0; store = 0.0; }
        void clear() { std::fill (buffer.begin(), buffer.end(), 0.0); store = 0.0; }

        double process (double in, double feedback, double damping)
        {
            const double out = buffer[pos];
            store = out * (1.0 - damping) + store * damping;
            buffer[pos] = in + store * feedback;
            pos = (pos + 1) % buffer.size();
            return out;
        }
    };

    struct Allpass
    {
        std::vector<double> buffer;
        size_t pos = 0;

        void setSize (int n) { buffer.assign (static_cast<size_t> (std::max (1, n)), 0.0); pos = 0; }
        void clear() { std::fill (buffer.begin(), buffer.end(), 0.0); }

        double process (double in)
        {
            const double delayed = buffer[pos];
            buffer[pos] = in + delayed * 0.5;
            pos = (pos + 1) % buffer.size();
            return delayed - in;
        }
    };

    std::array<std::array<Comb, 4>, 2> combs;
    std::array<std::array<Allpass, 2>, 2> allpasses;
    double feedback = 0.8;
};

// The "heard" part of a hit: a noise burst kept above the sub (high-passed at
// 150 Hz), shaped by Tone and Length, sent into a small room for Space. Fired on
// every new note so the sub, the burst and the tail always line up.
class Impact
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        room.prepare (sampleRate);
        highPass.setHighPass (sampleRate, 150.0);
        tonePrepared = false;
        setSettings (settings);
        reset();
    }

    void reset()
    {
        env = 0.0;
        room.reset();
        highPass.reset();
        lowPass.reset();
    }

    void setSettings (const ImpactSettings& s)
    {
        if (std::abs (s.toneHz - settings.toneHz) > 1.0e-9 || ! tonePrepared)
        {
            lowPass.setLowPass (sampleRate, std::min (s.toneHz, 0.45 * sampleRate));
            tonePrepared = true;
        }

        settings = s;
        decayCoef = std::exp (std::log (0.001) / std::max (1.0, settings.lengthMs * 0.001 * sampleRate));
        room.setSize (settings.space);
    }

    void trigger (double velocityGain) { env = velocityGain; }

    bool isActive() const { return settings.level > 0.0; }

    void process (double& left, double& right)
    {
        double burst = 0.0;
        if (env > 1.0e-5)
        {
            burst = highPass.process (lowPass.process (nextNoise())) * env;
            env *= decayCoef;
        }

        double wetL = 0.0, wetR = 0.0;
        room.process (burst, wetL, wetR);

        const double dry = 1.0 - 0.6 * settings.space;
        const double wet = 1.6 * settings.space;
        left = settings.level * (burst * dry + wetL * wet);
        right = settings.level * (burst * dry + wetR * wet);
    }

private:
    double nextNoise()
    {
        noiseState ^= noiseState << 13;
        noiseState ^= noiseState >> 17;
        noiseState ^= noiseState << 5;
        return static_cast<double> (noiseState) / 2147483648.0 - 1.0;
    }

    ImpactSettings settings;
    double sampleRate = 44100.0;
    bool tonePrepared = false;

    Butterworth4 lowPass, highPass;
    Room room;
    double env = 0.0, decayCoef = 0.0;
    uint32_t noiseState = 0x12345678u;
};

} // namespace subforge
