#include "ExportSettings.h"

namespace sawblade::plugin {

bool exportSizeValid(const std::string& arch, const std::string& size) {
  if (arch == "a2") return size == "full" || size == "lite";
  if (arch == "a1") return size == "feather" || size == "lite" || size == "standard";
  return false;
}

std::string defaultExportSize(const std::string& arch) { return arch == "a1" ? "standard" : "full"; }

std::string exportHistoryKey(const std::string& arch, const std::string& size) { return arch + "." + size; }

nlohmann::json exportSettingsToJson(const ExportSettings& s) {
  return {{"mode", s.mode}, {"arch", s.arch}, {"size", s.size}, {"diSource", s.diSource}, {"compChoice", s.compChoice}, {"outputFolder", s.outputFolder}};
}

ExportSettings exportSettingsFromJson(const nlohmann::json& j) {
  ExportSettings s;
  if (!j.is_object()) return s;
  auto pick = [&](const char* key, std::string& dst, std::initializer_list<const char*> allowed) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return;
    const std::string v = it->get<std::string>();
    for (const char* a : allowed)
      if (v == a) {
        dst = v;
        return;
      }
  };
  pick("mode", s.mode, {"", "nocab", "withcab"});
  // Architecture first; a saved object without one is pre-A2 (all its sizes were A1 sizes).
  const bool hasArch = j.contains("arch") && j["arch"].is_string();
  if (hasArch) pick("arch", s.arch, {"a2", "a1"});
  else if (j.contains("size")) s.arch = "a1";
  s.size = defaultExportSize(s.arch);
  if (auto it = j.find("size"); it != j.end() && it->is_string() && exportSizeValid(s.arch, it->get<std::string>())) s.size = it->get<std::string>();
  pick("diSource", s.diSource, {"take", "builtin"});
  pick("compChoice", s.compChoice, {"drop", "keep"});
  if (auto it = j.find("outputFolder"); it != j.end() && it->is_string()) s.outputFolder = it->get<std::string>();
  return s;
}

}  // namespace sawblade::plugin
