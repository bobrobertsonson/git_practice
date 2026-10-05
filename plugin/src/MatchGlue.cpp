#include "MatchGlue.h"

#include <cmath>
#include <ctime>
#include <fstream>

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

MatchPlan planMatch(SawbladeProcessor& p) {
  MatchPlan plan;
  const std::string folder = p.playAlong().activeStemsDir();
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
  plan.request.referenceLabel = activeSongName(p) + " (" + plan.reference.label + ")";
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

}  // namespace sawblade::plugin
