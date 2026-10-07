#include "MatchGlue.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "PluginProcessor.h"
#include "PresetMapping.h"
#include "Sha256.h"

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

std::string activeSongName(SawbladeProcessor& p) {
  const auto s = p.playAlong().settings();
  if (!s.songFile.empty()) return fs::path(s.songFile).stem().string();
  return fs::path(s.folder).filename().string();
}

void toggleRecording(SawbladeProcessor& p) {
  auto& r = p.recorder();
  if (r.state() == TakeRecorder::State::Idle) r.start(p.playAlong().activeStemsDir());
  else r.stop();
}

std::string recordUnavailableReason(SawbladeProcessor& p) {
  return p.recorder().state() == TakeRecorder::State::Finalizing ? "The last take is still being saved." : std::string();
}

std::string recordStateText(SawbladeProcessor& p) {
  auto& r = p.recorder();
  switch (r.state()) {
    case TakeRecorder::State::Idle: return "READY";
    case TakeRecorder::State::Armed: return "ARMED";
    case TakeRecorder::State::Finalizing: return "SAVING";
    case TakeRecorder::State::Recording: break;
  }
  const double rate = r.sampleRate() > 0.0 ? r.sampleRate() : 48000.0;
  const int tenths = static_cast<int>(std::floor(static_cast<double>(r.recordedSamples()) / rate * 10.0 + 1e-9));
  char b[32];
  std::snprintf(b, sizeof b, "REC %02d:%02d.%d", tenths / 600, (tenths / 10) % 60, tenths % 10);
  return b;
}

std::string takeOriginText(const TakeInfo& t) {
  if (t.imported.present) return "IMPORTED";
  const auto off = t.offsetMs();
  if (!off) return "no song";
  char buf[48];
  std::snprintf(buf, sizeof buf, "@ %.1f s", *off / 1000.0);
  return buf;
}

MatchPlan planMatch(SawbladeProcessor& p) {
  MatchPlan plan;
  const std::string folder = p.playAlong().activeStemsDir();
  if (folder.empty()) {
    plan.message = "Load a song first: its guitar stem is the reference.";
    return plan;
  }
  plan.reference = chooseReferenceFile(folder);
  if (!plan.reference.found) {
    plan.message = "No audio files found in " + folder + ".";
    return plan;
  }
  plan.take = selectedTake(p);
  if (!plan.take) {
    plan.message = "Record or import a DI first.";
    return plan;
  }
  plan.request.di = plan.take->wav;
  plan.request.ref = plan.reference.file;
  plan.request.referenceLabel = activeSongName(p) + " (" + plan.reference.label + ")";
  plan.request.diLabel = plan.take->name;
  const bool sameSong = plan.take->songFolder.empty() || fs::path(plan.take->songFolder) == fs::path(folder);
  if (plan.take->imported.present) {
    // An imported file: its position in the song is used only when it is the same performance as the song (a matched pair,
    // `--matched mono`); otherwise it is a different performance and the matcher matches tone, not timing.
    const auto& im = plan.take->imported;
    if (im.samePerformance) {
      plan.request.matched = true;
      if (im.offsetMs) {
        plan.request.offsetMs = *im.offsetMs;
        plan.offsetNote = "Imported, same performance as the song: starts " + seconds(*im.offsetMs / 1000.0) + " into it.";
      } else {
        plan.offsetNote = "Imported, same performance as the song: the matcher will search the whole song for where it starts.";
      }
    } else {
      plan.offsetNote = "Imported file: a different performance from the song, so its timing is not used.";
    }
  } else if (const auto off = plan.take->offsetMs(); off && sameSong) {
    plan.request.offsetMs = *off;
    plan.offsetNote = "Recorded take: matched by tone (its song position, " + seconds(*off / 1000.0) + " in, is kept but not used).";
  } else if (off && !sameSong) {
    plan.offsetNote = "Recorded against another song: position ignored, the matcher will search for it.";
  } else {
    plan.offsetNote = "Position in the song unknown (no backing was running): the matcher will search for it.";
  }
  plan.ok = true;
  return plan;
}

