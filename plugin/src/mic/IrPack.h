#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "IrNameParser.h"
#include "sawblade/preset.h"

// An IR pack (docs/specs/phase9a_mic_page.md section 4): one TONE3000 IR tone with many models (a manifest written by
// `sawblade-t3k pack`), or a local folder of WAVs, or just the single IR the cab uses now. JUCE-free; nothing here
// throws (errors come back as a message).
namespace sawblade::plugin::mic {

// Coordinates of the dots: the cab image's pixel coordinates divided by the image WIDTH (so the unit is the same
// in x and y and Euclidean distances are true distances). The image's left edge is x = 0, its top edge y = 0.
struct Point {
  double x = 0.0, y = 0.0;
  bool operator==(const Point&) const = default;
};

// The cab layout sidecar written next to an open cab view (design/render/cab_layout.py via export_ui_assets.py):
// every driver's centre and radii in pixels of the stored image.
struct CabLayout {
  struct Driver {
    int slot = 0;  // 1-based
    double cx = 0, cy = 0, coneRadius = 0, capRadius = 0;
  };
  std::string cab;  // "4x12" / "2x12"
  double width = 0, height = 0;
  std::vector<Driver> drivers;
  bool valid() const { return width > 0 && height > 0 && !drivers.empty(); }
};
bool parseCabLayout(std::string_view json, CabLayout& out);  // false (out untouched) if malformed

struct PackModel {
  std::string modelId, name;
  std::filesystem::path file;  // absolute
  std::string sha256;          // lower-case hex or ""
  MicShot shot;                // parsed from `name`
};

struct PackInfo {
  enum class Kind { None, Single, Manifest, Folder };
  Kind kind = Kind::None;
  std::string toneId, title, creator, license, url;  // manifest packs; folder: title = folder name
};

struct Dot {
  Point pos;
  int driver = 0;        // 0-based index into the layout's drivers
  double radial = 0.0;   // offset from the dust-cap centre as a fraction of the cone radius
  bool offAxis = false;  // some model of the dot is an off-axis shot (drawn with an angle mark)
  std::vector<int> models;  // model indices, ascending
};

struct Snap {
  int dot = -1, model = -1;  // -1 if the pack has no dots
};

class IrPack {
 public:
  // Sources. On failure the result is empty() and *error (if given) says why.
  static IrPack fromManifestJson(std::string_view json, std::string* error = nullptr);
  static IrPack fromManifestFile(const std::filesystem::path& file, std::string* error = nullptr);  // also checks that the IR files exist
  static IrPack fromFolder(const std::filesystem::path& dir, std::string* error = nullptr);
  // The cab's current IR as a pack of one model (nothing is read from disk).
  static IrPack single(const Capture& cap);

  bool empty() const { return models_.empty(); }
  const PackInfo& info() const { return info_; }
  const std::vector<PackModel>& models() const { return models_; }
  int size() const { return static_cast<int>(models_.size()); }

  // "2x12" when most models that name a cab size say 2x12, else "4x12".
  std::string preferredCab() const;

  // Dot layout. setLayout() maps every model to a point (and merges models at the same point into one dot).
  void setLayout(const CabLayout& layout);
  const std::vector<Dot>& dots() const { return dots_; }
  int dotOfModel(int model) const;  // -1 if none
  Point modelPoint(int model) const;

  // The dot nearest to `p` (Euclidean; ties go to the lower dot, i.e. the lower model index); inside it the model
  // with the current mic and the nearest distance to the current model's, else the dot's first model.
  Snap snapToNearest(Point p, int currentModel) const;

  // The Capture that choosing `model` writes into a preset: manifest packs carry the absolute cache path, the
  // sha256 and a TONE3000 `source`; folder packs and single IRs carry `file` only (a single IR returns the capture
  // it was made from).
  Capture captureFor(int model) const;
  // The model whose file is the capture's (by resolved path); -1 if none.
  int findModel(const Capture& cap) const;

 private:
  void finish();  // parses the names

  PackInfo info_;
  std::vector<PackModel> models_;
  Capture singleCapture_;
  CabLayout layout_;
  std::vector<Dot> dots_;
  std::vector<int> dotOfModel_;
  std::vector<Point> points_;
};

// The TONE3000 tone id of a capture ("" if it has none).
std::string toneIdOf(const Capture& cap);

}  // namespace sawblade::plugin::mic
