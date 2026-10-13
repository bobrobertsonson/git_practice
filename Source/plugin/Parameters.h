#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <utility>
#include <vector>

namespace subforge::params
{

// Parameter IDs. Changing one breaks saved sessions, so treat them as permanent.
inline constexpr auto mode = "mode";
inline constexpr auto tune = "tune";
inline constexpr auto fine = "fine";
inline constexpr auto punch = "punch";
inline constexpr auto punchTime = "punchTime";
inline constexpr auto decay = "decay";
inline constexpr auto oneShot = "oneShot";
inline constexpr auto release = "release";
inline constexpr auto glide = "glide";
inline constexpr auto click = "click";
inline constexpr auto velocity = "velocity";

inline constexpr auto dropAmount = "dropAmount";
inline constexpr auto dropLength = "dropLength";
inline constexpr auto dropCurve = "dropCurve";
inline constexpr auto dropTime = "dropTime";

inline constexpr auto lowCut = "lowCut";
inline constexpr auto crossover = "crossover";
inline constexpr auto harmonics = "harmonics";
inline constexpr auto autoHarmonics = "autoHarmonics";
inline constexpr auto drive = "drive";
inline constexpr auto ceiling = "ceiling";

inline constexpr auto duckDepth = "duckDepth";
inline constexpr auto duckThreshold = "duckThreshold";
inline constexpr auto duckRelease = "duckRelease";
inline constexpr auto rollHold = "rollHold";
inline constexpr auto rollWindow = "rollWindow";
inline constexpr auto rollDepth = "rollDepth";
inline constexpr auto impactLevel = "impactLevel";
inline constexpr auto impactTone = "impactTone";
inline constexpr auto impactLength = "impactLength";
inline constexpr auto impactSpace = "impactSpace";
inline constexpr auto phoneCheck = "phoneCheck";
inline constexpr auto output = "output";

inline const juce::StringArray modeChoices { "Boom", "Drop" };
// The last choice, "Free", uses Drop Time (ms) instead of a beat length.
inline const juce::StringArray dropLengthChoices { "1/4 beat", "1/2 beat", "1 beat", "2 beats", "4 beats", "8 beats", "16 beats", "Free" };
inline constexpr double dropLengthBeats[] = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };
inline constexpr int dropLengthFree = 7;

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    const auto range = [] (float lo, float hi, float centre)
    {
        NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    };

    const auto unit = [] (const char* suffix, int decimals)
    {
        return AudioParameterFloatAttributes().withLabel (suffix).withStringFromValueFunction (
            [suffix, decimals] (float v, int) { return String (v, decimals) + " " + suffix; });
    };

    const auto percent = AudioParameterFloatAttributes().withStringFromValueFunction (
        [] (float v, int) { return String (roundToInt (v * 100.0f)) + " %"; });

    const auto add = [&layout] (auto p) { layout.add (std::move (p)); };

    add (std::make_unique<AudioParameterChoice> (ParameterID { mode, 1 }, "Mode", modeChoices, 0));
    add (std::make_unique<AudioParameterInt> (ParameterID { tune, 1 }, "Tune", -24, 24, 0,
                                              AudioParameterIntAttributes().withLabel ("st")));
    add (std::make_unique<AudioParameterFloat> (ParameterID { fine, 1 }, "Fine", NormalisableRange<float> (-100.0f, 100.0f), 0.0f, unit ("ct", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { punch, 1 }, "Punch", NormalisableRange<float> (0.0f, 36.0f), 7.0f, unit ("st", 1)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { punchTime, 1 }, "Punch Time", range (5.0f, 150.0f, 40.0f), 40.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { decay, 1 }, "Decay", range (50.0f, 12000.0f, 1000.0f), 800.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterBool> (ParameterID { oneShot, 1 }, "One Shot", true));
    add (std::make_unique<AudioParameterFloat> (ParameterID { release, 1 }, "Release", range (5.0f, 2000.0f, 150.0f), 60.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { glide, 1 }, "Glide", range (0.0f, 500.0f, 80.0f), 0.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { click, 1 }, "Click", NormalisableRange<float> (0.0f, 1.0f), 0.2f, percent));
    add (std::make_unique<AudioParameterFloat> (ParameterID { velocity, 1 }, "Velocity", NormalisableRange<float> (0.0f, 1.0f), 0.5f, percent));

    add (std::make_unique<AudioParameterInt> (ParameterID { dropAmount, 1 }, "Drop Amount", 1, 48, 24,
                                              AudioParameterIntAttributes().withLabel ("st")));
    add (std::make_unique<AudioParameterChoice> (ParameterID { dropLength, 1 }, "Drop Length", dropLengthChoices, 4));
    add (std::make_unique<AudioParameterFloat> (ParameterID { dropCurve, 1 }, "Drop Curve", NormalisableRange<float> (-1.0f, 1.0f), 0.3f, unit ("", 2)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { dropTime, 1 }, "Drop Time", range (50.0f, 16000.0f, 1000.0f), 1000.0f, unit ("ms", 0)));

    add (std::make_unique<AudioParameterFloat> (ParameterID { lowCut, 1 }, "Low Cut", range (15.0f, 60.0f, 30.0f), 25.0f, unit ("Hz", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { crossover, 1 }, "Crossover", range (60.0f, 200.0f, 110.0f), 100.0f, unit ("Hz", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { harmonics, 1 }, "Harmonics", NormalisableRange<float> (0.0f, 1.0f), 0.35f, percent));
    add (std::make_unique<AudioParameterFloat> (ParameterID { autoHarmonics, 1 }, "Auto Harmonics", NormalisableRange<float> (0.0f, 1.0f), 0.6f, percent));
    add (std::make_unique<AudioParameterFloat> (ParameterID { drive, 1 }, "Drive", NormalisableRange<float> (-12.0f, 18.0f), 3.0f, unit ("dB", 1)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { ceiling, 1 }, "Ceiling", NormalisableRange<float> (-18.0f, 0.0f), -6.0f, unit ("dB", 1)));

    add (std::make_unique<AudioParameterFloat> (ParameterID { duckDepth, 1 }, "Duck Depth", NormalisableRange<float> (0.0f, 24.0f), 8.0f, unit ("dB", 1)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { duckThreshold, 1 }, "Duck Threshold", NormalisableRange<float> (-48.0f, 0.0f), -30.0f, unit ("dB", 1)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { duckRelease, 1 }, "Duck Release", range (20.0f, 500.0f, 120.0f), 120.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { impactLevel, 1 }, "Impact", NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent));
    add (std::make_unique<AudioParameterFloat> (ParameterID { impactTone, 1 }, "Impact Tone", range (1000.0f, 12000.0f, 4000.0f), 4000.0f, unit ("Hz", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { impactLength, 1 }, "Impact Length", range (50.0f, 2000.0f, 400.0f), 400.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { impactSpace, 1 }, "Impact Space", NormalisableRange<float> (0.0f, 1.0f), 0.4f, percent));
    add (std::make_unique<AudioParameterBool> (ParameterID { rollHold, 1 }, "Roll Hold", false));
    add (std::make_unique<AudioParameterFloat> (ParameterID { rollWindow, 1 }, "Roll Window", range (60.0f, 250.0f, 140.0f), 150.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { rollDepth, 1 }, "Roll Depth", NormalisableRange<float> (0.0f, 12.0f), 2.0f, unit ("dB", 1)));
    add (std::make_unique<AudioParameterBool> (ParameterID { phoneCheck, 1 }, "Phone Check", false));
    add (std::make_unique<AudioParameterFloat> (ParameterID { output, 1 }, "Output", NormalisableRange<float> (-24.0f, 6.0f), 0.0f, unit ("dB", 1)));

    return layout;
}

} // namespace subforge::params

namespace subforge::presets
{

// Factory presets. Anything a preset doesn't list goes back to its default.
struct Preset
{
    const char* name;
    std::vector<std::pair<const char*, float>> values;
};

inline const std::vector<Preset> all {
    { "Default", {} },

    // --- Booms -------------------------------------------------------------

    // Matched to a reference track: a 30 Hz 808 that dives from ~120 Hz,
    // barely decays, and is cut off by the note end. Fine is set so B-1 in
    // Logic plays 30 Hz; set it to 0 to stay at concert pitch.
    { "Long 30 Hz Boom",
      { { params::tune, 0.0f }, { params::fine, -49.0f }, { params::punch, 28.0f }, { params::punchTime, 22.0f },
        { params::decay, 8000.0f }, { params::oneShot, 0.0f }, { params::release, 150.0f }, { params::click, 0.1f },
        { params::lowCut, 18.0f }, { params::crossover, 60.0f }, { params::harmonics, 0.15f },
        { params::autoHarmonics, 0.2f }, { params::drive, -3.0f }, { params::ceiling, -4.0f } } },

    // Melodic 808 for basslines: gated by note length, overlapping notes slide.
    { "Trap 808 Slide",
      { { params::punch, 5.0f }, { params::punchTime, 30.0f }, { params::decay, 2500.0f }, { params::oneShot, 0.0f },
        { params::release, 80.0f }, { params::glide, 90.0f }, { params::click, 0.15f }, { params::harmonics, 0.4f },
        { params::autoHarmonics, 0.5f }, { params::drive, 6.0f } } },

    // Short, bouncy hit with a strong attack for fast patterns.
    { "Short Punchy 808",
      { { params::punch, 12.0f }, { params::punchTime, 25.0f }, { params::decay, 350.0f }, { params::click, 0.35f },
        { params::harmonics, 0.45f }, { params::drive, 5.0f } } },

    // Gritty, saturated 808 that cuts through a busy mix.
    { "Distorted 808",
      { { params::punch, 9.0f }, { params::punchTime, 35.0f }, { params::decay, 1500.0f }, { params::drive, 14.0f },
        { params::harmonics, 0.6f }, { params::autoHarmonics, 0.4f }, { params::crossover, 90.0f } } },

    // Pure, deep sub with no dirt: for sparse mixes and big systems.
    { "Clean Sine Sub",
      { { params::punch, 3.0f }, { params::punchTime, 20.0f }, { params::decay, 3000.0f }, { params::oneShot, 0.0f },
        { params::release, 120.0f }, { params::click, 0.0f }, { params::harmonics, 0.15f },
        { params::autoHarmonics, 0.3f }, { params::drive, -8.0f }, { params::lowCut, 22.0f } } },

    // Extra harmonics so the 808 still reads on phones and laptops.
    { "Phone-Safe 808",
      { { params::decay, 1200.0f }, { params::click, 0.3f }, { params::harmonics, 0.7f },
        { params::autoHarmonics, 1.0f }, { params::crossover, 80.0f }, { params::drive, 4.0f } } },

    // One big cinematic hit: a deep pitch dive with a hard attack.
    { "Impact Boom",
      { { params::punch, 24.0f }, { params::punchTime, 18.0f }, { params::decay, 1800.0f }, { params::click, 0.5f },
        { params::harmonics, 0.4f }, { params::drive, 8.0f },
        { params::impactLevel, 0.6f }, { params::impactTone, 5000.0f }, { params::impactLength, 600.0f },
        { params::impactSpace, 0.6f } } },

    // For dense metal: a deep boom that ducks hard under breakdown hits and holds
    // steady under double kick instead of disappearing. Fine 0 + B-1 = low B.
    { "Metal Breakdown Boom",
      { { params::punch, 18.0f }, { params::punchTime, 22.0f }, { params::decay, 4000.0f }, { params::oneShot, 0.0f },
        { params::release, 120.0f }, { params::click, 0.3f }, { params::lowCut, 20.0f }, { params::crossover, 80.0f },
        { params::harmonics, 0.2f }, { params::autoHarmonics, 0.3f }, { params::drive, 0.0f }, { params::ceiling, -7.0f },
        { params::duckDepth, 6.0f }, { params::duckRelease, 100.0f }, { params::rollHold, 1.0f },
        { params::rollWindow, 150.0f }, { params::rollDepth, 2.0f },
        { params::impactLevel, 0.45f }, { params::impactTone, 3500.0f }, { params::impactLength, 350.0f },
        { params::impactSpace, 0.45f } } },

    // --- Sub layers --------------------------------------------------------

    // A clean, steady sub under open chords and big sections. Follow the bass
    // root notes; note length sets how long it sounds. Ducks lightly under
    // the kick and holds steady through double kick.
    { "Sub Layer (Sustained)",
      { { params::punch, 0.0f }, { params::decay, 12000.0f }, { params::oneShot, 0.0f }, { params::release, 80.0f },
        { params::glide, 30.0f }, { params::click, 0.0f }, { params::velocity, 0.2f }, { params::lowCut, 20.0f },
        { params::crossover, 80.0f }, { params::harmonics, 0.1f }, { params::autoHarmonics, 0.2f },
        { params::drive, -6.0f }, { params::ceiling, -8.0f }, { params::duckDepth, 4.0f },
        { params::duckRelease, 90.0f }, { params::rollHold, 1.0f }, { params::rollDepth, 2.0f } } },

    // Tight sub that doubles palm-muted chugs: short attack, short release,
    // a touch of punch so each chug lands. Copy the chug rhythm on the root.
    // Low Cut sits at its minimum: a low cut close to the note rings after
    // each chug and smears fast patterns together.
    { "Sub Layer (Chugs)",
      { { params::punch, 2.0f }, { params::punchTime, 15.0f }, { params::decay, 600.0f }, { params::oneShot, 0.0f },
        { params::release, 15.0f }, { params::click, 0.1f }, { params::velocity, 0.4f }, { params::lowCut, 15.0f },
        { params::crossover, 80.0f }, { params::harmonics, 0.15f }, { params::autoHarmonics, 0.3f },
        { params::drive, 0.0f }, { params::ceiling, -8.0f }, { params::duckDepth, 3.0f },
        { params::duckRelease, 60.0f }, { params::rollHold, 1.0f }, { params::rollDepth, 1.5f } } },

    // --- Drops -------------------------------------------------------------

    // The classic metal sub drop: played ON the downbeat, hits, then falls two
    // octaves over the bar (fast at first, then settling), in key.
    { "Classic Drop (1 bar)",
      { { params::mode, 1.0f }, { params::dropAmount, 24.0f }, { params::dropLength, 4.0f }, { params::dropCurve, -0.3f },
        { params::decay, 1500.0f }, { params::click, 0.25f }, { params::harmonics, 0.4f },
        { params::autoHarmonics, 0.8f }, { params::drive, 2.0f },
        { params::impactLevel, 0.45f }, { params::impactTone, 4500.0f }, { params::impactLength, 700.0f },
        { params::impactSpace, 0.6f } } },

    { "Long Drop (2 bars)",
      { { params::mode, 1.0f }, { params::dropAmount, 36.0f }, { params::dropLength, 5.0f }, { params::dropCurve, 0.5f },
        { params::decay, 2500.0f }, { params::click, 0.0f }, { params::harmonics, 0.45f },
        { params::autoHarmonics, 0.9f }, { params::drive, 2.0f } } },

    // A quick one-octave fall over a single beat, for fills.
    { "Quick Dive (1 beat)",
      { { params::mode, 1.0f }, { params::dropAmount, 12.0f }, { params::dropLength, 2.0f }, { params::dropCurve, -0.4f },
        { params::decay, 600.0f }, { params::click, 0.2f }, { params::drive, 4.0f } } },

    // Transition hit: a hard attack that sinks fast, then rumbles out.
    { "Impact Drop (2 beats)",
      { { params::mode, 1.0f }, { params::dropAmount, 24.0f }, { params::dropLength, 3.0f }, { params::dropCurve, -0.6f },
        { params::decay, 2000.0f }, { params::click, 0.5f }, { params::harmonics, 0.5f }, { params::drive, 8.0f },
        { params::impactLevel, 0.5f }, { params::impactTone, 6000.0f }, { params::impactLength, 800.0f },
        { params::impactSpace, 0.7f } } },
};

} // namespace subforge::presets