std::string exportBlockedReason(const Preset& p) {
  auto missing = [](const Capture& c) { return c.resolvedPath.empty() || isNoCapture(c); };
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

ExportSource prepareExportSource(SawbladeProcessor& p, bool dropComp, bool write) {
  ExportSource s;
  Preset current;
  // NAM export follows the ACTIVE dynamics set (live or record): the exporter is handed a preset whose gate / busComp are that set.
  const bool compOn = (current = resolveDynamics(p.currentPreset())).busComp.enabled;
  // The auditioned / applied candidate is exported as it is, unless the comp has to go (a no-cab export that drops it).
  if (const auto f = p.audition().currentCandidateFile(); f && fs::exists(*f) && !(dropComp && compOn)) {
    std::ifstream in(*f, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    s.ok = true;
    s.file = *f;
    s.sha256 = sha256Hex(bytes);
    s.description = "Matched preset: " + f->filename().string();
    return s;
  }
  if (const std::string why = exportBlockedReason(current); !why.empty()) {
    s.message = why;
    return s;
  }
  if (dropComp) current.busComp.enabled = false;
  // NAM export always trains the UN-trimmed chain (docs/PRESET_SCHEMA.md "Level matching"): the auto trim (output.autoTrim.db, the
  // plugin's LEVEL MATCH) is a plain gain on the way out and never part of the model, so neither the exported preset nor its key
  // carries it. A slot's capture-swap make-up is part of the sound and stays.
  current.autoTrim.db = 0.0;
  current.autoTrim.hash.clear();
  // The file is named by the hash of its bytes: the same rig always gives the same file (and the same "same rig" key
  // for RESUME); a changed rig gives another one.
  const std::string text = presetToStateJson(current);
  s.sha256 = sha256Hex(text);
  const fs::path dir = p.jobs().jobsDir() / "inputs";
  const fs::path file = dir / (s.sha256.substr(0, 16) + ".preset.json");
  if (!write) {  // only the key (and where the file would go)
    s.ok = true;
    s.file = file;
    s.description = "Current preset: " + p.status().presetName;
    return s;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  bool same = false;
  {
    std::ifstream in(file, std::ios::binary);
    if (in) same = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) == text;
  }
  if (!same) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) {
      s.message = "Could not write the preset to " + file.string();
      s.sha256.clear();
      return s;
    }
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

// Every accessor below is guarded by a type check: sameChain runs on the UI timer on files the matcher wrote, and a
// malformed one must read as "different", never throw.
std::string strOf(const json& o, const char* k) {
  auto it = o.find(k);
  return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool hasSourceId(const json& c) {
  auto src = c.find("source");
  return src != c.end() && src->is_object() && src->contains("id");
}

std::string sourceKey(const json& c) {
  const json& src = c.at("source");
  const json& id = src.at("id");
  return "src:" + strOf(src, "provider") + "/" + (id.is_string() ? id.get<std::string>() : id.dump()) + "/" + strOf(src, "modelId");
}

bool sameValue(const json& a, const json& b, const std::string& key);

bool sameObject(const json& a, const json& b) {
  if (a.contains("file") || b.contains("file")) {  // a capture (NAM model or IR): compared by identity, not by path
    auto fa = a.find("file"), fb = b.find("file");
    if (fa == a.end() || fb == b.end() || !fa->is_string() || !fb->is_string()) return false;
    // A source id on both sides decides; if only one side has it, fall back to the file names.
    if (hasSourceId(a) && hasSourceId(b)) return sourceKey(a) == sourceKey(b);
    return fs::path(fa->get<std::string>()).filename() == fs::path(fb->get<std::string>()).filename();
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

bool sameChain(const json& a, const json& b) {
  try {
    return a.is_object() && b.is_object() && sameObject(a, b);
  } catch (...) {
    return false;  // belt and braces: malformed input is "different"
  }
}

bool sameChainFiles(const fs::path& a, const fs::path& b) {
  const json ja = readJsonFile(a), jb = readJsonFile(b);
  return sameChain(ja, jb);
}

void chooseTakeForMatch(SawbladeProcessor& p, const std::string& name) {
  const bool changed = p.matchSettings().selectedTake() != name;
  p.matchSettings().setSelectedTake(name);
  if (changed) p.jobs().cancelRefine();  // also a refinement that is about to start (refinePending)
}

bool renameTakeForMatch(SawbladeProcessor& p, const std::string& oldName, const std::string& newName, std::string* error) {
  const bool selected = p.matchSettings().selectedTake() == oldName;
  // The refinement works on the old file: it is cancelled before the file moves.
  if (selected) p.jobs().cancelRefine();
  if (!p.recorder().renameTake(oldName, newName, error)) return false;
  if (selected) p.matchSettings().setSelectedTake(newName);
  return true;
}

bool deleteTakeForMatch(SawbladeProcessor& p, const std::string& name) {
  const bool selected = p.matchSettings().selectedTake() == name;
  if (selected) p.jobs().cancelRefine();
  if (!p.recorder().removeTake(name)) return false;
  if (selected) p.matchSettings().setSelectedTake({});
  return true;
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
  const std::string key = applied->string() + "|" + best.string() + "|" + std::to_string(static_cast<long long>(fs::last_write_time(best, ec).time_since_epoch().count())) + "|" +
                          std::to_string(static_cast<long long>(fs::last_write_time(*applied, ec).time_since_epoch().count()));
  if (key == lastKey) return lastResult;
  lastKey = key;
  lastResult = sameChainFiles(*applied, best);
  return lastResult;
}

}  // namespace sawblade::plugin
