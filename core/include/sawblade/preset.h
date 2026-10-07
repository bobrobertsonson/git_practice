#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sawblade/amp_controls.h"
#include "sawblade/bus_comp.h"
#include "sawblade/eq.h"
#include "sawblade/gate.h"
#include "sawblade/preset_reader.h"

// C++ mirror of docs/PRESET_SCHEMA.md, v3 (v1 and v2 files are still read; v3 adds output.autoTrimDb / autoTrimHash and the
// nam block's makeupDb). Parsing is strict (see PresetError).
namespace sawblade {

constexpr int kPresetVersion = 4;
constexpr int kMaxBlocksPerPath = 8;
// Limits of the level-matching gains (auto_trim.h): output.autoTrimDb and a nam block's makeupDb are clamped / bounded to +-.
constexpr double kMaxAutoTrimDb = 48.0;

bool operator==(const GateParams&, const GateParams&);
bool operator==(const EqBand&, const EqBand&);

struct CaptureSource {
  std::string provider, id;                       // required when `source` is given
  std::string modelId, url, title, creator, license;  // optional ("" = absent)
  bool operator==(const CaptureSource&) const = default;
};

// One rung of a gain ladder (v0.2 Task B): the same amp at another gain setting. `modelId` is the TONE3000 model id
// (a string); `gain` the amp's gain setting as the pack names it; `name` the model's title (display only).
struct LadderRung {
  std::string modelId;
  double gain = 0.0;
  std::string name;
  bool operator==(const LadderRung&) const = default;
};
constexpr int kMaxLadderRungsInPreset = 64;

// NAM model or IR reference. `file` is as written; `resolvedPath` is absolute-or-relative to the
// process cwd as resolved against the preset file's directory.
struct Capture {
  std::string file;
  std::filesystem::path resolvedPath;
  std::string sha256;  // lowercase hex, empty if absent
  std::optional<CaptureSource> source;
  std::vector<LadderRung> ladder;  // NAM model captures only (v0.2): ascending gain, 2..64 rungs, or empty
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
  // v3 capture-swap make-up (docs/PRESET_SCHEMA.md "Level matching"): a gain, in dB, on the block's output, written by the plugin
  // when the capture in this slot is replaced so that the path's loudness on the reference DI stays unchanged. Kept apart from
  // outputGainDb (the user's knob); the chain applies both. 0 = absent in the file.
  double makeupDb = 0.0;
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
  AmpControls ampControls;  // v2; all-default (and no gainStep) = absent in the file
  bool operator==(const PathPreset&) const = default;
};

// The block the path's amp controls act around: the last block with slot "amp", else the last "nam"
// block; -1 if the path has none (it then has no amp controls). The one rule, shared with the rig UI.
int ampIndex(const PathPreset& p);

enum class AlignMode { Auto, Manual, Off };
struct AlignParams {
  AlignMode mode = AlignMode::Auto;
  double maxLagMs = 5.0;
  int delaySamplesB = 0;
  bool invertB = false;
  bool operator==(const AlignParams&) const = default;
};

// Phase 10.1. Defaults when the keys are absent: mode off, law linear (legacy presets render
// bit-identically). Trims are the stored values of `manual` mode, in [0, kMaxLevelTrimDb].
enum class LevelMatchMode { Auto, Manual, Off };
struct LevelMatch {
  LevelMatchMode mode = LevelMatchMode::Off;
  double trimADb = 0.0, trimBDb = 0.0;
  bool operator==(const LevelMatch&) const = default;
};
constexpr double kMaxLevelTrimDb = 18.0;
enum class BlendLaw { Linear, ConstantLoudness };

constexpr int kMaxIrOffsetSamples = 256;
enum class CabMode { Shared, PerPath, IrMix };
struct CabPreset {
  CabMode mode = CabMode::Shared;
  Capture ir;          // shared
  Capture irA, irB;    // perPath and irMix
  double mix = 0.5;    // irMix only: h = (1 - mix) * irA + mix * irB, in [0, 1]
  int offsetSamplesB = 0;   // irMix only, [-kMaxIrOffsetSamples, +kMaxIrOffsetSamples]: hB shifted right (+) / advanced (-)
  bool invertB = false;     // irMix only: hB is negated before the sum
  bool enabled = true;
  bool normalize = true;
  bool operator==(const CabPreset&) const = default;
};

// The measurement the level matching stores in a preset (output.autoTrimDb / autoTrimHash): metadata about the sound, not part of it.
// Two presets that differ only in their stamp are equal (the plugin writes a freshly measured trim into the running preset without
// it being an edit; undo and "did anything change" comparisons must not see it).
struct AutoTrimStamp {
  double db = 0.0;
  std::string hash;  // "" = not measured
  bool operator==(const AutoTrimStamp&) const noexcept { return true; }
};

// v4 (docs/PRESET_SCHEMA.md "Live dynamics"): who made the preset. Only "match" presets get the derived live dynamics.
enum class PresetOrigin { User, Match, Official };
// Which dynamics set the engine runs: the record set (the preset's `gate` / `busComp`, as fitted to a recording) or the live set.
enum class DynamicsMode { Record, Live };

// One complete set of dynamics processing; handed over as one object (plugin) so no block runs half old, half new.
struct DynamicsSet {
  GateParams gate = [] { GateParams g; g.enabled = false; return g; }();
  BusCompParams busComp;
  bool operator==(const DynamicsSet&) const = default;
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
  BlendLaw blendLaw = BlendLaw::Linear;
  LevelMatch levelMatch;
  CabPreset cab;
  std::vector<EqBand> postEq;
  BusCompParams busComp;
  // v4: `gate` + `busComp` above are the record set. `liveDynamics` is the explicit live set (absent = derived, see
  // liveDynamicsOf); `dynamicsMode` absent in the file = record (nullopt; the plugin then sets Live on load).
  std::optional<DynamicsSet> liveDynamics;
  std::optional<DynamicsMode> dynamicsMode;
  PresetOrigin origin = PresetOrigin::User;
  double outputGainDb = 0.0;
  // v3 (docs/PRESET_SCHEMA.md "Level matching"): the trim, in dB, that brings this preset to kAutoTrimTargetLufs on the reference DI
  // (auto_trim.h), and the hash of the level-affecting parts it was measured for ("" = not measured). It is a plain gain after
  // outputGainDb, applied only when the player asks for it (RenderOptions::applyAutoTrim, the plugin's LEVEL MATCH); the chain,
  // the NAM export and the matcher never see it.
  AutoTrimStamp autoTrim;
  bool operator==(const Preset&) const = default;
};

// The record set of a preset (its `gate` / `busComp`).
DynamicsSet recordDynamicsOf(const Preset& p);
// The derivation rule applied to a record set (origin "match" only; see liveDynamicsOf).
DynamicsSet deriveLiveDynamics(const DynamicsSet& record);
// The live set: the explicit `liveDynamics` if present; else, for origin "match", deriveLiveDynamics(record); else (user,
// official, old files) the record set unchanged.
DynamicsSet liveDynamicsOf(const Preset& p);
// THE resolver: the set the preset's dynamicsMode selects (absent = record). Used by the render path (Chain), the NAM
// exporter and the plugin.
DynamicsSet activeDynamics(const Preset& p);
inline DynamicsMode effectiveDynamicsMode(const Preset& p) { return p.dynamicsMode.value_or(DynamicsMode::Record); }

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
Capture parseCapture(const nlohmann::json& j, const std::string& path, const std::filesystem::path& baseDir, bool allowLadder = false);
nlohmann::json toJson(const Capture& c);
std::vector<EqBand> parseEqBands(const nlohmann::json& arr, const std::string& path);
nlohmann::json toJson(const EqBand& b);

}  // namespace sawblade
