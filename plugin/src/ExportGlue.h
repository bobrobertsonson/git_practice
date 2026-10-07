#pragma once

// Message-thread glue of EXPORT NAM (docs/specs/phase12_export_in_plugin.md): what the export panel shows about the rig
// (chain, cab mode, comp, licences), which mode is exact, the ExportRequest built from the panel's settings and the
// processor, and the lookup of a cancelled run that can be resumed. Kept out of the UI so it is testable.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ExportSettings.h"
#include "JobRunner.h"
#include "MatchGlue.h"
#include "TakeRecorder.h"
#include "sawblade/preset.h"

namespace sawblade::plugin {

class SawbladeProcessor;

// One capture's credit line: "title - creator (licence)" (the way the .nam metadata credits it).
struct LicenceLine {
  std::string title, creator, license;
  bool nonCommercial = false;   // cc-by-nc*
  bool known = false;           // the preset recorded a TONE3000 source for it
  std::string text() const;
};

struct RigSummary {
  std::vector<std::string> chainLines;   // "SAW  TS808 > JCM800 2203" (a path that is off or empty is left out)
  std::string cabLine;                    // "CAB  shared IR (live blend)" ...
  bool cabEnabled = true;
  bool noCabExact = true;                 // shared / irMix cab (one convolver) or no cab: the no-cab export is exact
  bool compOn = false;
  std::string dynamics = "record";        // "live" | "record": which dynamics set the export follows (activeDynamics)
  double compReleaseMs = 0.0;
  bool compTrainable = true;              // release <= kBusCompMaxTrainableReleaseMs
  std::vector<LicenceLine> licences;
  bool nonCommercial = false;
};
RigSummary summariseRig(const Preset& p);

// "nocab" for a rig whose no-cab export is exact (shared cab), else "withcab" (per-path cabs).
std::string defaultExportMode(const RigSummary& r);
// The saved mode if it is still exact for this rig, else the default.
std::string effectiveExportMode(const ExportSettings& s, const RigSummary& r);
// Does the exported preset have its bus comp switched off? (no-cab export, comp on, DROP COMP)
bool exportDropsComp(const std::string& mode, const ExportSettings& s, const RigSummary& r);
std::filesystem::path defaultExportsRoot();  // <app data>/exports
std::filesystem::path exportsRootFor(const ExportSettings& s);

struct ExportPlan {
  RigSummary rig;
  std::string mode;                       // effective mode
  std::optional<TakeInfo> take;           // the newest take, if any
  bool diBuiltin = false;                 // the built-in signal (chosen, or no take exists)
  bool dropComp = false, allowInexact = false;
  std::filesystem::path exportsRoot;
  std::string sourceSha256;               // of the preset that would be exported ("" = it cannot be exported)
  std::string blocked;                    // why it cannot be exported ("" = fine)
};
// Everything the panel needs, without writing any file.
ExportPlan planExport(SawbladeProcessor& p, const ExportSettings& s);
// Writes the resolved preset and builds the request. False (and *error) if the rig cannot be exported.
bool buildExportRequest(SawbladeProcessor& p, const ExportSettings& s, const ExportPlan& plan, ExportRequest& out, std::string* error);

// A cancelled run of THIS rig whose checkpoint is intact (contract item 8).
struct ResumeOffer {
  bool available = false;
  std::filesystem::path dir;              // the run's folder (--resume <dir>)
  int epoch = 0, epochs = 0;
  std::string mode, size;
};
ResumeOffer findResumableExport(SawbladeProcessor& p);
// Request that resumes `offer` with the run's own mode and size.
bool buildResumeRequest(SawbladeProcessor& p, const ResumeOffer& offer, ExportRequest& out, std::string* error);

}  // namespace sawblade::plugin
