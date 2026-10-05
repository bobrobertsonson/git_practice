#include "CaptureList.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace sawblade::plugin::about {
namespace {
std::string capitalise(std::string s) {
  if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

void add(std::vector<CaptureRow>& out, const std::string& slot, const sawblade::Capture& c) {
  if (c.file.empty()) return;
  CaptureRow r;
  r.slot = slot;
  r.file = c.file;
  r.title = std::filesystem::path(c.file).filename().string();
  std::error_code ec;
  r.onDisk = std::filesystem::exists(sawblade::locateCapture(c), ec);  // incl. the TONE3000 cache copy
  if (c.source) {
    r.hasSource = true;
    r.provider = c.source->provider;
    if (!c.source->title.empty()) r.title = c.source->title;
    r.creator = c.source->creator;
    r.license = c.source->license;
    r.url = c.source->url;
    if (r.url.empty() && c.source->provider == "tone3000" && !c.source->id.empty()) r.url = "https://www.tone3000.com/tones/" + c.source->id;
    std::string l = r.license;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    r.nonCommercial = l.find("-nc") != std::string::npos;
  }
  out.push_back(std::move(r));
}
}  // namespace

bool isWebUrl(const std::string& url) {
  auto starts = [&](const char* prefix) {
    size_t n = 0;
    while (prefix[n] != '\0') ++n;
    if (url.size() <= n) return false;  // needs something after the scheme
    for (size_t i = 0; i < n; ++i)
      if (std::tolower(static_cast<unsigned char>(url[i])) != prefix[i]) return false;
    return true;
  };
  return starts("http://") || starts("https://");
}

std::vector<CaptureRow> listCaptures(const sawblade::Preset& preset) {
  std::vector<CaptureRow> out;
  const sawblade::PathPreset* paths[2] = {&preset.a, &preset.b};
  const char* fallback[2] = {"Path A", "Path B"};
  for (int i = 0; i < 2; ++i) {
    const std::string role = paths[i]->role.empty() ? fallback[i] : capitalise(paths[i]->role);
    for (const auto& b : paths[i]->blocks) {
      auto nam = std::dynamic_pointer_cast<const sawblade::NamBlockParams>(b.params);
      if (!nam) continue;
      add(out, b.slot.empty() ? role : role + " " + b.slot, nam->model);
    }
  }
  if (preset.cab.mode == sawblade::CabMode::Shared) {
    add(out, "Cab", preset.cab.ir);
  } else {
    add(out, "Cab A", preset.cab.irA);
    add(out, "Cab B", preset.cab.irB);
  }
  return out;
}

}  // namespace sawblade::plugin::about
