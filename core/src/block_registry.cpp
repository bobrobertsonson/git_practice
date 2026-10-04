#include "sawblade/block_registry.h"

#include <stdexcept>

#include "sawblade/capture_cache.h"
#include "sawblade/eq.h"
#include "sawblade/nam_block.h"
#include "sawblade/pedal_eye.h"
#include "sawblade/pedal_hm.h"
#include "sawblade/pedal_hmx.h"
#include "sawblade/pedal_ts.h"

namespace sawblade {
namespace {

std::shared_ptr<const BlockParams> parseNam(JsonObject& o, const std::filesystem::path& baseDir) {
  auto p = std::make_shared<NamBlockParams>();
  p->inputGainDb = o.number("inputGainDb", 0.0, -120.0, 60.0);
  p->outputGainDb = o.number("outputGainDb", 0.0, -120.0, 60.0);
  p->normalizeLoudness = o.boolean("normalizeLoudness", false);
  const nlohmann::json* m = o.take("model");
  if (!m) throw PresetError(o.child("model"), "required field is missing");
  p->model = parseCapture(*m, o.child("model"), baseDir);
  return p;
}

std::unique_ptr<Processor> createNam(const Block& b, const BlockBuildContext& ctx) {
  const auto& p = static_cast<const NamBlockParams&>(*b.params);
  const std::string filePath = ctx.jsonPath + ".model.file";
  NamBlockConfig cfg;
  cfg.inputGainDb = p.inputGainDb;
  cfg.outputGainDb = p.outputGainDb;
  cfg.normalizeLoudness = p.normalizeLoudness;
  if (ctx.cache) return NamBlock::load(*ctx.cache->namModel(p.model, filePath), cfg);  // bypass: see Chain
  verifyCapture(p.model, filePath);
  try {
    return NamBlock::load(p.model.resolvedPath, cfg);  // bypass is handled by the Chain
  } catch (const std::exception& e) {
    throw CaptureError(filePath, e.what());
  }
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

std::unique_ptr<Processor> createTs(const Block& b, const BlockBuildContext&) {
  return std::make_unique<TsPedal>(static_cast<const TsBlockParams&>(*b.params).p);
}

}  // namespace

BlockRegistry::BlockRegistry() {
  types_["nam"] = BlockType{{/*namTrainable=*/true}, parseNam, createNam};
  types_["eq"] = BlockType{{/*namTrainable=*/true}, parseEq, createEq};
  // Modeled pedals: static, nonlinear, time-invariant, so NAM-trainable.
  types_["pedal.hm"] = BlockType{{/*namTrainable=*/true}, parseHmBlock, createHm};
  types_["pedal.ts"] = BlockType{{/*namTrainable=*/true}, parseTsBlock, createTs};
  types_["pedal.hmx"] = BlockType{{/*namTrainable=*/true}, parseHmxBlock, createHmx};
  types_["pedal.eye"] = BlockType{{/*namTrainable=*/true}, parseEyeBlock, createEye};
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

}  // namespace sawblade
