#include "MicSession.h"

#include <algorithm>

#include "presets/T3kTool.h"

namespace sawblade::plugin::mic {

bool sameCab(const CabPreset& a, const CabPreset& b) {
  if (a.mode != b.mode || a.enabled != b.enabled || a.normalize != b.normalize) return false;
  switch (a.mode) {
    case CabMode::Shared: return a.ir == b.ir;
    case CabMode::PerPath: return a.irA == b.irA && a.irB == b.irB;
    case CabMode::IrMix: return a.irA == b.irA && a.irB == b.irB && a.mix == b.mix;
  }
  return false;
}

MicSession::MicSession() = default;

void MicSession::setLayout(const CabLayout& layout) {
  layout_ = layout;
  pack_.setLayout(layout_);
}

void MicSession::setPack(IrPack pack) {
  pack_ = std::move(pack);
  pack_.setLayout(layout_);
  prev_[0].reset();
  prev_[1].reset();
}

bool MicSession::loadCachedPack(std::string* error) {
  const std::string id = toneId();
  if (id.empty()) return false;
  std::error_code ec;
  const auto path = packManifestPath(id);
  if (!std::filesystem::is_regular_file(path, ec)) return false;
  std::string err;
  IrPack p = IrPack::fromManifestFile(path, &err);
  if (p.empty()) {
    if (error) *error = err;
    return false;
  }
  if (p.info().toneId != id) return false;
  setPack(std::move(p));
  return true;
}

int MicSession::model(int mic) const { return pack_.findModel(cap_[mic == 1 ? 1 : 0]); }

void MicSession::refreshPackFor(const Capture& a) {
  const bool keep = pack_.info().kind == PackInfo::Kind::Folder ||
                    (pack_.info().kind == PackInfo::Kind::Manifest && !toneIdOf(a).empty() && pack_.info().toneId == toneIdOf(a));
  if (keep) return;
  setPack(IrPack::single(a));
  loadCachedPack();
}

bool MicSession::adopt(const Preset& p) {
  const CabPreset& c = p.cab;
  const bool before = !pack_.empty();
  Capture a, b;
  bool blend = false, perPath = false;
  switch (c.mode) {
    case CabMode::Shared: a = c.ir; break;
    case CabMode::IrMix: a = c.irA; b = c.irB; blend = true; break;
    case CabMode::PerPath: a = c.irA; b = c.irB; perPath = true; break;
  }
  const bool changed = !before || a != cap_[0] || (blend && b != cap_[1]) || blend != blend_ || perPath != perPath_ ||
                       (blend && c.mix != mix_) || c.enabled != cab_.enabled || c.normalize != cab_.normalize;
  cab_ = c;
  cap_[0] = a;
  cap_[1] = blend || perPath ? b : a;
  blend_ = blend;
  perPath_ = perPath;
  if (blend) mix_ = c.mix;
  if (!blend) active_ = 0;
  if (changed) refreshPackFor(a);
  return changed;
}

Preset MicSession::build(const Preset& base) const {
  Preset p = base;
  CabPreset c = base.cab;
  c.ir = Capture{};
  c.irA = Capture{};
  c.irB = Capture{};
  c.mix = 0.5;
  if (perPath_) {
    c.mode = CabMode::PerPath;
    c.irA = cap_[0];
    c.irB = cap_[1];
  } else if (blend_) {
    c.mode = CabMode::IrMix;
    c.irA = cap_[0];
    c.irB = cap_[1];
    c.mix = mix_;
  } else {
    c.mode = CabMode::Shared;
    c.ir = cap_[0];
  }
  p.cab = std::move(c);
  return p;
}

std::optional<Preset> MicSession::choose(const Preset& base, int mic, int model) {
  if (perPath_ || model < 0 || model >= pack_.size()) return std::nullopt;
  mic = mic == 1 && blend_ ? 1 : 0;
  const Capture next = pack_.captureFor(model);
  if (next != cap_[mic]) {
    prev_[mic] = cap_[mic];
    cap_[mic] = next;
  }
  active_ = mic;
  return build(base);
}

std::optional<Preset> MicSession::setBlend(const Preset& base, bool on) {
  if (perPath_ || on == blend_) return std::nullopt;
  blend_ = on;
  if (on) {
    cap_[1] = cap_[0];  // the sound does not change
    mix_ = 0.5;
    prev_[1].reset();
  }
  active_ = 0;
  return build(base);
}

std::optional<Preset> MicSession::setMix(const Preset& base, double mix) {
  if (perPath_ || !blend_) return std::nullopt;
  mix_ = std::clamp(mix, 0.0, 1.0);
  return build(base);
}

std::optional<Preset> MicSession::toggleAB(const Preset& base) {
  if (!canToggleAB()) return std::nullopt;
  std::swap(cap_[active_], *prev_[active_]);
  return build(base);
}

std::optional<Preset> MicSession::nextPosition(const Preset& base) {
  if (perPath_ || pack_.dots().empty()) return std::nullopt;
  const int cur = model(active_);
  const int d = pack_.dotOfModel(cur);
  const int n = static_cast<int>(pack_.dots().size());
  if (n == 1 && d == 0) return std::nullopt;
  const int target = d < 0 ? 0 : (d + 1) % n;
  const Snap s = pack_.snapToNearest(pack_.dots()[static_cast<std::size_t>(target)].pos, cur);
  return choose(base, active_, s.model);
}

}  // namespace sawblade::plugin::mic
