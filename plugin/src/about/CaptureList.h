#pragma once

#include <string>
#include <vector>

#include "sawblade/preset.h"

namespace sawblade::plugin::about {

// One capture (NAM model or cab IR) of a preset, with the attribution CLAUDE.md requires.
struct CaptureRow {
  std::string slot;      // "Saw pedal", "Body amp", "Cab A", ...
  std::string title;     // source title, else the file name
  std::string file;      // as written in the preset
  std::string provider;  // "" when the capture has no source
  std::string creator, license, url;
  bool hasSource = false;
  bool onDisk = false;
  bool nonCommercial = false;  // licence contains "-nc"
};

// Every capture with a non-empty file: paths.a / paths.b NAM blocks in order, then the cab IR(s).
std::vector<CaptureRow> listCaptures(const sawblade::Preset& preset);

}  // namespace sawblade::plugin::about
