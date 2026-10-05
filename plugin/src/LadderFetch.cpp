#include "LadderFetch.h"

#include <algorithm>
#include <cmath>

#include "PresetMapping.h"
#include "sawblade/gain_ladder.h"

namespace sawblade::plugin {

std::vector<std::string> ladderArgs(const std::string& toneId, const std::string& size) {
  return {"ladder", toneId, "--size", size, "--json"};
}

LadderFetchResult parseLadderOutput(const std::string& output) {
  LadderFetchResult r;
  const auto lo = output.find('{');
  const auto hi = output.rfind('}');
  if (lo == std::string::npos || hi == std::string::npos || hi < lo) {
    r.error = "no JSON in the ladder output";
    return r;
  }
  const nlohmann::json j = nlohmann::json::parse(output.substr(lo, hi - lo + 1), nullptr, /*allow_exceptions=*/false);
  if (!j.is_object() || !j.contains("rungs") || !j.contains("tone_id") || !j["tone_id"].is_string()) {
    r.error = "not a ladder document";
    return r;
  }
  r.toneId = j["tone_id"].get<std::string>();
  const auto& rungs = j["rungs"];
  if (rungs.is_null()) {
    r.ok = true;
    return r;
  }
  if (!rungs.is_array()) {
    r.error = "rungs is not a list";
    return r;
  }
  std::vector<LadderRung> out;
  for (const auto& e : rungs) {
    if (!e.is_object() || !e.contains("model_id") || !e["model_id"].is_string() || !e.contains("gain") || !e["gain"].is_number()) {
      r.error = "malformed rung";
      return r;
    }
    LadderRung lr;
    lr.modelId = e["model_id"].get<std::string>();
    lr.gain = e["gain"].get<double>();
    if (e.contains("name") && e["name"].is_string()) lr.name = e["name"].get<std::string>();
    if (lr.modelId.empty() || !std::isfinite(lr.gain) || lr.gain < 0.0 || lr.gain > 100.0) {
      r.error = "malformed rung";
      return r;
    }
    for (const auto& o : out)
      if (o.modelId == lr.modelId || o.gain == lr.gain) {
        r.error = "duplicate rung";
        return r;
      }
    out.push_back(std::move(lr));
  }
  if (out.size() < 2 || out.size() > static_cast<std::size_t>(kMaxLadderRungs)) {
    r.error = "a ladder has 2 to 64 rungs";
    return r;
  }
  std::stable_sort(out.begin(), out.end(), [](const LadderRung& a, const LadderRung& b) { return a.gain < b.gain; });
  r.rungs = std::move(out);
  r.ok = true;
  return r;
}

namespace {
const NamBlockParams* ampNam(const PathPreset& p) {
  const int i = ampIndex(p);
  return i < 0 ? nullptr : dynamic_cast<const NamBlockParams*>(p.blocks[static_cast<std::size_t>(i)].params.get());
}
}  // namespace

std::vector<std::string> toneIdsNeedingLadder(const Preset& p) {
  std::vector<std::string> ids;
  for (const PathPreset* path : {&p.a, &p.b}) {
    const NamBlockParams* nam = ampNam(*path);
    if (!nam || !nam->model.ladder.empty() || !nam->model.source) continue;
    const CaptureSource& s = *nam->model.source;
    if (s.provider != "tone3000" || s.id.empty() || s.modelId.empty()) continue;
    if (std::find(ids.begin(), ids.end(), s.id) == ids.end()) ids.push_back(s.id);
  }
  return ids;
}

bool applyLadderToPreset(Preset& p, const std::string& toneId, const std::vector<LadderRung>& rungs) {
  if (rungs.size() < 2) return false;
  bool changed = false;
  for (PathPreset* path : {&p.a, &p.b}) {
    const NamBlockParams* nam = ampNam(*path);
    if (!nam || !nam->model.ladder.empty() || !nam->model.source) continue;
    // Copied out: `nam` (and the source inside it) is freed when the block's params are replaced below.
    const std::string provider = nam->model.source->provider, id = nam->model.source->id, modelId = nam->model.source->modelId;
    const int own = rungIndexOfModel(rungs, modelId);
    if (provider != "tone3000" || id != toneId || own < 0) continue;
    auto copy = std::make_shared<NamBlockParams>(*nam);
    copy->model.ladder = rungs;
    path->blocks[static_cast<std::size_t>(ampIndex(*path))].params = copy;
    if (path->ampControls.gainStep.empty() && path->ampControls.gain == kAmpKnobDefault) {
      const auto pos = ladderPositions(rungs);
      path->ampControls.gain = snapParam(pos[static_cast<std::size_t>(own)]);
    }
    changed = true;
  }
  return changed;
}

}  // namespace sawblade::plugin
