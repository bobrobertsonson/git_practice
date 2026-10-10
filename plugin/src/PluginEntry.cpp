// Plugin entry point (VST3 / AU / Standalone wrappers call this).
#include "PluginProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new sawblade::plugin::SawbladeProcessor(); }
