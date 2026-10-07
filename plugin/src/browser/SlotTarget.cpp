#include "SlotTarget.h"

#include <algorithm>

#include "rig/RigModel.h"

namespace sawblade::plugin {
namespace {

const PathPreset& pathOf(const Preset& p, char which) { return which == 'a' ? p.a : p.b; }
PathPreset& pathOf(Preset& p, char which) { return which == 'a' ? p.a : p.b; }

int findNamBlock(const PathPreset& path, bool pedal) {
  std::vector<int> nam;
  for (std::size_t i = 0; i < path.blocks.size(); ++i)
    if (path.blocks[i].type == "nam") nam.push_back(static_cast<int>(i));
  bool anySlot = false;
  for (int i : nam) anySlot = anySlot || !path.blocks[static_cast<std::size_t>(i)].slot.empty();
  for (int i : nam) {
    const std::string& s = path.blocks[static_cast<std::size_t>(i)].slot;
    if (pedal ? (s == "pedal" || s == "boost") : s == "amp") return i;
  }
  if (anySlot || nam.empty()) return -1;
  if (pedal) return nam.size() >= 2 ? nam.front() : -1;
  return nam.back();
}

}  // namespace

const char* slotName(Slot s) {
  switch (s) {
    case Slot::SawPedal: return "SAW PEDAL";
    case Slot::BodyPedal: return "BODY PEDAL";
    case Slot::SawAmp: return "SAW AMP";
    case Slot::BodyAmp: return "BODY AMP";
    case Slot::Cab: return "CAB";
  }
  return "";
}

const char* slotGear(Slot s) {
  switch (s) {
    case Slot::SawPedal:
    case Slot::BodyPedal: return "pedal";
    case Slot::SawAmp:
    case Slot::BodyAmp: return "amp";
    case Slot::Cab: return "ir";
  }
  return "";
}

std::vector<SlotTarget> slotTargets(const Preset& preset, Slot slot, std::string* why, const std::string& pinnedBlockId,
                                    const std::optional<InsertPoint>& insert) {
  std::vector<SlotTarget> out;
  if (slot == Slot::Cab) {
    SlotTarget t;
    if (preset.cab.mode == CabMode::Shared) {
      t.kind = SlotTarget::Kind::CabShared;
      t.label = "CAB";
      out.push_back(t);
    } else {
      t.kind = SlotTarget::Kind::CabA;
      t.label = "SAW CAB";
      out.push_back(t);
      t.kind = SlotTarget::Kind::CabB;
      t.label = "BODY CAB";
      out.push_back(t);
    }
    return out;
  }
  const bool saw = slot == Slot::SawPedal || slot == Slot::SawAmp;
  const bool pedal = slot == Slot::SawPedal || slot == Slot::BodyPedal;
  const char which = saw ? 'a' : 'b';
  if (insert && pedal) {
    const PathPreset& path = pathOf(preset, insert->path);
    if (static_cast<int>(path.blocks.size()) >= kMaxBlocksPerPath) {
      if (why) *why = std::string(insert->path == 'a' ? "SAW" : "BODY") + " path full: 8 blocks";
      return out;
    }
    SlotTarget t;
    t.kind = SlotTarget::Kind::InsertNamBlock;
    t.path = insert->path;
    t.blockIndex = std::clamp(insert->index, 0, static_cast<int>(path.blocks.size()));
    t.label = insert->path == 'a' ? "SAW PEDAL" : "BODY PEDAL";
    out.push_back(t);
    return out;
  }
  int idx = -1;
  if (!pinnedBlockId.empty()) {
    const PathPreset& path = pathOf(preset, which);
    bool found = false;
    for (std::size_t i = 0; i < path.blocks.size(); ++i)
      if (path.blocks[i].id == pinnedBlockId) {
        found = true;
        if (path.blocks[i].type == "nam") idx = static_cast<int>(i);
      }
    if (idx < 0) {
      if (why) *why = found ? "this pedal is a modeled circuit, not a capture: it has no capture to replace" : "the pedal you selected is no longer in this path";
      return out;
    }
  } else {
    idx = findNamBlock(pathOf(preset, which), pedal);
  }
  if (idx < 0) {
    if (why) *why = std::string("this preset has no ") + (pedal ? "pedal" : "amp") + " in the " + (saw ? "saw" : "body") + " path";
    return out;
  }
  SlotTarget t;
  t.kind = SlotTarget::Kind::NamBlock;
  t.path = which;
  t.blockIndex = idx;
  t.label = slotName(slot);
  out.push_back(t);
  return out;
}

std::optional<Preset> withCapture(const Preset& preset, const SlotTarget& target, const t3k::FetchResult& f, std::string& error) {
  Capture cap;
  cap.file = f.path;
  cap.resolvedPath = f.path;
  cap.sha256 = f.sha256;
  cap.source = f.source;
  Preset out = preset;
  if (target.kind == SlotTarget::Kind::InsertNamBlock) {
    if (f.kind != "nam") {
      error = "this capture is an impulse response, but a pedal needs a NAM model";
      return std::nullopt;
    }
    PathPreset& path = pathOf(out, target.path);
    Block blk;
    blk.id = rig::newBlockId(out, target.path);
    blk.type = "nam";
    blk.slot = "pedal";
    auto np = std::make_shared<NamBlockParams>();
    np->model = cap;
    blk.params = std::move(np);
    if (!rig::addBlock(path, target.blockIndex, std::move(blk))) {
      error = std::string(target.path == 'a' ? "SAW" : "BODY") + " path full: 8 blocks";
      return std::nullopt;
    }
    return out;
  }
  if (target.isIr()) {
    if (f.kind != "ir") {
      error = "this capture is a NAM model, but the cab needs an impulse response";
      return std::nullopt;
    }
    switch (target.kind) {
      case SlotTarget::Kind::CabShared: out.cab.ir = cap; break;
      case SlotTarget::Kind::CabA: out.cab.irA = cap; break;
      default: out.cab.irB = cap; break;
    }
    return out;
  }
  if (f.kind != "nam") {
    error = "this capture is an impulse response, but this slot needs a NAM model";
    return std::nullopt;
  }
  PathPreset& path = pathOf(out, target.path);
  if (target.blockIndex < 0 || static_cast<std::size_t>(target.blockIndex) >= path.blocks.size() ||
      path.blocks[static_cast<std::size_t>(target.blockIndex)].type != "nam") {
    error = "the block this slot pointed at no longer exists";
    return std::nullopt;
  }
  Block& blk = path.blocks[static_cast<std::size_t>(target.blockIndex)];
  auto np = std::make_shared<NamBlockParams>(static_cast<const NamBlockParams&>(*blk.params));
  np->model = cap;
  blk.params = std::move(np);
  return out;
}

}  // namespace sawblade::plugin
