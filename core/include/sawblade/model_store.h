#pragma once

#include <filesystem>
#include <string>

// Locating and verifying the separation model files (phase 5.1b). The files are derivative works of the
// official htdemucs checkpoints: never committed, never bundled, fetched and exported on the user's
// machine by `match/.venv/bin/sawblade-models fetch`. Load-time only (I/O, hashing, allocation).
namespace sawblade {

enum class SeparationModel {
  Htdemucs6s,  // default: drums, bass, vocals, guitar, piano, other
  Htdemucs4s,  // fallback: drums, bass, vocals, other
};

// "htdemucs_6s" / "htdemucs": the id used in file names, cache keys and the fetch command.
const char* separationModelId(SeparationModel m) noexcept;
// Number of sources the network emits: 6 or 4.
int separationModelSourceCount(SeparationModel m) noexcept;
// Pinned sha256 of the ONNX file the project's exporter produced (hex, lowercase).
const char* separationModelPinnedSha256(SeparationModel m) noexcept;
// The exact command that makes the model available, run from the repository root:
// "match/.venv/bin/sawblade-models fetch --model htdemucs_6s".
std::string separationModelFetchCommand(SeparationModel m);

struct ModelStatus {
  enum class State { Ok, Missing, Mismatch, Unreadable };
  State state = State::Missing;
  SeparationModel model = SeparationModel::Htdemucs6s;
  std::filesystem::path path;       // <dir>/<id>-core-opset17.onnx
  std::string actualSha256;         // empty unless the file was hashed
  bool matchedPinned = false;       // Ok because the sha256 equals the pinned value
  bool matchedSidecar = false;      // Ok because it equals the <file>.sha256 sidecar (the fetch tool writes it
                                    // only after checking ORT against torch; other platforms may export other bytes)
  std::string message;              // human readable; for a non-Ok state it ends with the fetch command
  bool ok() const noexcept { return state == State::Ok; }
};

// Model directory: $SAWBLADE_MODELS_DIR, else macOS ~/Library/Application Support/Sawblade/models, else
// $XDG_DATA_HOME/sawblade/models or ~/.local/share/sawblade/models. Shared with the Python fetch tool.
std::filesystem::path defaultModelsDirectory();
// Stem cache directory: $SAWBLADE_STEMS_DIR, else the sibling `stems` of the models directory
// (so ~/.local/share/sawblade/stems).
std::filesystem::path defaultStemsDirectory();

class ModelStore {
 public:
  explicit ModelStore(std::filesystem::path dir = defaultModelsDirectory()) : dir_(std::move(dir)) {}
  const std::filesystem::path& directory() const noexcept { return dir_; }
  // <dir>/<id>-core-opset17.onnx
  std::filesystem::path modelPath(SeparationModel m) const;
  // Hashes the file (115-175 MB: a second or so; cached in-process by path + size + mtime) and checks it
  // against the pinned value or its sidecar. Never throws.
  ModelStatus check(SeparationModel m) const;

 private:
  std::filesystem::path dir_;
};

}  // namespace sawblade
