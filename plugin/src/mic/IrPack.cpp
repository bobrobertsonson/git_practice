#include "../PresetMapping.h"
#include "IrPack.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <limits>
#include <system_error>

#include <nlohmann/json.hpp>

namespace sawblade::plugin::mic {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string idString(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  return {};
}

std::string str(const json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double radialFor(const MicShot& s, int maxIndex) {
  switch (s.position) {
    case MicPosition::Cap: return 0.0;
    case MicPosition::CapEdge: return 0.3;
    case MicPosition::Cone: return 0.6;
    case MicPosition::Edge: return 0.9;
    case MicPosition::OffAxis: return 0.6;
    case MicPosition::Unknown: break;
  }
  if (s.positionIndex > 0) return 0.9 * static_cast<double>(s.positionIndex - 1) / static_cast<double>(std::max(maxIndex - 1, 1));
  return 0.3;
}

}  // namespace

bool parseCabLayout(std::string_view text, CabLayout& out) {
  const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return false;
  CabLayout l;
  l.cab = str(j, "cab");
  const auto num = [](const json& o, const char* k) { return o.contains(k) && o[k].is_number() ? o[k].get<double>() : 0.0; };
  l.width = num(j, "width");
  l.height = num(j, "height");
  const auto it = j.find("drivers");
  if (it == j.end() || !it->is_array()) return false;
  for (const auto& d : *it) {
    if (!d.is_object()) return false;
    CabLayout::Driver dr;
    dr.slot = d.contains("slot") && d["slot"].is_number_integer() ? d["slot"].get<int>() : static_cast<int>(l.drivers.size()) + 1;
    dr.cx = num(d, "cx");
    dr.cy = num(d, "cy");
    dr.coneRadius = num(d, "cone_radius");
    dr.capRadius = num(d, "cap_radius");
    if (dr.coneRadius <= 0) return false;
    l.drivers.push_back(dr);
  }
  if (!l.valid()) return false;
  out = std::move(l);
  return true;
}

std::string toneIdOf(const Capture& cap) {
  return cap.source && cap.source->provider == "tone3000" ? cap.source->id : std::string();
}

void IrPack::finish() {
  for (auto& m : models_) m.shot = parseIrName(m.name);
}

IrPack IrPack::fromManifestJson(std::string_view text, std::string* error) {
  IrPack p;
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return IrPack();
  };
  try {
    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return fail("the pack manifest is not valid JSON");
    p.info_.kind = PackInfo::Kind::Manifest;
    p.info_.toneId = j.contains("toneId") ? idString(j["toneId"]) : std::string();
    p.info_.title = str(j, "title");
    p.info_.creator = str(j, "creator");
    p.info_.license = str(j, "license");
    p.info_.url = str(j, "url");
    const auto it = j.find("models");
    if (it == j.end() || !it->is_array()) return fail("the pack manifest has no \"models\" list");
    for (const auto& m : *it) {
      if (!m.is_object()) return fail("the pack manifest has a malformed model entry");
      PackModel pm;
      pm.modelId = m.contains("modelId") ? idString(m["modelId"]) : std::string();
      pm.name = str(m, "name");
      const std::string file = str(m, "file");
      if (file.empty()) return fail("the pack manifest has a model without a \"file\"");
      pm.file = fs::path(file);
      pm.sha256 = lower(str(m, "sha256"));
      if (pm.name.empty()) pm.name = pm.file.stem().string();
      p.models_.push_back(std::move(pm));
    }
    if (p.models_.empty()) return fail("the pack manifest lists no models");
    p.finish();
    return p;
  } catch (const std::exception& e) {
    return fail(std::string("cannot read the pack manifest: ") + e.what());
  }
}

