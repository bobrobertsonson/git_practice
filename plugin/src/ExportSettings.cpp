#include "ExportSettings.h"

namespace sawblade::plugin {

nlohmann::json exportSettingsToJson(const ExportSettings& s) {
  return {{"mode", s.mode}, {"size", s.size}, {"diSource", s.diSource}, {"compChoice", s.compChoice}, {"outputFolder", s.outputFolder}};
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
  pick("size", s.size, {"feather", "lite", "standard"});
  pick("diSource", s.diSource, {"take", "builtin"});
  pick("compChoice", s.compChoice, {"drop", "keep"});
  if (auto it = j.find("outputFolder"); it != j.end() && it->is_string()) s.outputFolder = it->get<std::string>();
  return s;
}

}  // namespace sawblade::plugin
