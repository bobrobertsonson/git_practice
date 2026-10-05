#include "MatchGlue.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>

#include <nlohmann/json.hpp>

#include "PluginProcessor.h"

namespace sawblade::plugin {
namespace fs = std::filesystem;

std::optional<TakeInfo> selectedTake(SawbladeProcessor& p) {
  const std::string name = p.matchSettings().selectedTake();
  if (name.empty()) return std::nullopt;
  for (auto& t : p.recorder().listTakes())
    if (t.name == name) return std::move(t);
  return std::nullopt;
}

namespace {
using nlohmann::json;
std::string seconds(double s) {
  char b[32];
  std::snprintf(b, sizeof b, "%.2f s", s);
  return b;
}
}  // namespace

MatchPlan planMatch(SawbladeProcessor& p) {
  MatchPlan plan;
  const std::string folder = p.playAlong().settings().folder;
  if (folder.empty()) {
    plan.message = "Load a song in PLAY ALONG first: its stems folder is the reference.";
    return plan;
  }
  plan.reference = chooseReferenceFile(folder);
  if (!plan.reference.found) {
    plan.message = "No audio files found in " + folder + ".";
    return plan;
  }
  plan.take = selectedTake(p);
  if (!plan.take) {
    plan.message = "Record a take and choose it with USE FOR MATCH.";
    return plan;
  }
  plan.request.di = plan.take->wav;
  plan.request.ref = plan.reference.file;
  plan.request.referenceLabel = fs::path(folder).filename().string() + " (" + plan.reference.label + ")";
  plan.request.diLabel = plan.take->name;
  const bool sameSong = plan.take->songFolder.empty() || fs::path(plan.take->songFolder) == fs::path(folder);
  if (const auto off = plan.take->offsetMs(); off && sameSong) {
    plan.request.offsetMs = *off;
    plan.offsetNote = "Starts " + seconds(*off / 1000.0) + " into the song (from the take).";
  } else if (off && !sameSong) {
    plan.offsetNote = "Recorded against another song: position ignored, the matcher will search for it.";
  } else {
    plan.offsetNote = "Position in the song unknown (no backing was running): the matcher will search for it.";
  }
  plan.ok = true;
  return plan;
}

std::string exportBlockedReason(const Preset& p) {
  auto missing = [](const Capture& c) { return c.resolvedPath.empty(); };
  for (const PathPreset* path : {&p.a, &p.b})
    for (const auto& b : path->blocks)
      if (const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get()))
        if (missing(nam->model)) return "A NAM block in the preset has no model file, so it cannot be exported. Load a preset whose captures are on disk.";
  if (p.cab.enabled) {
    const bool bad = p.cab.mode == CabMode::Shared ? missing(p.cab.ir) : (missing(p.cab.irA) || missing(p.cab.irB));
    if (bad) return "The cab IR in the preset has no file, so it cannot be exported.";
  }
  return {};
}

ExportSource prepareExportSource(SawbladeProcessor& p) {
  ExportSource s;
  if (const auto f = p.audition().currentCandidateFile(); f && fs::exists(*f)) {
    s.ok = true;
    s.file = *f;
    s.description = "Matched preset: " + f->filename().string();
    return s;
  }
  if (const std::string why = exportBlockedReason(p.currentPreset()); !why.empty()) {
    s.message = why;
    return s;
  }
  const fs::path dir = p.jobs().jobsDir() / "inputs";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
  const fs::path file = dir / (std::string(buf) + ".preset.json");
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << presetToStateJson(p.currentPreset());
  out.close();
  if (!out) {
    s.message = "Could not write the preset to " + file.string();
    return s;
  }
  s.ok = true;
  s.file = file;
  s.description = "Current preset: " + p.status().presetName;
  return s;
}

