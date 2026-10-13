#pragma once

#include "Filters.h"
#include "SubVoice.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace subforge
{

struct EngineSettings
{
    VoiceSettings voice;

    double tuneSemis = 0.0;
    double dropBeats = 4.0;       // drop length in beats, converted with the host tempo

    double lowCutHz = 25.0;       // removes sub-audible energy that only eats headroom
    double crossoverHz = 100.0;   // harmonics layer is kept above this, sub stays below it
    double driveDb = 3.0;         // push into the sub clipper for weight and density
    double ceilingDb = -6.0;      // hard upper limit for the whole instrument's peak

    double duckDepthDb = 8.0;     // max reduction when the sidechain (kick) hits
    double duckThresholdDb = -30.0;
    double duckReleaseMs = 120.0;

    // Roll Hold: when kicks come faster than rollWindowMs (double kick, blasts),
    // stop pumping on every hit and hold a steady, lighter reduction instead.
    // The first kick of a run still gets the full duck.
    bool rollHold = false;
    double rollWindowMs = 150.0;
    double rollDepthDb = 2.0;

    bool phoneCheck = false;      // monitor-only: hear roughly what a phone speaker plays
    double outputDb = 0.0;
};

// Monophonic boom/drop instrument plus its low-end control chain:
//
//   voice ─┬─ sub ──── low cut ── drive ── soft clip (ceiling) ─┐
//          ├─ harmonics ── high-pass at crossover ──────────────┼─ duck ── safety clip ── output ── [phone check]
//          └─ click ────────────────────────────────────────────┘
//
// The sub stays mono and below the ceiling; the harmonics carry the drop on
// small speakers; the sidechain (normally the kick) ducks it so peaks never stack.
class SubEngine
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        voice.prepare (sampleRate);
        tail.prepare (sampleRate);
        heldNotes.clear();
        heldNotes.reserve (128);
        filtersDirty = true;
        reset();
        setSettings (settings);
    }

    void reset()
    {
        lowCut.reset();
        harmHighPass.reset();
        sidechainLowPass.reset();
        phoneHighPass.reset();
        duckEnv = 0.0;
        onsetFast = onsetSlow = 0.0;
        samplesSinceOnset = 1 << 30;
        inRoll = false;
        duckSmoothedDb = 0.0;
        voice = SubVoice();
        tail = SubVoice();
        voice.prepare (sampleRate);
        tail.prepare (sampleRate);
        heldNotes.clear();
    }

    void setTempo (double bpm)
    {
        if (bpm > 0.0)
            tempoBpm = bpm;
    }

    void setSettings (const EngineSettings& s)
    {
        filtersDirty = filtersDirty
                       || std::abs (s.lowCutHz - settings.lowCutHz) > 1.0e-9
                       || std::abs (s.crossoverHz - settings.crossoverHz) > 1.0e-9;

        if (s.phoneCheck != settings.phoneCheck)
            phoneHighPass.reset();

        settings = s;
        updateFilters();

        settings.voice.dropSeconds = settings.dropBeats * 60.0 / tempoBpm;
        voice.updateSettings (settings.voice);
        tail.updateSettings (settings.voice);

        driveGain = dbToGain (settings.driveDb);
        ceiling = dbToGain (settings.ceilingDb);
        outputGain = dbToGain (settings.outputDb);
        duckReleaseCoef = std::exp (-1.0 / std::max (1.0, settings.duckReleaseMs * 0.001 * sampleRate));
        duckAttackCoef = std::exp (-1.0 / (0.001 * sampleRate));
        onsetFastCoef = std::exp (-1.0 / (0.002 * sampleRate));
        onsetSlowCoef = std::exp (-1.0 / (0.030 * sampleRate));
        smoothFastCoef = std::exp (-1.0 / (0.001 * sampleRate));
        smoothSlowCoef = std::exp (-1.0 / (0.008 * sampleRate));
        rollWindowSamples = static_cast<int> (settings.rollWindowMs * 0.001 * sampleRate);
        refractorySamples = static_cast<int> (0.025 * sampleRate);
    }

    void noteOn (int note, float velocity)
    {
        const bool legato = voice.isActive() && ! heldNotes.empty() && settings.voice.glideMs > 0.0;

        removeHeld (note);
        heldNotes.push_back (note);

        if (legato)
        {
            voice.glideTo (note + settings.tuneSemis);
            return;
        }

        // Choke the previous hit with a short fade, then start the new one at
        // phase zero so every hit has the same, predictable start.
        if (voice.isActive())
        {
            tail = voice;
            tail.fadeOut (5.0);
        }

        voice.start (settings.voice, note + settings.tuneSemis, velocity);
    }

    void noteOff (int note)
    {
        const bool wasCurrent = ! heldNotes.empty() && heldNotes.back() == note;
        removeHeld (note);

        if (! wasCurrent)
            return;

        if (! heldNotes.empty() && settings.voice.glideMs > 0.0 && voice.isActive())
            voice.glideTo (heldNotes.back() + settings.tuneSemis);
        else if (heldNotes.empty())
            voice.release();
    }

    void allNotesOff()
    {
        heldNotes.clear();

        if (voice.isActive())
        {
            tail = voice;
            tail.fadeOut (5.0);
            voice = SubVoice();
            voice.prepare (sampleRate);
        }
    }

    // sidechainL/R may be null when nothing is routed into the sidechain.
    void process (float* outL, float* outR, const float* sidechainL, const float* sidechainR, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const VoiceOutput a = voice.render();
            const VoiceOutput b = tail.render();

            // Sub: strip sub-audible rumble, then drive into a clipper capped at the ceiling.
            const double sub = lowCut.process (a.sub + b.sub);
            const double driven = sub * driveGain;
            const double clipped = softClip (driven, ceiling);

            const double drivenMag = std::abs (driven);
            if (drivenMag > 1.0e-6)
                blockSubReductionDb = std::max (blockSubReductionDb, gainToDb (drivenMag) - gainToDb (std::abs (clipped)));

            const double harm = harmHighPass.process (a.harmonics + b.harmonics);

            // Duck from the low end of the sidechain only (the kick, not its hats).
            double duckGain = 1.0;

            if (sidechainL != nullptr || sidechainR != nullptr)
            {
                double sc = 0.0;
                if (sidechainL != nullptr) sc += sidechainL[i];
                if (sidechainR != nullptr) sc += sidechainR[i];
                if (sidechainL != nullptr && sidechainR != nullptr) sc *= 0.5;

                const double low = sidechainLowPass.process (sc);
                const double level = std::abs (low);
                const double coef = level > duckEnv ? duckAttackCoef : duckReleaseCoef;
                duckEnv = level + coef * (duckEnv - level);

                const double amount = std::clamp ((gainToDb (duckEnv) - settings.duckThresholdDb) / 12.0, 0.0, 1.0);
                double targetDb = amount * settings.duckDepthDb;

                if (settings.rollHold)
                {
                    // Kick onsets: short-term energy jumps well above the recent average.
                    // Comparing fast against slow energy ignores the ripple within one kick.
                    const double power = low * low;
                    onsetFast = power + onsetFastCoef * (onsetFast - power);
                    onsetSlow = power + onsetSlowCoef * (onsetSlow - power);

                    const double thresholdPower = dbToGain (2.0 * settings.duckThresholdDb);
                    const bool onset = samplesSinceOnset > refractorySamples
                                       && onsetFast > thresholdPower
                                       && onsetFast > 2.5 * onsetSlow;

                    if (onset)
                    {
                        inRoll = samplesSinceOnset < rollWindowSamples;
                        samplesSinceOnset = 0;
                    }
                    else if (samplesSinceOnset < (1 << 30))
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
                const double smoothCoef = targetDb > duckSmoothedDb && ! inRoll ? smoothFastCoef : smoothSlowCoef;
                duckSmoothedDb = targetDb + smoothCoef * (duckSmoothedDb - targetDb);

                duckGain = dbToGain (-duckSmoothedDb);
                blockDuckReductionDb = std::max (blockDuckReductionDb, duckSmoothedDb);
            }

            double out = (clipped + harm + a.click + b.click) * duckGain;
            out = softClip (out, ceiling) * outputGain;

            if (settings.phoneCheck)
                out = phoneHighPass.process (out);

            blockPeak = std::max (blockPeak, std::abs (out));

            const float sample = static_cast<float> (out);
            outL[i] = sample;
            if (outR != nullptr)
                outR[i] = sample;
        }
    }

    // Meter values accumulated since the last call; reading resets them.
    struct Meters
    {
        double outputPeakDb = -180.0;
        double subReductionDb = 0.0;
        double duckReductionDb = 0.0;
    };

    Meters takeMeters()
    {
        Meters m { gainToDb (blockPeak), blockSubReductionDb, blockDuckReductionDb };
        blockPeak = 0.0;
        blockSubReductionDb = 0.0;
        blockDuckReductionDb = 0.0;
        return m;
    }

    bool isSounding() const { return voice.isActive() || tail.isActive(); }