IrPack IrPack::fromManifestFile(const fs::path& file, std::string* error) {
  try {
    std::ifstream f(file, std::ios::binary);
    if (!f) {
      if (error) *error = "cannot open the pack manifest: " + file.string();
      return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    IrPack p = fromManifestJson(ss.str(), error);
    if (p.empty()) return p;
    std::size_t missing = 0;
    for (const auto& m : p.models_) {
      std::error_code ec;
      if (!fs::is_regular_file(m.file, ec)) ++missing;
    }
    if (missing > 0) {
      if (error) *error = std::to_string(missing) + " of " + std::to_string(p.models_.size()) + " IR files of the pack are missing from the cache";
      return {};
    }
    return p;
  } catch (const std::exception& e) {
    if (error) *error = std::string("cannot read the pack manifest: ") + e.what();
    return {};
  }
}

IrPack IrPack::fromFolder(const fs::path& dir, std::string* error) {
  try {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      if (error) *error = "not a folder: " + dir.string();
      return {};
    }
    IrPack p;
    p.info_.kind = PackInfo::Kind::Folder;
    p.info_.title = dir.filename().empty() ? dir.parent_path().filename().string() : dir.filename().string();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (!it->is_regular_file(e2)) continue;
      if (lower(it->path().extension().string()) != ".wav") continue;
      PackModel m;
      m.file = fs::absolute(it->path(), e2);
      m.name = it->path().stem().string();
      p.models_.push_back(std::move(m));
    }
    if (p.models_.empty()) {
      if (error) *error = "no .wav files in " + dir.string();
      return {};
    }
    std::sort(p.models_.begin(), p.models_.end(), [](const PackModel& a, const PackModel& b) {
      const std::string x = lower(a.name), y = lower(b.name);
      return x != y ? x < y : a.name < b.name;
    });
    p.finish();
    return p;
  } catch (const std::exception& e) {
    if (error) *error = std::string("cannot read the folder: ") + e.what();
    return {};
  }
}

IrPack IrPack::single(const Capture& cap) {
  IrPack p;
  if ((cap.file.empty() && cap.resolvedPath.empty()) || isNoCapture(cap)) return p;  // no IR (the Init preset): nothing to place
  p.info_.kind = PackInfo::Kind::Single;
  p.singleCapture_ = cap;
  PackModel m;
  m.file = locateCapture(cap);  // a cached-but-unresolved TONE3000 IR is found in the capture cache
  if (m.file.empty()) m.file = fs::path(cap.file);
  m.sha256 = cap.sha256;
  if (cap.source) {
    m.modelId = cap.source->modelId;
    m.name = cap.source->title;
    p.info_.toneId = toneIdOf(cap);
    p.info_.creator = cap.source->creator;
    p.info_.license = cap.source->license;
    p.info_.url = cap.source->url;
  }
  if (m.name.empty()) m.name = m.file.stem().string();
  p.info_.title = m.name;
  p.models_.push_back(std::move(m));
  p.finish();
  return p;
}

std::string IrPack::preferredCab() const {
  int two = 0, named = 0;
  for (const auto& m : models_) {
    if (m.shot.cabSize.empty()) continue;
    ++named;
    if (m.shot.cabSize == "2x12") ++two;
  }
  return named > 0 && two * 2 > named ? "2x12" : "4x12";
}

void IrPack::setLayout(const CabLayout& layout) {
  layout_ = layout;
  dots_.clear();
  dotOfModel_.assign(models_.size(), -1);
  points_.assign(models_.size(), Point{});
  if (!layout.valid() || models_.empty()) return;
  int maxIndex = 0;
  for (const auto& m : models_) maxIndex = std::max(maxIndex, m.shot.positionIndex);
  const int nDrivers = static_cast<int>(layout.drivers.size());
  struct Key {
    int driver;
    long radial;
    bool operator==(const Key&) const = default;
  };
  std::vector<Key> keys;
  for (int i = 0; i < static_cast<int>(models_.size()); ++i) {
    const MicShot& s = models_[static_cast<std::size_t>(i)].shot;
    const int driver = s.speakerSlot >= 1 ? (s.speakerSlot - 1) % nDrivers : 0;
    const double radial = radialFor(s, maxIndex);
    const Key key{driver, std::lround(radial * 10000.0)};
    std::size_t d = 0;
    while (d < keys.size() && !(keys[d] == key)) ++d;
    if (d == keys.size()) {
      keys.push_back(key);
      const auto& drv = layout.drivers[static_cast<std::size_t>(driver)];
      const double dir = drv.cx > layout.width / 2.0 ? -1.0 : 1.0;  // towards the cab centre, horizontally
      Dot dot;
      dot.driver = driver;
      dot.radial = radial;
      dot.pos = {(drv.cx + dir * radial * drv.coneRadius) / layout.width, drv.cy / layout.width};
      dots_.push_back(dot);
    }
    Dot& dot = dots_[d];
    dot.models.push_back(i);
    dot.offAxis = dot.offAxis || s.position == MicPosition::OffAxis;
    dotOfModel_[static_cast<std::size_t>(i)] = static_cast<int>(d);
    points_[static_cast<std::size_t>(i)] = dot.pos;
  }
}

