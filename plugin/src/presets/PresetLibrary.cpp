#include "PresetLibrary.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include <juce_core/juce_core.h>

#include "../PresetMapping.h"
#include "T3kTool.h"

namespace sawblade::plugin {
namespace fs = std::filesystem;
using nlohmann::json;

const char* subBankName(SubBank b) {
  switch (b) {
    case SubBank::Classic: return "Classic";
    case SubBank::Styles: return "Styles";
    case SubBank::Matched: return "Matched";
    case SubBank::User: return "User";
  }
  return "";
}

std::string PresetEntry::displayCategory() const {
  if (!category.empty()) return category;
  return bank == SubBank::Matched ? "Matched" : "Uncategorised";
}

namespace {
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

LibraryConfig defaultLibraryConfig() {
  LibraryConfig c;
  c.factoryDir = settings::factoryPresetDir();
  c.userDir = appDataDir() / "presets";
  return c;
}

bool nonCommercialLicense(const std::string& license) { return lower(license).find("nc") != std::string::npos; }

static CaptureSummary summary(const std::string& where, const Capture& c) {
  CaptureSummary s;
  s.where = where;
  s.file = c.file;
  if (c.source) {
    s.hasSource = true;
    s.title = c.source->title;
    s.creator = c.source->creator;
    s.license = c.source->license;
    s.url = c.source->url;
    s.nonCommercial = nonCommercialLicense(c.source->license);
  }
  return s;
}

std::vector<CaptureSummary> summariseCaptures(const Preset& p) {
  std::vector<CaptureSummary> out;
  const PathPreset* paths[2] = {&p.a, &p.b};
  const char* names[2] = {"Path A", "Path B"};
  for (int k = 0; k < 2; ++k)
    for (const auto& b : paths[k]->blocks)
      if (const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get()))
        out.push_back(summary(std::string(names[k]) + " / " + b.id + (b.slot.empty() ? "" : " (" + b.slot + ")"), nam->model));
  switch (p.cab.mode) {
    case CabMode::Shared: out.push_back(summary("Cab", p.cab.ir)); break;
    case CabMode::PerPath:
      out.push_back(summary("Cab irA (path A)", p.cab.irA));
      out.push_back(summary("Cab irB (path B)", p.cab.irB));
      break;
    case CabMode::IrMix:
      out.push_back(summary("Cab irA (mic 1)", p.cab.irA));
      out.push_back(summary("Cab irB (mic 2)", p.cab.irB));
      break;
  }
  return out;
}

PresetEntry readEntry(const fs::path& file, SubBank bank) {
  PresetEntry e;
  e.bank = bank;
  e.file = file;
  e.name = file.stem().string();
  try {
    const Preset p = sawblade::loadPresetFile(file);  // parses; never loads captures
    e.name = p.name;
    e.category = p.category;
    e.notes = p.notes;
    e.captures = summariseCaptures(p);
  } catch (const std::exception& ex) {
    e.error = ex.what();
  }
  return e;
}

static void scanDir(const fs::path& dir, SubBank bank, std::vector<PresetEntry>& out) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> files;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    const std::string fn = it->path().filename().string();
    if (lower(it->path().extension().string()) != ".json") continue;
    if (lower(fn).size() >= 14 && lower(fn).compare(lower(fn).size() - 14, 14, ".resolved.json") == 0) continue;
    files.push_back(it->path());
  }
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return lower(a.filename().string()) < lower(b.filename().string()); });
  for (const auto& f : files) out.push_back(readEntry(f, bank));
}

std::vector<PresetEntry> scanLibrary(const LibraryConfig& cfg) {
  std::vector<PresetEntry> out;
  scanDir(cfg.factoryDir, SubBank::Classic, out);
  scanDir(cfg.factoryDir / "styles", SubBank::Styles, out);
  scanDir(cfg.factoryDir / "matched", SubBank::Matched, out);
  scanDir(cfg.userDir, SubBank::User, out);
  return out;
}

bool entryMatches(const PresetEntry& e, const LibraryFilter& f) {
  if (!f.anyBank) {
    if (f.factoryOnly ? !isFactory(e.bank) : e.bank != f.bank) return false;
  }
  if (!f.category.empty() && e.displayCategory() != f.category) return false;
  std::istringstream terms(lower(f.search));
  std::string term;
  std::string hay;
  bool built = false;
  while (terms >> term) {
    if (!built) {
      hay = lower(e.name + "\n" + e.displayCategory() + "\n" + e.category + "\n" + e.notes);
      for (const auto& c : e.captures) hay += "\n" + lower(c.title + "\n" + c.creator);
      built = true;
    }
    if (hay.find(term) == std::string::npos) return false;
  }
  return true;
}

