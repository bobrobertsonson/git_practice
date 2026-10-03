#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sawblade/bus_comp.h"
#include "sawblade/eq.h"
#include "sawblade/gate.h"
#include "sawblade/preset_reader.h"

// C++ mirror of docs/PRESET_SCHEMA.md, v1. Parsing is strict (see PresetError).
namespace sawblade {

constexpr int kPresetVersion = 1;
constexpr int kMaxBlocksPerPath = 8;

bool operator==(const GateParams&, const GateParams&);
bool operator==(const EqBand&, const EqBand&);

struct CaptureSource {
  std::string provider, id, url, title, creator, license;  // all required when `source` is given
  bool operator==(const CaptureSource&) const = default;
};

// NAM model or IR reference. `file` is as written; `resolvedPath` is absolute-or-relative to the
// process cwd as resolved against the preset file's directory.
struct Capture {
  std::string file;
  std::filesystem::path resolvedPath;
  std::string sha256;  // lowercase hex, empty if absent
  std::optional<CaptureSource> source;
  bool operator==(const Capture&) const = default;
};

// Type-specific part of a block; each registered block type defines its own subclass.
struct BlockParams {
  virtual ~BlockParams() = default;
  virtual bool equals(const BlockParams& other) const = 0;
  virtual nlohmann::json toJson() const = 0;  // type-specific members only
};

struct Block {
  std::string id;
  std::string type;
  std::string slot;  // "" if absent
  bool bypass = false;
  std::shared_ptr<const BlockParams> params;  // never null after parsing
  bool operator==(const Block& o) const;
};

struct NamBlockParams : BlockParams {
  double inputGainDb = 0.0;
  double outputGainDb = 0.0;
  bool normalizeLoudness = false;
  Capture model;
  bool equals(const BlockParams& other) const override;
  nlohmann::json toJson() const override;
};

struct EqBlockParams : BlockParams {
  std::vector<EqBand> bands;
  bool equals(const BlockParams& other) const override;
  nlohmann::json toJson() const override;
};

struct PathPreset {
  std::string role;  // "", "saw" or "body"
  bool enabled = true;
  std::vector<EqBand> preEq;
  std::vector<Block> blocks;
  std::vector<EqBand> eq;
  double levelDb = 0.0;
  bool invert = false;
  bool operator==(const PathPreset&) const = default;
};

enum class AlignMode { Auto, Manual, Off };
struct AlignParams {
  AlignMode mode = AlignMode::Auto;
  double maxLagMs = 5.0;
  int delaySamplesB = 0;
  bool invertB = false;
  bool operator==(const AlignParams&) const = default;
};

enum class CabMode { Shared, PerPath };
struct CabPreset {
  CabMode mode = CabMode::Shared;
  Capture ir;          // shared
  Capture irA, irB;    // perPath
  bool enabled = true;
  bool normalize = true;
  bool operator==(const CabPreset&) const = default;
};

struct Preset {
  std::string schema = "sawblade.preset";
  int version = kPresetVersion;
  std::string name;
  std::string notes;
  double inputGainDb = 0.0;
  GateParams gate = [] { GateParams g; g.enabled = false; return g; }();
  PathPreset a, b;
  AlignParams align;
  double blend = 0.5;
  CabPreset cab;
  std::vector<EqBand> postEq;
  BusCompParams busComp;
  double outputGainDb = 0.0;
  bool operator==(const Preset&) const = default;
};

// Block types are looked up in the BlockRegistry (unknown type -> PresetError).
Preset parsePreset(const nlohmann::json& j, const std::filesystem::path& baseDir);
nlohmann::json toJson(const Preset& p);
// Reads and parses a file; relative paths resolve against its directory. Throws PresetError for
// bad content (including invalid JSON) and std::runtime_error if the file cannot be read.
Preset loadPresetFile(const std::filesystem::path& path);

// Load-time check of a capture: the file exists and, if `sha256` is set, hashes to it. Throws
// std::runtime_error with the JSON path in the message.
void verifyCapture(const Capture& c, const std::string& jsonPath);

// Shared parse helpers (used by block-type parse hooks).
Capture parseCapture(const nlohmann::json& j, const std::string& path, const std::filesystem::path& baseDir);
nlohmann::json toJson(const Capture& c);
std::vector<EqBand> parseEqBands(const nlohmann::json& arr, const std::string& path);
nlohmann::json toJson(const EqBand& b);

}  // namespace sawblade
