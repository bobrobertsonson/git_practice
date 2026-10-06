#include "sawblade/auto_trim.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "sawblade/loudness.h"
#include "sawblade/reference_di.h"
#include "sawblade/render.h"
#include "sawblade/sha256.h"

namespace sawblade {
namespace {

using nlohmann::json;

// A capture object (anything with a string `file`) reduced to its identity.
void normaliseCaptures(json& j) {
  if (j.is_array()) {
    for (auto& v : j) normaliseCaptures(v);
    return;
  }
  if (!j.is_object()) return;
  if (j.contains("file") && j["file"].is_string()) {
    const auto src = j.find("source");
    if (src != j.end() && src->is_object() && src->contains("provider") && src->contains("id")) {
      json id = {{"provider", (*src)["provider"]}, {"id", (*src)["id"]}};
      if (src->contains("modelId")) id["modelId"] = (*src)["modelId"];
      j.erase("file");
      j["source"] = std::move(id);
    } else {
      j["file"] = std::filesystem::path(j["file"].get<std::string>()).filename().string();
      j.erase("source");
    }
  }
  for (auto& [k, v] : j.items()) normaliseCaptures(v);
}

Preset withoutTrim(Preset p) {
  p.autoTrimDb = 0.0;
  p.autoTrimHash.clear();
  return p;
}

std::optional<double> lufsOf(const std::vector<float>& mono, double rate) {
  const std::vector<float> silent(mono.size(), 0.0f);  // mono: the right channel is silent, as chain.cpp's level match does
  return integratedLoudnessLufs(mono.data(), silent.data(), static_cast<std::int64_t>(mono.size()), rate);
}

}  // namespace

std::vector<std::string> missingCaptures(const Preset& p) {
  std::vector<std::string> out;
  std::error_code ec;
  const auto check = [&](const Capture& c, const std::string& where) {
    if (c.file.empty() || c.file == "(none)") return;
    if (!std::filesystem::exists(locateCapture(c), ec)) out.push_back(where + ": " + c.file);
  };
  const PathPreset* pp[2] = {&p.a, &p.b};
  for (std::size_t k = 0; k < 2; ++k) {
    if (!pp[k]->enabled) continue;
    for (std::size_t i = 0; i < pp[k]->blocks.size(); ++i)
      if (const auto* nam = dynamic_cast<const NamBlockParams*>(pp[k]->blocks[i].params.get()))
        check(nam->model, std::string(k == 0 ? "paths.a" : "paths.b") + ".blocks[" + std::to_string(i) + "].model");
  }
  if (p.cab.enabled) {
    if (p.cab.mode == CabMode::Shared) {
      check(p.cab.ir, "cab.ir");
    } else {
      check(p.cab.irA, "cab.irA");
      check(p.cab.irB, "cab.irB");
    }
  }
  return out;
}

std::string autoTrimHash(const Preset& p) {
  json j = toJson(withoutTrim(p));
  for (const char* k : {"name", "notes", "category", "version", "schema"}) j.erase(k);
  normaliseCaptures(j);
  const std::string text = "sawblade.autotrim." + std::to_string(kAutoTrimVersion) + ".ref." + std::to_string(kReferenceDiVersion) +
                           ".target." + std::to_string(static_cast<int>(kAutoTrimTargetLufs)) + "\n" + j.dump();
  return sha256Hex(text.data(), text.size());
}

bool autoTrimFresh(const Preset& p) { return !p.autoTrimHash.empty() && p.autoTrimHash == autoTrimHash(p); }

std::optional<double> measureReferenceLufs(const Preset& p, CaptureCache* cache, bool applyTrim) {
  AudioFile in;
  in.sampleRate = kReferenceDiRate;
  in.channels = 1;
  in.interleaved = referenceDi();
  RenderOptions o;
  o.cache = cache;
  o.outRate = OutRate::Input;
  o.applyAutoTrim = applyTrim;
  const RenderResult r = renderPreset(applyTrim ? p : withoutTrim(p), in, o);
  return lufsOf(r.samples, r.sampleRate);
}

std::optional<AutoTrimResult> computeAutoTrim(const Preset& p, CaptureCache* cache) {
  const auto l = measureReferenceLufs(p, cache);
  if (!l) return std::nullopt;
  AutoTrimResult r;
  r.lufs = *l;
  r.trimDb = std::min(kMaxAutoTrimDb, std::max(-kMaxAutoTrimDb, kAutoTrimTargetLufs - *l));
  r.hash = autoTrimHash(p);
  return r;
}

bool stampAutoTrim(Preset& p, CaptureCache* cache) {
  const auto r = computeAutoTrim(p, cache);
  if (!r) return false;
  p.autoTrimDb = r->trimDb;
  p.autoTrimHash = r->hash;
  return true;
}

bool ensureAutoTrim(Preset& p, CaptureCache* cache) {
  if (autoTrimFresh(p)) return true;
  return stampAutoTrim(p, cache);
}

std::optional<double> measurePathLufs(const Preset& p, int path, CaptureCache* cache) {
  Preset q = withoutTrim(p);
  PathPreset& mine = path == 0 ? q.a : q.b;
  PathPreset& other = path == 0 ? q.b : q.a;
  if (!mine.enabled) return std::nullopt;
  other.enabled = false;
  q.blend = path == 0 ? 0.0 : 1.0;
  q.blendLaw = BlendLaw::Linear;
  q.levelMatch = {};
  q.align.mode = AlignMode::Off;
  q.align.delaySamplesB = 0;
  q.align.invertB = false;
  return measureReferenceLufs(q, cache);
}

std::optional<double> slotMakeupDb(const Preset& before, const Preset& after, int path, CaptureCache* cache) {
  const auto lb = measurePathLufs(before, path, cache);
  const auto la = measurePathLufs(after, path, cache);
  if (!lb || !la) return std::nullopt;
  return std::min(kMaxSlotMakeupDb, std::max(-kMaxSlotMakeupDb, *lb - *la));
}

Preset withSlotMakeup(const Preset& p, int path, int blockIndex, double makeupDb) {
  Preset out = p;
  PathPreset& pp = path == 0 ? out.a : out.b;
  if (blockIndex < 0 || static_cast<std::size_t>(blockIndex) >= pp.blocks.size()) return out;
  Block& b = pp.blocks[static_cast<std::size_t>(blockIndex)];
  const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get());
  if (nam == nullptr) return out;
  auto np = std::make_shared<NamBlockParams>(*nam);
  np->makeupDb = std::min(kMaxSlotMakeupDb, std::max(-kMaxSlotMakeupDb, makeupDb));
  b.params = std::move(np);
  return out;
}

double slotMakeupOf(const Preset& p, int path, int blockIndex) {
  const PathPreset& pp = path == 0 ? p.a : p.b;
  if (blockIndex < 0 || static_cast<std::size_t>(blockIndex) >= pp.blocks.size()) return 0.0;
  const auto* nam = dynamic_cast<const NamBlockParams*>(pp.blocks[static_cast<std::size_t>(blockIndex)].params.get());
  return nam ? nam->makeupDb : 0.0;
}

}  // namespace sawblade