std::string sanitiseFileName(const std::string& name) {
  std::string out;
  for (unsigned char c : name) {
    if (std::isalnum(c) || c >= 0x80 || c == '-' || c == '_' || c == '.' || c == '+' || c == '(' || c == ')') out.push_back(static_cast<char>(c));
    else if (c == ' ') out.push_back(' ');
    else out.push_back('_');
  }
  // trim spaces and dots at the ends (no hidden or empty names), collapse "..", keep it short
  auto trim = [](std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '.')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s;
  };
  out = trim(out);
  std::string::size_type pos;
  while ((pos = out.find("..")) != std::string::npos) out.replace(pos, 2, "_");
  if (out.size() > 120) out.resize(120);
  return trim(out);
}

std::vector<int> PresetLibrary::filtered(const LibraryFilter& f) const {
  std::vector<int> r;
  for (int i = 0; i < static_cast<int>(entries_.size()); ++i)
    if (entryMatches(entries_[static_cast<std::size_t>(i)], f)) r.push_back(i);
  return r;
}

std::vector<std::pair<std::string, int>> PresetLibrary::categories(const LibraryFilter& f) const {
  LibraryFilter g = f;
  g.category.clear();
  std::vector<std::pair<std::string, int>> out;
  for (int i : filtered(g)) {
    const std::string c = entries_[static_cast<std::size_t>(i)].displayCategory();
    auto it = std::find_if(out.begin(), out.end(), [&](const auto& p) { return p.first == c; });
    if (it == out.end()) out.emplace_back(c, 1);
    else ++it->second;
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return lower(a.first) < lower(b.first); });
  return out;
}

int PresetLibrary::indexOfFile(const fs::path& file) const {
  std::error_code ec;
  const fs::path want = fs::weakly_canonical(file, ec);
  for (int i = 0; i < static_cast<int>(entries_.size()); ++i)
    if (entries_[static_cast<std::size_t>(i)].file == file || fs::weakly_canonical(entries_[static_cast<std::size_t>(i)].file, ec) == want) return i;
  return -1;
}

fs::path PresetLibrary::userPathFor(const std::string& name) const {
  const std::string s = sanitiseFileName(name);
  return s.empty() ? fs::path() : cfg_.userDir / (s + ".json");
}

static bool writeJsonFile(const fs::path& file, const json& j, std::string* error) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  if (ec) {
    if (error) *error = "cannot create " + file.parent_path().string() + ": " + ec.message();
    return false;
  }
  const fs::path tmp = file.string() + ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    o << j.dump(2) << '\n';
    if (!o) {
      if (error) *error = "cannot write " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    if (error) *error = "cannot write " + file.string() + ": " + ec.message();
    return false;
  }
  return true;
}

bool PresetLibrary::saveAs(Preset preset, const std::string& name, const std::string& category, bool overwrite, fs::path* written, std::string* error) {
  const fs::path dest = userPathFor(name);
  if (dest.empty()) {
    if (error) *error = "the name has no usable characters";
    return false;
  }
  std::error_code ec;
  if (!overwrite && fs::exists(dest, ec)) {
    if (error) *error = "exists: " + dest.string();
    return false;
  }
  preset.name = name;
  preset.category = category;
  if (!writeJsonFile(dest, json::parse(presetToStateJson(preset)), error)) return false;  // absolute capture paths
  if (written) *written = dest;
  rescan();
  return true;
}

bool PresetLibrary::save(const Preset& preset, const fs::path& userFile, std::string* error) {
  if (!writeJsonFile(userFile, json::parse(presetToStateJson(preset)), error)) return false;
  rescan();
  return true;
}

bool PresetLibrary::rename(int index, const std::string& newName, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  if (index < 0 || index >= static_cast<int>(entries_.size())) return fail("no such preset");
  const PresetEntry e = entries_[static_cast<std::size_t>(index)];
  if (isFactory(e.bank)) return fail("factory presets are read-only");
  if (!e.loadable()) return fail("this preset file cannot be read: " + e.error);
  const fs::path dest = userPathFor(newName);
  if (dest.empty()) return fail("the name has no usable characters");
  std::error_code ec;
  if (dest != e.file && fs::exists(dest, ec)) return fail("exists: " + dest.string());
  json j = json::parse(std::ifstream(e.file), nullptr, false);
  if (!j.is_object()) return fail("cannot read " + e.file.string());
  j["name"] = newName;
  if (!writeJsonFile(dest, j, error)) return false;
  if (dest != e.file) fs::remove(e.file, ec);
  rescan();
  return true;
}

bool PresetLibrary::moveToOsTrash(const fs::path& file) { return juce::File(file.string()).moveToTrash(); }

bool PresetLibrary::remove(int index, const TrashFn& trash, std::string* error) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) {
    if (error) *error = "no such preset";
    return false;
  }
  const PresetEntry& e = entries_[static_cast<std::size_t>(index)];
  if (isFactory(e.bank)) {
    if (error) *error = "factory presets are read-only";
    return false;
  }
  if (!(trash ? trash(e.file) : moveToOsTrash(e.file))) {
    if (error) *error = "could not move " + e.file.string() + " to the trash";
    return false;
  }
  rescan();
  return true;
}

}  // namespace sawblade::plugin
