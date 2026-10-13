#pragma once

#include "Ducker.h"
#include "Filters.h"
#include "Impact.h"
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
    double dropMs = 0.0;          // > 0: free drop length in ms instead (ignores tempo)

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

    ImpactSettings impact;        // noise burst + room fired with every new note

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
        ducker.prepare (sampleRate);
        impact.prepare (sampleRate);
        reset();
        setSettings (settings);
    }

    void reset()
    {
        lowCut.reset();
        harmHighPass.reset();
        phoneHighPass.reset();
        ducker.reset();
        impact.reset();
        phoneHighPassR.reset();
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
        {
            phoneHighPass.reset();
            phoneHighPassR.reset();
        }

        settings = s;
        updateFilters();

        settings.voice.dropSeconds = settings.dropMs > 0.0 ? settings.dropMs * 0.001 : settings.dropBeats * 60.0 / tempoBpm;
        voice.updateSettings (settings.voice);
        tail.updateSettings (settings.voice);

        driveGain = dbToGain (settings.driveDb);
        ceiling = dbToGain (settings.ceilingDb);
        outputGain = dbToGain (settings.outputDb);
        impact.setSettings (settings.impact);
        ducker.setSettings ({ settings.duckDepthDb, settings.duckThresholdDb, settings.duckReleaseMs,
                              settings.rollHold, settings.rollWindowMs, settings.rollDepthDb });
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
        impact.trigger (1.0 - settings.voice.velocitySens * (1.0 - std::clamp (static_cast<double> (velocity), 0.0, 1.0)));
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

                const double reductionDb = ducker.process (sc);
                duckGain = dbToGain (-reductionDb);
                blockDuckReductionDb = std::max (blockDuckReductionDb, reductionDb);
            }

            const double mono = (clipped + harm + a.click + b.click) * duckGain;

            // The impact layer sits on top in stereo; it has no low end, so the
            // kick duck leaves it alone. The ceiling still caps every channel.
            double impactL = 0.0, impactR = 0.0;
            if (impact.isActive())
            {
                impact.process (impactL, impactR);
                if (outR == nullptr)
                    impactL = impactR = 0.5 * (impactL + impactR);
            }

            double left = softClip (mono + impactL, ceiling) * outputGain;
            double right = softClip (mono + impactR, ceiling) * outputGain;

            if (settings.phoneCheck)
            {
                left = phoneHighPass.process (left);
                right = phoneHighPassR.process (right);
            }

            blockPeak = std::max (blockPeak, std::max (std::abs (left), std::abs (right)));

            outL[i] = static_cast<float> (left);
            if (outR != nullptr)
                outR[i] = static_cast<float> (right);
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
        phoneHighPass.setHighPass (sampleRate, 150.0);
        phoneHighPassR.setHighPass (sampleRate, 150.0);
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

    Butterworth4 lowCut, harmHighPass, phoneHighPass, phoneHighPassR;
    bool filtersDirty = true;

    double driveGain = 1.0, ceiling = 0.5, outputGain = 1.0;
    Ducker ducker;
    Impact impact;

    double blockPeak = 0.0, blockSubReductionDb = 0.0, blockDuckReductionDb = 0.0;
};

} // namespace subforge
