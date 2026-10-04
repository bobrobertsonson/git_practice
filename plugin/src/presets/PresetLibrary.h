#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "sawblade/preset.h"

// The preset library behind the browser (docs/specs/phase9b_preset_browser.md section 2). JUCE-free.
// Banks: Factory (read-only: presets/*.json "Classic", presets/styles "Styles", presets/matched "Matched") and User
// (<appdata>/sawblade/presets, created on first save). `*.resolved.json` files are never listed. Scanning parses with the
// core parser, which does not load captures; unparseable files are listed with their error and are not loadable.
namespace sawblade::plugin {

enum class SubBank { Classic, Styles, Matched, User };
const char* subBankName(SubBank b);  // "Classic" / "Styles" / "Matched" / "User"
inline bool isFactory(SubBank b) { return b != SubBank::User; }

struct CaptureSummary {
  std::string where;     // "Path A / a1 (amp)", "Cab", "Cab irA"
  std::string title, creator, license, url, file;
  bool hasSource = false;     // no `source`: "local file: no attribution recorded"
  bool nonCommercial = false; // licence contains "nc" (case-insensitive)
};

struct PresetEntry {
  SubBank bank = SubBank::Classic;
  std::filesystem::path file;
  std::string name, category, notes;
  std::string error;  // non-empty: not loadable
  std::vector<CaptureSummary> captures;
  bool loadable() const { return error.empty(); }
  // "Uncategorised" for an empty category, "Matched" for an uncategorised preset in matched/.
  std::string displayCategory() const;
};

struct LibraryConfig {
  std::filesystem::path factoryDir, userDir;
};
// factoryDir: settings key `factoryPresetDir`, default <repo>/presets; userDir: <appdata>/presets.
LibraryConfig defaultLibraryConfig();

struct LibraryFilter {
  bool anyBank = true;
  bool factoryOnly = false;  // with !anyBank: all three factory sub-banks
  SubBank bank = SubBank::User;  // with !anyBank && !factoryOnly
  std::string category;          // displayCategory() or "" for all
  std::string search;            // whitespace-separated terms, ANDed, case-insensitive
};

// Pure helpers (also used by tests).
bool nonCommercialLicense(const std::string& license);
std::vector<CaptureSummary> summariseCaptures(const Preset& p);
PresetEntry readEntry(const std::filesystem::path& file, SubBank bank);
std::vector<PresetEntry> scanLibrary(const LibraryConfig& cfg);  // blocking: call off the message thread
bool entryMatches(const PresetEntry& e, const LibraryFilter& f);
std::string sanitiseFileName(const std::string& name);  // "" if nothing usable

class PresetLibrary {
 public:
  explicit PresetLibrary(LibraryConfig cfg) : cfg_(std::move(cfg)) {}
  const LibraryConfig& config() const { return cfg_; }
  void setEntries(std::vector<PresetEntry> e) { entries_ = std::move(e); }
  void rescan() { entries_ = scanLibrary(cfg_); }  // blocking
  const std::vector<PresetEntry>& entries() const { return entries_; }

  // Indices of the entries passing `f`, in library order.
  std::vector<int> filtered(const LibraryFilter& f) const;
  // Categories (display names) among entries passing `f` minus its category filter, with counts.
  std::vector<std::pair<std::string, int>> categories(const LibraryFilter& f) const;
  int count(const LibraryFilter& f) const { return static_cast<int>(filtered(f).size()); }
  int indexOfFile(const std::filesystem::path& file) const;

  // --- user operations (all through the core writer; the play-along state is never part of a Preset) ---
  // Where Save As would write `name`.
  std::filesystem::path userPathFor(const std::string& name) const;
  // Writes `preset` (with `name` / `category` set) to the user bank. False + *error on a collision unless `overwrite`.
  bool saveAs(Preset preset, const std::string& name, const std::string& category, bool overwrite, std::filesystem::path* written,
              std::string* error);
  // Overwrites a user preset file with `preset` (name and category kept from `preset`).
  bool save(const Preset& preset, const std::filesystem::path& userFile, std::string* error);
  bool rename(int index, const std::string& newName, std::string* error);
  // Moves the file to the trash through `trash` (default: the OS trash); factory presets cannot be deleted.
  using TrashFn = std::function<bool(const std::filesystem::path&)>;
  bool remove(int index, const TrashFn& trash, std::string* error);
  static bool moveToOsTrash(const std::filesystem::path& file);

 private:
  LibraryConfig cfg_;
  std::vector<PresetEntry> entries_;
};

}  // namespace sawblade::plugin