// ---- two-pass MATCH ----------------------------------------------------------------------------------------------
namespace {
bool endsWith(const std::string& s, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string captureKey(const json& c) {
  if (auto src = c.find("source"); src != c.end() && src->is_object() && src->contains("id")) {
    auto str = [&](const char* k) {
      auto it = src->find(k);
      return it != src->end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    auto id = src->at("id");
    return "src:" + str("provider") + "/" + (id.is_string() ? id.get<std::string>() : id.dump()) + "/" + str("modelId");
  }
  auto f = c.find("file");
  return f != c.end() && f->is_string() ? "file:" + fs::path(f->get<std::string>()).filename().string() : std::string("none");
}

bool sameValue(const json& a, const json& b, const std::string& key);

bool sameObject(const json& a, const json& b) {
  if (a.contains("file") || b.contains("file")) {  // a capture (NAM model or IR): compared by identity, not by path
    if (!(a.contains("file") && b.contains("file"))) return false;
    // A source id on both sides decides; if only one side has it, fall back to the file names.
    const bool sa = a.contains("source") && a["source"].is_object() && a["source"].contains("id");
    const bool sb = b.contains("source") && b["source"].is_object() && b["source"].contains("id");
    if (sa && sb) return captureKey(a) == captureKey(b);
    return fs::path(a["file"].get<std::string>()).filename() == fs::path(b["file"].get<std::string>()).filename();
  }
  auto ignored = [](const std::string& k) { return k == "name" || k == "notes" || k == "id" || k == "playAlong"; };
  std::size_t na = 0, nb = 0;
  for (auto it = a.begin(); it != a.end(); ++it)
    if (!ignored(it.key())) ++na;
  for (auto it = b.begin(); it != b.end(); ++it)
    if (!ignored(it.key())) ++nb;
  if (na != nb) return false;
  for (auto it = a.begin(); it != a.end(); ++it) {
    if (ignored(it.key())) continue;
    auto o = b.find(it.key());
    if (o == b.end() || !sameValue(it.value(), *o, it.key())) return false;
  }
  return true;
}

bool sameValue(const json& a, const json& b, const std::string& key) {
  if (a.is_number() && b.is_number()) {
    const double x = a.get<double>(), y = b.get<double>();
    if (endsWith(key, "Db")) return std::abs(x - y) <= kSameChainDbTolerance;
    if (key == "blend") return std::abs(x - y) <= kSameChainBlendTolerance;
    return std::abs(x - y) <= kSameChainRelTolerance * std::max(std::abs(x), std::abs(y));
  }
  if (a.type() != b.type()) return false;
  if (a.is_object()) return sameObject(a, b);
  if (a.is_array()) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
      if (!sameValue(a[i], b[i], key)) return false;
    return true;
  }
  return a == b;
}

json readJsonFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  return json::parse(f, nullptr, /*allow_exceptions=*/false);
}
}  // namespace

bool sameChain(const json& a, const json& b) { return a.is_object() && b.is_object() && sameObject(a, b); }

bool sameChainFiles(const fs::path& a, const fs::path& b) {
  const json ja = readJsonFile(a), jb = readJsonFile(b);
  return sameChain(ja, jb);
}

void chooseTakeForMatch(SawbladeProcessor& p, const std::string& name) {
  const bool changed = p.matchSettings().selectedTake() != name;
  p.matchSettings().setSelectedTake(name);
  if (changed) p.jobs().cancelRefine();
}

bool applyRefinedBest(SawbladeProcessor& p, std::string* error) {
  const JobSnapshot refine = p.jobs().refineSnapshot();
  if (refine.state != JobState::Succeeded || refine.results.empty() || !refine.results.front().presetExists) {
    if (error) *error = "There is no refined result to apply yet.";
    return false;
  }
  if (!p.audition().audition(refine.results.front().preset, error)) return false;
  return p.audition().apply();
}

bool appliedQuickIsRefinedBest(SawbladeProcessor& p, const JobSnapshot& quick, const JobSnapshot& refine) {
  if (quick.pass != "quick" || refine.state != JobState::Succeeded || refine.results.empty()) return false;
  const auto applied = p.audition().appliedCandidateFile();
  if (!applied || applied->parent_path() != quick.dir) return false;  // not a quick candidate
  static thread_local std::string lastKey;
  static thread_local bool lastResult = false;
  const fs::path best = refine.results.front().preset;
  std::error_code ec;
  const std::string key = applied->string() + "|" + best.string() + "|" + std::to_string(fs::last_write_time(best, ec).time_since_epoch().count()) + "|" +
                          std::to_string(fs::last_write_time(*applied, ec).time_since_epoch().count());
  if (key == lastKey) return lastResult;
  lastKey = key;
  lastResult = sameChainFiles(*applied, best);
  return lastResult;
}

}  // namespace sawblade::plugin