private:
    void updateFilters()
    {
        if (! filtersDirty)
            return;

        lowCut.setHighPass (sampleRate, settings.lowCutHz);
        harmHighPass.setHighPass (sampleRate, settings.crossoverHz);
        sidechainLowPass.setLowPass (sampleRate, 150.0, 0.7071);
        phoneHighPass.setHighPass (sampleRate, 150.0);
        filtersDirty = false;
    }

    void removeHeld (int note)
    {
        heldNotes.erase (std::remove (heldNotes.begin(), heldNotes.end(), note), heldNotes.end());
    }

    EngineSettings settings;
    double sampleRate = 44100.0;
    double tempoBpm = 120.0;

    SubVoice voice, tail;
    std::vector<int> heldNotes;

    Butterworth4 lowCut, harmHighPass, phoneHighPass;
    Biquad sidechainLowPass;
    bool filtersDirty = true;

    double driveGain = 1.0, ceiling = 0.5, outputGain = 1.0;
    double duckEnv = 0.0, duckAttackCoef = 0.0, duckReleaseCoef = 0.0;
    double onsetFast = 0.0, onsetSlow = 0.0, onsetFastCoef = 0.0, onsetSlowCoef = 0.0;
    double duckSmoothedDb = 0.0, smoothFastCoef = 0.0, smoothSlowCoef = 0.0;
    int samplesSinceOnset = 1 << 30, rollWindowSamples = 0, refractorySamples = 0;
    bool inRoll = false;

    double blockPeak = 0.0, blockSubReductionDb = 0.0, blockDuckReductionDb = 0.0;
};

} // namespace subforge
