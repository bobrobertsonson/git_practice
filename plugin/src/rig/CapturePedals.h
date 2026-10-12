#pragma once

// v0.4 Task B: pedal captures from the local capture cache (JUCE-free). `sawblade-t3k fetch` writes <cache>/<tone>/meta.json (the tone's
// raw record under "tone", one entry per fetched model under "models") and the model files beside it; the pedalboard's CAPTURES tab lists
// the tones whose gear is "pedal" and the setting selector of a capture tile lists the models of its tone, from there. Nothing is fetched.

#include <string>
#include <vector>

namespace sawblade::plugin::rig {

// The ONE rule for a name read from a cache's meta.json (a tone id, a model id, a model file) before it becomes a path under the cache:
// plain ids are non-empty with no '/', '\\' or '.'; a model file is a plain stem + extension that is its own filename (no separator, no "..").
bool plainCacheId(const std::string& s);
bool plainCacheFile(const std::string& file);

struct CachedModel {
  std::string modelId;
  std::string name;  // the model's own name ("Gain 6", "Standard"); its id when it has none
};

struct CachedPedal {
  std::string toneId, title, creator, license, url;
  std::vector<CachedModel> models;  // those with a file in the cache, in setting order (see orderSettings)
};

// The cached tones with gear "pedal" and at least one .nam model file, by title (case-insensitive), then tone id.
std::vector<CachedPedal> cachedPedalCaptures();
// One cached tone's models (any gear); empty when the tone is not cached.
std::vector<CachedModel> cachedModelsOf(const std::string& toneId);

// The settings of a pedal in the order a player expects: when every name has a number ("Gain 2", "Drive 7", "3") they are ordered by
// it (the v0.2 gain-ladder order); otherwise by model id (numerically when they are numbers), i.e. the plain list.
std::vector<CachedModel> orderSettings(std::vector<CachedModel> models);

}  // namespace sawblade::plugin::rig
