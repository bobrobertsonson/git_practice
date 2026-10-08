#include "sawblade/block_registry.h"

#include <stdexcept>

#include "sawblade/capture_cache.h"
#include "sawblade/eq.h"
#include "sawblade/gain_ladder.h"
#include "sawblade/nam_block.h"
#include "sawblade/pedal_eye.h"
#include "sawblade/pedal_hm.h"
#include "sawblade/pedal_hmx.h"
#include "sawblade/pedal_muff.h"
#include "sawblade/pedal_ts.h"

namespace sawblade {
namespace {

std::shared_ptr<const BlockParams> parseNam(JsonObject& o, const std::filesystem::path& baseDir) {
  auto p = std::make_shared<NamBlockParams>();
  p->inputGainDb = o.number("inputGainDb", 0.0, -120.0, 60.0);
  p->outputGainDb = o.number("outputGainDb", 0.0, -120.0, 60.0);
  p->normalizeLoudness = o.boolean("normalizeLoudness", false);
  p->makeupDb = o.number("makeupDb", 0.0, -kMaxAutoTrimDb, kMaxAutoTrimDb);
  const nlohmann::json* m = o.take("model");
  if (!m) throw PresetError(o.child("model"), "required field is missing");
  p->model = parseCapture(*m, o.child("model"), baseDir, /*allowLadder=*/true);
  return p;
}

std::unique_ptr<Processor> createNam(const Block& b, const BlockBuildContext& ctx) {
  const auto& p = static_cast<const NamBlockParams&>(*b.params);
  const std::string filePath = ctx.jsonPath + ".model.file";
  NamBlockConfig cfg;
  cfg.inputGainDb = p.inputGainDb;
  cfg.outputGainDb = p.outputGainDb + p.makeupDb;
  cfg.normalizeLoudness = p.normalizeLoudness;
  cfg.makeupDb = p.makeupDb;
  const auto build = [&](const Capture& cap, const std::string& fp, bool verify) -> std::unique_ptr<NamBlock> {
    if (ctx.cache) return NamBlock::load(*ctx.cache->namModel(cap, fp), cfg);  // bypass: see Chain
    if (verify) verifyCapture(cap, fp);
    try {
      return NamBlock::load(locateCapture(cap), cfg);  // bypass is handled by the Chain
    } catch (const std::exception& e) {
      throw CaptureError(fp, e.what());
    }
  };
  const int own = ownRungIndex(p.model);
  if (p.model.ladder.size() >= 2 && own >= 0) {
    // A gain ladder: the block starts on the rung `gainStep` names (offline renders use it directly, no crossfade) if
    // that model is cached, else on its own capture; the Chain drives the rest.
    int active = own;
    if (ctx.gainStep && !ctx.gainStep->empty()) {
      const int r = rungIndexOfModel(p.model.ladder, *ctx.gainStep);
      if (r < 0) {
        if (ctx.warnings) ctx.warnings->push_back(ctx.jsonPath + ".ampControls.gainStep: \"" + *ctx.gainStep + "\" is not a rung of the ladder; using the block's own capture");
      } else if (r != own) {
        if (locateRungFile(p.model, p.model.ladder[static_cast<std::size_t>(r)])) active = r;
        else if (ctx.warnings) ctx.warnings->push_back(ctx.jsonPath + ".ampControls.gainStep: rung " + *ctx.gainStep + " is not in the capture cache; using the block's own capture (drive only)");
      }
    }
    std::unique_ptr<NamBlock> nb = active == own
        ? build(p.model, filePath, true)
        : build(rungCapture(p.model, p.model.ladder[static_cast<std::size_t>(active)]), ctx.jsonPath + ".model.ladder", false);
    return std::make_unique<LadderBlock>(static_cast<int>(p.model.ladder.size()), active, std::move(nb));
  }
  return build(p.model, filePath, true);
}

std::shared_ptr<const BlockParams> parseEq(JsonObject& o, const std::filesystem::path&) {
  auto p = std::make_shared<EqBlockParams>();
  const auto& arr = o.requireArray("bands", ParametricEq::kMaxBands);
  p->bands = parseEqBands(arr, o.child("bands"));
  return p;
}

std::unique_ptr<Processor> createEq(const Block& b, const BlockBuildContext& ctx) {
  const auto& p = static_cast<const EqBlockParams&>(*b.params);
  auto eq = std::make_unique<ParametricEq>();
  try {
    eq->configure(ctx.sampleRate, p.bands);
  } catch (const std::invalid_argument& e) {
    throw PresetError(ctx.jsonPath + ".bands", e.what());  // semantic: depends on the render rate
  }
  return eq;
}

std::unique_ptr<Processor> createHm(const Block& b, const BlockBuildContext&) {
  return std::make_unique<HmPedal>(static_cast<const HmBlockParams&>(*b.params).p);
}

std::unique_ptr<Processor> createMuff(const Block& b, const BlockBuildContext&) {
  return std::make_unique<MuffPedal>(static_cast<const MuffBlockParams&>(*b.params).p);
}

std::unique_ptr<Processor> createTs(const Block& b, const BlockBuildContext&) {
  return std::make_unique<TsPedal>(static_cast<const TsBlockParams&>(*b.params).p);
}

}  // namespace

BlockRegistry::BlockRegistry() {
  types_["nam"] = BlockType{{/*namTrainable=*/true}, parseNam, createNam, {}};
  types_["eq"] = BlockType{{/*namTrainable=*/true}, parseEq, createEq, {}};
  // Modeled pedals: static, nonlinear, time-invariant, so NAM-trainable.
  types_["pedal.hm"] = BlockType{{/*namTrainable=*/true}, parseHmBlock, createHm, hmLiveParamDescs()};
  types_["pedal.muff"] = BlockType{{/*namTrainable=*/true}, parseMuffBlock, createMuff, muffLiveParamDescs()};
  types_["pedal.ts"] = BlockType{{/*namTrainable=*/true}, parseTsBlock, createTs, {}};
  types_["pedal.hmx"] = BlockType{{/*namTrainable=*/true}, parseHmxBlock, createHmx, hmxLiveParamDescs()};
  types_["pedal.eye"] = BlockType{{/*namTrainable=*/true}, parseEyeBlock, createEye, eyeLiveParamDescs()};
}

BlockRegistry& BlockRegistry::instance() {
  static BlockRegistry r;
  return r;
}

void BlockRegistry::add(const std::string& type, BlockType t) {
  if (!types_.emplace(type, std::move(t)).second) throw std::invalid_argument("block type already registered: " + type);
}

const BlockType* BlockRegistry::find(const std::string& type) const {
  auto it = types_.find(type);
  return it == types_.end() ? nullptr : &it->second;
}

std::vector<std::string> BlockRegistry::typeNames() const {
  std::vector<std::string> n;
  n.reserve(types_.size());
  for (const auto& kv : types_) n.push_back(kv.first);  // std::map: already sorted
  return n;
}

}  // namespace sawblade
