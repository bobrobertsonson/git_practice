#include "ExportGlue.h"

#include <algorithm>

#include "AppPaths.h"
#include "presets/PresetLibrary.h"
#include "PluginProcessor.h"
#include "sawblade/bus_comp.h"

namespace sawblade::plugin {
namespace fs = std::filesystem;

std::string LicenceLine::text() const {
  if (!known) return title + " - local file (no licence recorded)";
  return title + " - " + (creator.empty() ? "unknown creator" : creator) + " (" + (license.empty() ? "unknown licence" : license) + ")";
}

namespace {
bool isNc(const std::string& license) { return nonCommercialLicense(license); }

LicenceLine lineFor(const Capture& c) {
  LicenceLine l;
  const std::string stem = c.resolvedPath.empty() ? c.file : c.resolvedPath.stem().string();
  if (c.source) {
    l.known = true;
    l.title = !c.source->title.empty() ? c.source->title : stem.empty() ? c.source->id : stem;
    l.creator = c.source->creator;
    l.license = c.source->license;
    l.nonCommercial = isNc(c.source->license);
  } else {
    l.title = stem.empty() ? "capture" : stem;
  }
  return l;
}

std::string pathChain(const PathPreset& p, const std::vector<LicenceLine>& lines, std::size_t& next) {
  std::string out;
  for (const auto& b : p.blocks) {
    if (b.bypass) continue;
    std::string name;
    if (b.type == "nam") name = next < lines.size() ? lines[next++].title : "NAM";
    else if (b.type == "eq") name = "EQ";
    else name = b.type;
    if (!out.empty()) out += " > ";
    out += name;
  }
  return out;
}
}  // namespace

RigSummary summariseRig(const Preset& p) {
  RigSummary r;
  r.cabEnabled = p.cab.enabled;
  r.noCabExact = !p.cab.enabled || p.cab.mode != CabMode::PerPath;  // shared and irMix are one convolver
  r.compOn = p.busComp.enabled;
  r.compReleaseMs = p.busComp.releaseMs;
  r.compTrainable = p.busComp.releaseMs <= kBusCompMaxTrainableReleaseMs;

  // Captures in rig order: path A's NAM blocks, path B's, then the cab IR(s).
  std::vector<LicenceLine> a, b, cab;
  for (const auto& blk : p.a.blocks)
    if (const auto* nam = dynamic_cast<const NamBlockParams*>(blk.params.get())) a.push_back(lineFor(nam->model));
  for (const auto& blk : p.b.blocks)
    if (const auto* nam = dynamic_cast<const NamBlockParams*>(blk.params.get())) b.push_back(lineFor(nam->model));
  if (p.cab.enabled) {
    if (p.cab.mode == CabMode::Shared) cab.push_back(lineFor(p.cab.ir));
    else {
      cab.push_back(lineFor(p.cab.irA));
      cab.push_back(lineFor(p.cab.irB));
    }
  }
  std::size_t ia = 0, ib = 0;
  const std::string chainA = p.a.enabled ? pathChain(p.a, a, ia) : std::string();
  const std::string chainB = p.b.enabled ? pathChain(p.b, b, ib) : std::string();
  const std::string roleA = p.a.role == "body" ? "BODY" : p.a.role == "saw" ? "SAW " : "A   ";
  const std::string roleB = p.b.role == "saw" ? "SAW " : p.b.role == "body" ? "BODY" : "B   ";
  if (!chainA.empty()) r.chainLines.push_back(roleA + "  " + chainA);
  if (!chainB.empty()) r.chainLines.push_back(roleB + "  " + chainB);
  if (r.chainLines.empty()) r.chainLines.push_back("(no blocks)");

  if (!p.cab.enabled) r.cabLine = "CAB   off";
  else if (p.cab.mode == CabMode::Shared) r.cabLine = "CAB   shared IR: live blend, the no-cab export is exact";
  else if (p.cab.mode == CabMode::PerPath) r.cabLine = "CAB   one IR per path: studio blend, only the with-cab export is exact";
  else r.cabLine = "CAB   IR mix of two cabs (one convolver): live blend, the no-cab export is exact";

  for (const auto* v : {&a, &b, &cab})
    for (const auto& l : *v) {
      r.licences.push_back(l);
      r.nonCommercial = r.nonCommercial || l.nonCommercial;
    }
  return r;
}

std::string defaultExportMode(const RigSummary& r) { return r.noCabExact ? "nocab" : "withcab"; }

std::string effectiveExportMode(const ExportSettings& s, const RigSummary& r) {
  if (s.mode == "withcab") return "withcab";
  if (s.mode == "nocab" && r.noCabExact) return "nocab";
  return defaultExportMode(r);
}

bool exportDropsComp(const std::string& mode, const ExportSettings& s, const RigSummary& r) { return mode == "nocab" && r.compOn && s.compChoice != "keep"; }

fs::path defaultExportsRoot() { return appDataDir() / "exports"; }
fs::path exportsRootFor(const ExportSettings& s) { return s.outputFolder.empty() ? defaultExportsRoot() : fs::path(s.outputFolder); }

ExportPlan planExport(SawbladeProcessor& p, const ExportSettings& s) {
  ExportPlan plan;
  plan.rig = summariseRig(p.currentPreset());
  plan.mode = effectiveExportMode(s, plan.rig);
  plan.dropComp = exportDropsComp(plan.mode, s, plan.rig);
  plan.allowInexact = plan.mode == "nocab" && plan.rig.compOn && s.compChoice == "keep";
  const auto takes = p.recorder().listTakes();  // newest first
  if (!takes.empty()) plan.take = takes.front();
  plan.diBuiltin = s.diSource == "builtin" || !plan.take;
  plan.exportsRoot = exportsRootFor(s);
  const ExportSource src = prepareExportSource(p, plan.dropComp, /*write=*/false);
  if (src.ok) plan.sourceSha256 = src.sha256;
  else plan.blocked = src.message;
  return plan;
}

bool buildExportRequest(SawbladeProcessor& p, const ExportSettings& s, const ExportPlan& plan, ExportRequest& out, std::string* error) {
  const ExportSource src = prepareExportSource(p, plan.dropComp, /*write=*/true);
  if (!src.ok) {
    if (error) *error = src.message;
    return false;
  }
  out = ExportRequest{};
  out.preset = src.file;
  out.mode = plan.mode;
  out.arch = s.arch;
  out.size = s.size;
  if (!plan.diBuiltin && plan.take) out.di = plan.take->wav;
  else out.diBuiltin = true;
  out.exportsRoot = plan.exportsRoot;
  out.allowInexact = plan.allowInexact;
  return true;
}

ResumeOffer findResumableExport(SawbladeProcessor& p) {
  ResumeOffer o;
  const JobSnapshot snap = p.jobs().snapshot(JobKind::Export);
  if (snap.state != JobState::Cancelled || snap.outDir.empty() || snap.sourceSha256.empty()) return o;
  const CheckpointInfo ck = readCheckpoint(snap.outDir);  // exists and is not complete
  if (!ck.resumable) return o;
  // The same rig: what would be exported now (with the comp handled as that run handled it) hashes to the run's key.
  const RigSummary rig = summariseRig(p.currentPreset());
  const bool drop = snap.exportMode == "nocab" && rig.compOn && !snap.allowInexact;
  const ExportSource src = prepareExportSource(p, drop, /*write=*/false);
  if (!src.ok || src.sha256 != snap.sourceSha256) return o;
  o.available = true;
  o.dir = snap.outDir;
  o.epoch = ck.epoch;
  o.epochs = ck.epochs;
  o.mode = snap.exportMode;
  o.arch = snap.exportArch.empty() ? "a1" : snap.exportArch;
  o.size = snap.exportSize;
  return o;
}

bool buildResumeRequest(SawbladeProcessor& p, const ResumeOffer& offer, ExportRequest& out, std::string* error) {
  const JobSnapshot snap = p.jobs().snapshot(JobKind::Export);
  const RigSummary rig = summariseRig(p.currentPreset());
  const bool drop = offer.mode == "nocab" && rig.compOn && !snap.allowInexact;
  const ExportSource src = prepareExportSource(p, drop, /*write=*/true);
  if (!offer.available || !src.ok) {
    if (error) *error = src.ok ? "There is no run to resume." : src.message;
    return false;
  }
  out = ExportRequest{};
  out.preset = src.file;
  out.mode = offer.mode;
  out.arch = offer.arch.empty() ? "a1" : offer.arch;
  out.size = offer.size;
  out.exportsRoot = snap.exportsRoot;
  out.allowInexact = snap.allowInexact;
  out.resumeDir = offer.dir;
  if (!snap.di.empty() && !snap.diBuiltin) {
    // The same validation DI as the cancelled run, if that take still exists.
    for (const auto& t : p.recorder().listTakes())
      if (t.wav.filename().string() == snap.di) out.di = t.wav;
  }
  out.diBuiltin = !out.di.has_value();
  return true;
}

}  // namespace sawblade::plugin
