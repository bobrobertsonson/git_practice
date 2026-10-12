#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace subforge::params
{

// Parameter IDs. Changing one breaks saved sessions, so treat them as permanent.
inline constexpr auto mode = "mode";
inline constexpr auto tune = "tune";
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

inline constexpr auto lowCut = "lowCut";
inline constexpr auto crossover = "crossover";
inline constexpr auto harmonics = "harmonics";
inline constexpr auto autoHarmonics = "autoHarmonics";
inline constexpr auto drive = "drive";
inline constexpr auto ceiling = "ceiling";

inline constexpr auto duckDepth = "duckDepth";
inline constexpr auto duckThreshold = "duckThreshold";
inline constexpr auto duckRelease = "duckRelease";
inline constexpr auto phoneCheck = "phoneCheck";
inline constexpr auto output = "output";

inline const juce::StringArray modeChoices { "Boom", "Drop" };
inline const juce::StringArray dropLengthChoices { "1/4 beat", "1/2 beat", "1 beat", "2 beats", "4 beats", "8 beats", "16 beats" };
inline constexpr double dropLengthBeats[] = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };

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
    add (std::make_unique<AudioParameterFloat> (ParameterID { punch, 1 }, "Punch", NormalisableRange<float> (0.0f, 24.0f), 7.0f, unit ("st", 1)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { punchTime, 1 }, "Punch Time", range (5.0f, 150.0f, 40.0f), 40.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { decay, 1 }, "Decay", range (50.0f, 4000.0f, 700.0f), 800.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterBool> (ParameterID { oneShot, 1 }, "One Shot", true));
    add (std::make_unique<AudioParameterFloat> (ParameterID { release, 1 }, "Release", range (5.0f, 2000.0f, 150.0f), 60.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { glide, 1 }, "Glide", range (0.0f, 500.0f, 80.0f), 0.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { click, 1 }, "Click", NormalisableRange<float> (0.0f, 1.0f), 0.2f, percent));
    add (std::make_unique<AudioParameterFloat> (ParameterID { velocity, 1 }, "Velocity", NormalisableRange<float> (0.0f, 1.0f), 0.5f, percent));

    add (std::make_unique<AudioParameterInt> (ParameterID { dropAmount, 1 }, "Drop Amount", 1, 48, 24,
                                              AudioParameterIntAttributes().withLabel ("st")));
    add (std::make_unique<AudioParameterChoice> (ParameterID { dropLength, 1 }, "Drop Length", dropLengthChoices, 4));
    add (std::make_unique<AudioParameterFloat> (ParameterID { dropCurve, 1 }, "Drop Curve", NormalisableRange<float> (-1.0f, 1.0f), 0.3f, unit ("", 2)));

    add (std::make_unique<AudioParameterFloat> (ParameterID { lowCut, 1 }, "Low Cut", range (15.0f, 60.0f, 30.0f), 25.0f, unit ("Hz", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { crossover, 1 }, "Crossover", range (60.0f, 200.0f, 110.0f), 100.0f, unit ("Hz", 0)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { harmonics, 1 }, "Harmonics", NormalisableRange<float> (0.0f, 1.0f), 0.35f, percent));
    add (std::make_unique<AudioParameterFloat> (ParameterID { autoHarmonics, 1 }, "Auto Harmonics", NormalisableRange<float> (0.0f, 1.0f), 0.6f, percent));
    add (std::make_unique<AudioParameterFloat> (ParameterID { drive, 1 }, "Drive", NormalisableRange<float> (0.0f, 18.0f), 3.0f, unit ("dB", 1)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { ceiling, 1 }, "Ceiling", NormalisableRange<float> (-18.0f, 0.0f), -6.0f, unit ("dB", 1)));

    add (std::make_unique<AudioParameterFloat> (ParameterID { duckDepth, 1 }, "Duck Depth", NormalisableRange<float> (0.0f, 24.0f), 8.0f, unit ("dB", 1)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { duckThreshold, 1 }, "Duck Threshold", NormalisableRange<float> (-48.0f, 0.0f), -30.0f, unit ("dB", 1)));
    add (std::make_unique<AudioParameterFloat> (ParameterID { duckRelease, 1 }, "Duck Release", range (20.0f, 500.0f, 120.0f), 120.0f, unit ("ms", 0)));
    add (std::make_unique<AudioParameterBool> (ParameterID { phoneCheck, 1 }, "Phone Check", false));
    add (std::make_unique<AudioParameterFloat> (ParameterID { output, 1 }, "Output", NormalisableRange<float> (-24.0f, 6.0f), 0.0f, unit ("dB", 1)));

    return layout;
}

} // namespace subforge::params
