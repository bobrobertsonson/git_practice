#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sawblade/preset.h"
#include "sawblade/processor.h"

namespace sawblade {

class CaptureCache;

// Static, per-type facts. Time-based effects (delay, reverb, modulation, long-release dynamics)
// are not NAM-trainable; the export phase refuses or bypasses them.
struct BlockTraits {
  bool namTrainable = true;
};

// The per-path amp control set (core/include/sawblade/amp_controls.h) is not a block type: the Chain owns it
// and the block picker cannot insert it. Its traits live here, next to the registry's: latency 0, and
// NAM-trainable because it is linear and time-invariant at fixed knobs (gain, shelves, a peak, a gain).
inline constexpr BlockTraits kAmpControlsTraits{true};
inline constexpr int kAmpControlsLatencySamples = 0;

struct BlockBuildContext {
  double sampleRate = 0.0;
  std::vector<std::string>* warnings = nullptr;  // loaders append human-readable warnings
  std::string jsonPath;                          // e.g. "paths.a.blocks[1]" for error messages
  CaptureCache* cache = nullptr;                 // optional: reuse parsed captures across loads
  // The path's ampControls.gainStep when this block is the path's amp block (else null): a nam block whose capture
  // has a gain ladder starts on that rung if its model is in the capture cache (v0.2 Task B).
  const std::string* gainStep = nullptr;
};

// One live (knob-movable) parameter of a block type; index order = the `values` order of
// Processor::setLiveParams. A non-empty `choices` makes it an enum: the value is a choice index.
struct LiveParamDesc {
  std::string key, name;
  double min = 0.0, max = 1.0, def = 0.0;
  std::vector<std::string> choices;  // empty = continuous
};

struct BlockType {
  BlockTraits traits;
  // Reads the type-specific members of a block object (common members id/type/slot/bypass are
  // already consumed). Must not call finish(); the caller does. Throws PresetError.
  std::function<std::shared_ptr<const BlockParams>(JsonObject&, const std::filesystem::path& baseDir)> parse;
  // Load-time: builds the processor (loads models etc.). Latency is whatever the processor
  // reports after prepare(). Throws std::runtime_error on I/O or model errors.
  std::function<std::unique_ptr<Processor>(const Block&, const BlockBuildContext&)> create;
  // Live parameters, if the type supports Processor::setLiveParams (empty otherwise).
  std::vector<LiveParamDesc> liveParams;
};

// Process-wide map type string -> BlockType. "nam" and "eq" are built in. add() is for startup
// (single-threaded); lookups afterwards are read-only.
class BlockRegistry {
 public:
  static BlockRegistry& instance();
  void add(const std::string& type, BlockType t);  // throws std::invalid_argument on duplicates
  const BlockType* find(const std::string& type) const;
  std::vector<std::string> typeNames() const;  // sorted

 private:
  BlockRegistry();
  std::map<std::string, BlockType, std::less<>> types_;
};

}  // namespace sawblade