int IrPack::dotOfModel(int model) const {
  return model >= 0 && model < static_cast<int>(dotOfModel_.size()) ? dotOfModel_[static_cast<std::size_t>(model)] : -1;
}

Point IrPack::modelPoint(int model) const {
  return model >= 0 && model < static_cast<int>(points_.size()) ? points_[static_cast<std::size_t>(model)] : Point{};
}

Snap IrPack::snapToNearest(Point p, int currentModel) const {
  Snap r;
  if (dots_.empty()) return r;
  double best = std::numeric_limits<double>::infinity();
  for (int d = 0; d < static_cast<int>(dots_.size()); ++d) {
    const double dx = dots_[static_cast<std::size_t>(d)].pos.x - p.x, dy = dots_[static_cast<std::size_t>(d)].pos.y - p.y;
    const double dist = dx * dx + dy * dy;
    if (dist < best) {  // strict: ties keep the lower dot
      best = dist;
      r.dot = d;
    }
  }
  const auto& models = dots_[static_cast<std::size_t>(r.dot)].models;
  r.model = models.front();
  if (currentModel >= 0 && currentModel < static_cast<int>(models_.size())) {
    const MicShot& cur = models_[static_cast<std::size_t>(currentModel)].shot;
    double bestDist = std::numeric_limits<double>::infinity();
    bool found = false;
    for (int m : models) {
      const MicShot& s = models_[static_cast<std::size_t>(m)].shot;
      if (s.mic != cur.mic) continue;
      const double dd = std::isnan(s.distanceIn) || std::isnan(cur.distanceIn) ? std::numeric_limits<double>::infinity()
                                                                                : std::fabs(s.distanceIn - cur.distanceIn);
      if (!found || dd < bestDist) {
        found = true;
        bestDist = dd;
        r.model = m;
      }
    }
  }
  return r;
}

Capture IrPack::captureFor(int model) const {
  Capture c;
  if (model < 0 || model >= static_cast<int>(models_.size())) return c;
  if (info_.kind == PackInfo::Kind::Single) return singleCapture_;
  const PackModel& m = models_[static_cast<std::size_t>(model)];
  c.file = m.file.string();
  c.resolvedPath = m.file;
  if (info_.kind == PackInfo::Kind::Manifest) {
    c.sha256 = m.sha256;
    CaptureSource s;
    s.provider = "tone3000";
    s.id = info_.toneId;
    s.modelId = m.modelId;
    s.title = m.name;
    s.creator = info_.creator;
    s.license = info_.license;
    s.url = info_.url;
    c.source = s;
  }
  return c;
}

int IrPack::findModel(const Capture& cap) const {
  std::error_code ec;
  fs::path want = locateCapture(cap);
  if (want.empty()) want = fs::path(cap.file);
  for (int i = 0; i < static_cast<int>(models_.size()); ++i) {
    const fs::path& f = models_[static_cast<std::size_t>(i)].file;
    if (f == want || fs::weakly_canonical(f, ec) == fs::weakly_canonical(want, ec)) return i;
  }
  // The same TONE3000 model downloaded to another cache folder.
  if (cap.source && cap.source->provider == "tone3000" && !cap.source->modelId.empty() && info_.kind == PackInfo::Kind::Manifest &&
      cap.source->id == info_.toneId)
    for (int i = 0; i < static_cast<int>(models_.size()); ++i)
      if (models_[static_cast<std::size_t>(i)].modelId == cap.source->modelId) return i;
  return -1;
}

}  // namespace sawblade::plugin::mic
