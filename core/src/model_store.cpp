#include "sawblade/model_store.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <tuple>

#include "sawblade/sha256.h"

namespace sawblade {
namespace {

std::string envOrEmpty(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

std::filesystem::path homeDir() {
  const std::string h = envOrEmpty("HOME");
  return h.empty() ? std::filesystem::path(".") : std::filesystem::path(h);
}

std::filesystem::path dataRoot() {
#if defined(__APPLE__)
  return homeDir() / "Library" / "Application Support" / "Sawblade";
#else
  const std::string x = envOrEmpty("XDG_DATA_HOME");
  if (!x.empty()) return std::filesystem::path(x) / "sawblade";
  return homeDir() / ".local" / "share" / "sawblade";
#endif
}

std::string trimLower(std::string s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  s = s.substr(b, e - b);
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// sha256 of a model file, cached in-process by (path, size, mtime).
std::string cachedSha256(const std::filesystem::path& p, std::uintmax_t size, std::filesystem::file_time_type mtime) {
  static std::mutex m;
  static std::map<std::tuple<std::string, std::uintmax_t, std::int64_t>, std::string> cache;
  const auto key = std::make_tuple(p.string(), size, static_cast<std::int64_t>(mtime.time_since_epoch().count()));
  {
    std::lock_guard<std::mutex> lk(m);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
  }
  std::string h = sha256File(p);
  std::lock_guard<std::mutex> lk(m);
  cache[key] = h;
  return h;
}

}  // namespace

const char* separationModelId(SeparationModel m) noexcept {
  return m == SeparationModel::Htdemucs6s ? "htdemucs_6s" : "htdemucs";
}
int separationModelSourceCount(SeparationModel m) noexcept { return m == SeparationModel::Htdemucs6s ? 6 : 4; }
const char* separationModelPinnedSha256(SeparationModel m) noexcept {
  return m == SeparationModel::Htdemucs6s ? "d23996ba2e9396d393e2bd53c29f1411bd33b8cf3451854ad32d746ad3d06132"
                                          : "79189af3c584b1a2145ae5e4182a50c0204f88b76e2829bd27e4d4a88ede427d";
}
std::string separationModelFetchCommand(SeparationModel m) {
  return std::string("match/.venv/bin/sawblade-models fetch --model ") + separationModelId(m);
}

std::filesystem::path defaultModelsDirectory() {
  const std::string e = envOrEmpty("SAWBLADE_MODELS_DIR");
  if (!e.empty()) return e;
  return dataRoot() / "models";
}

std::filesystem::path defaultStemsDirectory() {
  const std::string e = envOrEmpty("SAWBLADE_STEMS_DIR");
  if (!e.empty()) return e;
  const std::string m = envOrEmpty("SAWBLADE_MODELS_DIR");
  if (!m.empty()) {
    std::filesystem::path mp(m);
    while (!mp.empty() && !mp.has_filename()) mp = mp.parent_path();  // strip a trailing slash
    return mp.parent_path() / "stems";
  }
  return dataRoot() / "stems";
}

std::filesystem::path ModelStore::modelPath(SeparationModel m) const {
  return dir_ / (std::string(separationModelId(m)) + "-core-opset17.onnx");
}

ModelStatus ModelStore::check(SeparationModel m) const {
  ModelStatus st;
  st.model = m;
  st.path = modelPath(m);
  const std::string fix = "Fetch it with (from the repository root): " + separationModelFetchCommand(m);
  try {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(st.path, ec)) {
      st.state = ModelStatus::State::Missing;
      st.message = std::string("Separation model ") + separationModelId(m) + " not found at " + st.path.string() + ". " + fix;
      return st;
    }
    const auto size = std::filesystem::file_size(st.path, ec);
    if (ec) throw std::runtime_error(ec.message());
    const auto mtime = std::filesystem::last_write_time(st.path, ec);
    if (ec) throw std::runtime_error(ec.message());
    st.actualSha256 = cachedSha256(st.path, size, mtime);
    if (st.actualSha256 == separationModelPinnedSha256(m)) {
      st.matchedPinned = true;
    } else {
      std::ifstream sc(st.path.string() + ".sha256");
      if (sc) {
        std::stringstream ss;
        ss << sc.rdbuf();
        if (trimLower(ss.str()) == st.actualSha256) st.matchedSidecar = true;
      }
    }
    if (st.matchedPinned || st.matchedSidecar) {
      st.state = ModelStatus::State::Ok;
      st.message = std::string("Separation model ") + separationModelId(m) + " verified (" +
                   (st.matchedPinned ? "pinned sha256" : "sidecar sha256") + ").";
    } else {
      st.state = ModelStatus::State::Mismatch;
      st.message = std::string("Separation model ") + separationModelId(m) + " at " + st.path.string() +
                   " does not match its pinned sha256 or its .sha256 sidecar (got " + st.actualSha256 + "). " + fix;
    }
  } catch (const std::exception& e) {
    st.state = ModelStatus::State::Unreadable;
    st.message = std::string("Separation model ") + separationModelId(m) + " at " + st.path.string() +
                 " cannot be read (" + e.what() + "). " + fix;
  }
  return st;
}

}  // namespace sawblade
