#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
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
  std::string provider, id;                       // required when `source` is given
  std::string modelId, url, title, creator, license;  // optional ("" = absent)
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

enum class CabMode { Shared, PerPath, IrMix };
struct CabPreset {
  CabMode mode = CabMode::Shared;
  Capture ir;          // shared
  Capture irA, irB;    // perPath and irMix
  double mix = 0.5;    // irMix only: h = (1 - mix) * irA + mix * irB, in [0, 1]
  bool enabled = true;
  bool normalize = true;
  bool operator==(const CabPreset&) const = default;
};

struct Preset {
  std::string schema = "sawblade.preset";
  int version = kPresetVersion;
  std::string name;
  std::string notes;
  std::string category;  // optional UI metadata (docs/PRESET_SCHEMA.md): not tone, ignored by the chain; "" = none
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

// A capture (NAM model or IR) could not be found, verified or loaded. what() is the full message
// (unchanged from a plain std::runtime_error); jsonPath() is the preset path of the offending
// `file` member, e.g. "paths.a.blocks[0].model.file".
class CaptureError : public std::runtime_error {
 public:
  CaptureError(std::string jsonPath, const std::string& message)
      : std::runtime_error(message), path_(std::move(jsonPath)) {}
  const std::string& jsonPath() const noexcept { return path_; }

 private:
  std::string path_;
};

// Load-time check of a capture: the file exists and, if `sha256` is set, hashes to it. Throws
// CaptureError (a std::runtime_error) with the JSON path in the message.
void verifyCapture(const Capture& c, const std::string& jsonPath);

// The TONE3000 capture cache root, as match/sawblade_match/t3k/cache.py: $SAWBLADE_CACHE_DIR if set, else
// ~/.cache/sawblade/captures. Layout: <root>/<tone id>/<model id>.nam (.wav for IRs).
std::filesystem::path captureCacheRoot();
// An in-process override that captureCacheRoot() consults before the environment (the plugin's Settings panel sets it for a
// stored cache folder; nothing mutates the process environment). nullopt clears it. Thread-safe (a mutex); only ever called at
// load time and from the message thread, never from the audio thread.
void setCaptureCacheRootOverride(std::optional<std::filesystem::path> root);
// Where the capture's file really is: `resolvedPath` if it exists; else, for a TONE3000 capture with id and modelId,
// the cached copy if that exists; else `resolvedPath`. IRs are recognised by the file extension (.wav / .flac).
// Every core loader (verifyCapture, CaptureCache, the NAM block, IR loading) goes through this.
std::filesystem::path locateCapture(const Capture& c);
// The "file not found" message for a capture (JSON path, file, and for a TONE3000 capture the resolve hint, or the reason the
// cache cannot be used when its id / modelId is not a plain token).
std::string captureNotFoundMessage(const Capture& c, const std::string& jsonPath);

// Shared parse helpers (used by block-type parse hooks).
Capture parseCapture(const nlohmann::json& j, const std::string& path, const std::filesystem::path& baseDir);
nlohmann::json toJson(const Capture& c);
std::vector<EqBand> parseEqBands(const nlohmann::json& arr, const std::string& path);
nlohmann::json toJson(const EqBand& b);

}  // namespace sawblade
