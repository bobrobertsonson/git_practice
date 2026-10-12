#pragma once

#include "Filters.h"

#include <algorithm>
#include <cmath>

namespace subforge
{

enum class Mode
{
    boom = 0, // 808-style hit: pitch punch, then exponential decay
    drop = 1  // sub sweep: falls a set number of semitones over a tempo-synced length
};

struct VoiceSettings
{
    Mode mode = Mode::boom;

    double punchSemis = 7.0;     // boom: how far above the note the hit starts
    double punchMs = 40.0;       // boom: how quickly the punch settles onto the note
    double decayMs = 800.0;      // time to fall 60 dB (after the drop finishes, in drop mode)
    double releaseMs = 60.0;     // used on note-off when oneShot is false
    bool oneShot = true;         // ignore note length and let the hit ring out
    double glideMs = 0.0;        // legato pitch glide between overlapping notes
    double clickLevel = 0.2;     // transient layer for small-speaker definition
    double velocitySens = 0.5;   // 0 = every hit full level, 1 = fully velocity driven

    double dropSemis = 24.0;     // drop: how far the pitch falls
    double dropSeconds = 1.7;    // drop: length of the fall (the engine syncs this to tempo)
    double dropCurve = 0.3;      // -1 = fast start, 0 = linear, +1 = slow start, fast end

    double harmonics = 0.35;     // amount of audible harmonics layer
    double autoHarmonics = 0.6;  // extra harmonics as the pitch falls below ~100 Hz
};

// What one voice produces for one sample. The sub and harmonics are kept apart
// so the engine can filter, clip and level them independently.
struct VoiceOutput
{
    double sub = 0.0;
    double harmonics = 0.0;
    double click = 0.0;
};

// One monophonic sine voice that knows its exact pitch at every sample. Since
// the pitch is known, the harmonic layer is drawn straight from the phase, so it
// is alias-free and tracks the drop perfectly, with no pitch detection or waveshaping.
class SubVoice
{
public:
    void prepare (double newSampleRate) { sampleRate = newSampleRate; }

    bool isActive() const { return active; }
    double getCurrentFreq() const { return currentFreq; }

    void start (const VoiceSettings& s, double note, double velocity)
    {
        settings = s;
        active = true;
        releasing = false;
        fadeGain = 1.0;
        fadeStep = 0.0;
        phase = 0.0;
        t = 0.0;
        env = 0.0;
        baseNote = note;
        targetNote = note;
        level = 1.0 - settings.velocitySens * (1.0 - std::clamp (velocity, 0.0, 1.0));
        clickPhase = 0.0;
        clickEnv = 1.0;
        updateCoefficients();
    }

    // Legato: move to a new note without retriggering the envelope or phase.
    void glideTo (double note)
    {
        targetNote = note;

        if (settings.glideMs <= 0.0)
            baseNote = note;

        releasing = false;
    }

    void release()
    {
        if (! settings.oneShot)
            releasing = true;
    }

    // Short linear fade used when a new note chokes this one, so the cut never clicks.
    void fadeOut (double ms)
    {
        fadeStep = 1.0 / std::max (1.0, ms * 0.001 * sampleRate);
    }

    void updateSettings (const VoiceSettings& s)
    {
        settings = s;
        updateCoefficients();
    }

    VoiceOutput render()
    {
        VoiceOutput out;

        if (! active)
            return out;

        // Pitch
        if (std::abs (targetNote - baseNote) > 0.0)
        {
            baseNote += (targetNote - baseNote) * glideCoef;

            if (std::abs (targetNote - baseNote) < 1.0e-4)
                baseNote = targetNote;
        }

        double note = baseNote;

        if (settings.mode == Mode::boom)
        {
            note += settings.punchSemis * std::exp (-t / std::max (1.0e-4, settings.punchMs * 0.001));
        }
        else
        {
            const double length = std::max (1.0e-3, settings.dropSeconds);
            const double x = std::min (1.0, t / length);
            note -= settings.dropSemis * std::pow (x, curveExponent);
        }

        currentFreq = std::max (12.0, midiNoteToHz (note));

        // Amplitude envelope
        const double attackSeconds = settings.mode == Mode::boom ? 0.0005 : 0.002;
        const bool inAttack = t < attackSeconds;
        const bool holding = settings.mode == Mode::drop && t < settings.dropSeconds;

        if (inAttack)
            env = t / attackSeconds;
        else if (releasing)
            env *= releaseCoef;
        else if (! holding)
            env *= decayCoef;
        else
            env = 1.0;

        if (fadeStep > 0.0)
        {
            fadeGain -= fadeStep;

            if (fadeGain <= 0.0)
            {
                active = false;
                return out;
            }
        }

        const double amp = env * level * fadeGain;

        // Sub fundamental
        const double twoPiPhase = 2.0 * kPi * phase;
        out.sub = amp * std::sin (twoPiPhase);

        // Harmonic layer: more of it the lower the pitch goes, so a falling drop
        // stays audible without anyone pushing the sub level up.
        const double lowness = std::clamp (std::log2 (100.0 / currentFreq), 0.0, 2.5);
        const double harmAmount = settings.harmonics * (1.0 + settings.autoHarmonics * lowness);

        if (harmAmount > 0.0)
        {
            static constexpr double weights[] = { 1.0, 0.7, 0.45, 0.3, 0.2 }; // harmonics 2..6
            static constexpr double weightSum = 2.65;
            double sum = 0.0;

            for (int i = 0; i < 5; ++i)
            {
                const int k = i + 2;

                if (k * currentFreq >= 0.45 * sampleRate)
                    break;

                sum += weights[i] * std::sin (k * twoPiPhase);
            }

            out.harmonics = amp * harmAmount * sum / weightSum;
        }

        // Click transient
        if (settings.clickLevel > 0.0 && clickEnv > 1.0e-4)
        {
            out.click = settings.clickLevel * level * fadeGain * clickEnv * std::sin (2.0 * kPi * clickPhase);
            clickPhase += 1800.0 / sampleRate;
            clickEnv *= clickCoef;
        }

        phase += currentFreq / sampleRate;
        phase -= std::floor (phase);
        t += 1.0 / sampleRate;

        if (! inAttack && (releasing || ! holding) && env < 1.0e-5)
            active = false;

        return out;
    }

private:
    void updateCoefficients()
    {
        const auto t60Coef = [this] (double ms)
        {
            return std::exp (std::log (0.001) / std::max (1.0, ms * 0.001 * sampleRate));
        };

        decayCoef = t60Coef (settings.decayMs);
        releaseCoef = t60Coef (settings.releaseMs);
        clickCoef = std::exp (-1.0 / (0.0012 * sampleRate));
        glideCoef = settings.glideMs > 0.0 ? 1.0 - std::exp (-1.0 / (settings.glideMs * 0.001 * sampleRate / 3.0))
                                           : 1.0;

        const double c = std::clamp (settings.dropCurve, -1.0, 1.0);
        curveExponent = c >= 0.0 ? 1.0 + 3.0 * c : 1.0 / (1.0 - 3.0 * c);
    }

    VoiceSettings settings;
    double sampleRate = 44100.0;

    bool active = false;
    bool releasing = false;
    double fadeGain = 1.0, fadeStep = 0.0;

    double phase = 0.0, t = 0.0, env = 0.0, level = 1.0;
    double baseNote = 36.0, targetNote = 36.0, currentFreq = 65.0;

    double clickPhase = 0.0, clickEnv = 0.0;

    double decayCoef = 0.0, releaseCoef = 0.0, clickCoef = 0.0, glideCoef = 1.0, curveExponent = 1.0;
};

} // namespace subforge
