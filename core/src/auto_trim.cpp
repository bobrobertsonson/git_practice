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
  p.autoTrim.db = 0.0;
  p.autoTrim.hash.clear();
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
  // Hashed as the rig plays it: the active dynamics set sits in gate / busComp (resolveDynamics), and the labels that choose the set
  // (dynamicsMode, liveDynamics, origin) are not hashed, so a preset whose live and record sets are equal keeps one hash in either
  // mode and every stamp written before preset v4 stays fresh.
  json j = toJson(resolveDynamics(withoutTrim(p)));
  for (const char* k : {"name", "notes", "category", "version", "schema", "output", "dynamicsMode", "liveDynamics", "origin"})
    j.erase(k);  // output.gainDb: the user's offset
  normaliseCaptures(j);
  const std::string text = "sawblade.autotrim." + std::to_string(kAutoTrimVersion) + ".ref." + std::to_string(kReferenceDiVersion) +
                           ".target." + std::to_string(static_cast<int>(kAutoTrimTargetLufs)) + "\n" + j.dump();
  return sha256Hex(text.data(), text.size());
}

bool autoTrimFresh(const Preset& p) { return !p.autoTrim.hash.empty() && p.autoTrim.hash == autoTrimHash(p); }

std::optional<double> measureReferenceLufs(const Preset& p, CaptureCache* cache, bool applyTrim, const std::atomic<bool>* cancel,
                                           const ChainCalibration& cal) {
  AudioFile in;
  in.sampleRate = kReferenceDiRate;
  in.channels = 1;
  in.interleaved = referenceDi();
  RenderOptions o;
  o.cache = cache;
  o.outRate = OutRate::Input;
  o.applyAutoTrim = applyTrim;
  o.calibration = cal;
  Preset q = applyTrim ? p : withoutTrim(p);
  q.outputGainDb = 0.0;  // the OUTPUT knob is a persistent offset on top of the match: it is measured at 0 dB
  const RenderResult r = renderPreset(q, in, o);
  if (cancel != nullptr && cancel->load()) return std::nullopt;  // the caller is shutting down: skip the measurement
  return lufsOf(r.samples, r.sampleRate);
}

bool hasNonlinearBlock(const Preset& p) {
  for (const PathPreset* pp : {&p.a, &p.b}) {
    if (!pp->enabled) continue;
    for (const Block& b : pp->blocks)
      if (!b.bypass && (b.type == "nam" || b.type.rfind("pedal.", 0) == 0)) return true;
  }
  return false;
}

std::optional<AutoTrimResult> computeAutoTrim(const Preset& p, CaptureCache* cache, const std::atomic<bool>* cancel,
                                              const ChainCalibration& cal) {
  const auto l = measureReferenceLufs(p, cache, false, cancel, cal);
  if (!l) return std::nullopt;
  AutoTrimResult r;
  r.lufs = *l;
  r.trimDb = hasNonlinearBlock(p) ? std::min(kMaxPositiveTrimDb, std::max(-kMaxAutoTrimDb, kAutoTrimTargetLufs - *l)) : 0.0;
  r.hash = autoTrimHash(p);
  return r;
}

bool stampAutoTrim(Preset& p, CaptureCache* cache, const ChainCalibration& cal) {
  const auto r = computeAutoTrim(p, cache, nullptr, cal);
  if (!r) return false;
  p.autoTrim.db = r->trimDb;
  p.autoTrim.hash = r->hash;
  return true;
}

bool ensureAutoTrim(Preset& p, CaptureCache* cache, const ChainCalibration& cal) {
  if (autoTrimFresh(p)) return true;
  return stampAutoTrim(p, cache, cal);
}

std::optional<double> measurePathLufs(const Preset& p, int path, CaptureCache* cache, const std::atomic<bool>* cancel,
                                      const ChainCalibration& cal) {
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
  return measureReferenceLufs(q, cache, false, cancel, cal);
}

bool blockFeedsNam(const Preset& p, int path, int blockIndex, const ChainCalibration& cal, CaptureCache* cache) {
  if (!cal.enabled || (path != 0 && path != 1) || blockIndex < 0) return false;
  constexpr double kPlanRate = 48000.0;  // levelInfo() does not depend on the rate
  Chain chain(p, loadResources(p, kPlanRate, cache));
  const CalibrationPlan plan = chain.planCalibration(cal);
  const auto& blocks = plan.blocks[static_cast<std::size_t>(path)];
  const auto i = static_cast<std::size_t>(blockIndex);
  return i < blocks.size() && blocks[i].feedsNam;
}

std::optional<SlotMakeup> slotMakeup(const Preset& before, const Preset& after, int path, int blockIndex, const ChainCalibration& cal,
                                     CaptureCache* cache, const std::atomic<bool>* cancel) {
  SlotMakeup r;
  if (blockFeedsNam(after, path, blockIndex, cal, cache)) {
    r.skippedHop = true;
    return r;
  }
  const auto lb = measurePathLufs(before, path, cache, cancel, cal);
  if (cancel != nullptr && cancel->load()) return std::nullopt;
  const auto la = measurePathLufs(after, path, cache, cancel, cal);
  if (!lb || !la) return std::nullopt;
  r.makeupDb = std::min(kMaxSlotMakeupDb, std::max(-kMaxSlotMakeupDb, *lb - *la));
  return r;
}

std::optional<double> slotMakeupDb(const Preset& before, const Preset& after, int path, CaptureCache* cache, const std::atomic<bool>* cancel,
                                   const ChainCalibration& cal, int blockIndex) {
  const auto r = slotMakeup(before, after, path, blockIndex, cal, cache, cancel);
  if (!r) return std::nullopt;
  return r->makeupDb;
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
