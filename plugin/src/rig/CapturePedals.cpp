#include "rig/CapturePedals.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

#include <nlohmann/json.hpp>

#include "sawblade/preset.h"

namespace sawblade::plugin::rig {
namespace fs = std::filesystem;

bool plainCacheId(const std::string& s) { return !s.empty() && s.find_first_of("/\\.") == std::string::npos; }

bool plainCacheFile(const std::string& file) {
  if (file.empty() || file.find_first_of("/\\") != std::string::npos || file.find("..") != std::string::npos) return false;
  const fs::path p(file);
  return p.filename().string() == file && plainCacheId(p.stem().string());
}

namespace {
using nlohmann::json;

std::string str(const json& o, const char* k) { return o.is_object() && o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); }

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The first number in `s` ("Gain 6.5" -> 6.5).
std::optional<double> firstNumber(const std::string& s) {
  for (std::size_t i = 0; i < s.size(); ++i)
    if (std::isdigit(static_cast<unsigned char>(s[i]))) {
      char* end = nullptr;
      const double v = std::strtod(s.c_str() + i, &end);
      if (end != s.c_str() + i) return v;
    }
  return std::nullopt;
}

bool idLess(const std::string& a, const std::string& b) {
  const bool na = !a.empty() && a.find_first_not_of("0123456789") == std::string::npos;
  const bool nb = !b.empty() && b.find_first_not_of("0123456789") == std::string::npos;
  if (na && nb) return a.size() != b.size() ? a.size() < b.size() : a < b;
  return a < b;
}

json readMeta(const fs::path& dir) {
  std::ifstream in(dir / "meta.json");
  if (!in) return json();
  const json meta = json::parse(in, nullptr, /*allow_exceptions=*/false);
  return meta.is_object() ? meta : json();
}

std::vector<CachedModel> modelsIn(const fs::path& dir, const json& meta, bool namOnly) {
  std::vector<CachedModel> out;
  if (!meta.contains("models") || !meta["models"].is_object()) return out;
  std::error_code ec;
  for (const auto& kv : meta["models"].items()) {
    if (!plainCacheId(kv.key()) || !kv.value().is_object()) continue;
    const std::string file = str(kv.value(), "file").empty() ? kv.key() + ".nam" : str(kv.value(), "file");
    if (!plainCacheFile(file) || !fs::exists(dir / file, ec)) continue;
    if (namOnly && fs::path(file).extension() != ".nam") continue;
    const std::string name = str(kv.value().contains("model") ? kv.value()["model"] : json(), "name");
    out.push_back({kv.key(), name.empty() ? kv.key() : name});
  }
  return orderSettings(std::move(out));
}
}  // namespace

std::vector<CachedModel> orderSettings(std::vector<CachedModel> models) {
  bool numbered = !models.empty();
  for (const auto& m : models) numbered = numbered && firstNumber(m.name).has_value();
  if (numbered)
    std::stable_sort(models.begin(), models.end(), [](const CachedModel& a, const CachedModel& b) {
      const double x = *firstNumber(a.name), y = *firstNumber(b.name);
      return x != y ? x < y : idLess(a.modelId, b.modelId);
    });
  else
    std::stable_sort(models.begin(), models.end(), [](const CachedModel& a, const CachedModel& b) { return idLess(a.modelId, b.modelId); });
  return models;
}

std::vector<CachedModel> cachedModelsOf(const std::string& toneId) {
  if (!plainCacheId(toneId)) return {};
  const fs::path dir = captureCacheRoot() / toneId;
  return modelsIn(dir, readMeta(dir), /*namOnly=*/false);
}

std::vector<CachedPedal> cachedPedalCaptures() {
  std::vector<CachedPedal> out;
  std::error_code ec;
  const fs::path root = captureCacheRoot();
  if (!fs::is_directory(root, ec)) return out;
  for (const auto& e : fs::directory_iterator(root, ec)) {
    if (!e.is_directory(ec)) continue;
    const std::string toneId = e.path().filename().string();
    if (!plainCacheId(toneId)) continue;
    const json meta = readMeta(e.path());
    if (!meta.contains("tone") || str(meta["tone"], "gear") != "pedal") continue;
    CachedPedal p;
    p.toneId = toneId;
    p.models = modelsIn(e.path(), meta, /*namOnly=*/true);
    if (p.models.empty()) continue;
    const json& tone = meta["tone"];
    const json user = tone.contains("user") ? tone["user"] : json::object();
    p.title = str(tone, "title").empty() ? "Tone " + toneId : str(tone, "title");
    p.creator = !str(user, "display_name").empty() ? str(user, "display_name") : !str(user, "username").empty() ? str(user, "username") : str(meta, "creatorUsername");
    p.license = str(tone, "license");
    p.url = str(tone, "url");
    out.push_back(std::move(p));
  }
  std::sort(out.begin(), out.end(), [](const CachedPedal& a, const CachedPedal& b) {
    const std::string x = lower(a.title), y = lower(b.title);
    return x != y ? x < y : idLess(a.toneId, b.toneId);
  });
  return out;
}

}  // namespace sawblade::plugin::rig
