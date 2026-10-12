#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "../PluginProcessor.h"
#include "PresetLibrary.h"
#include "T3kTool.h"

// Loading a preset from the library (docs/specs/phase9b_preset_browser.md section 3): a preset with a TONE3000 capture whose
// file is missing (and not in the capture cache) is resolved first with `sawblade-t3k resolve`, then the resolved file is loaded.
// The current sound never changes until the preset to load has loaded.
namespace sawblade::plugin {

struct LoadPlan {
  enum class Kind { Direct, UseResolved, NeedsResolve, Invalid } kind = Kind::Direct;
  std::filesystem::path load;         // Direct: the preset; UseResolved: the resolved file; NeedsResolve: the preset to resolve
  std::filesystem::path resolvedOut;  // <appdata>/resolved/<bank>/<stem>.resolved.json
  std::string error;                  // Invalid: the parse error
};

// `resolvedRoot` defaults to <appdata>/resolved. A resolved file is used directly when it exists, is newer than the preset
// and all its capture files exist. A cache hit counts as present (Capture cache fallback in the core loader).
LoadPlan planPresetLoad(const std::filesystem::path& presetFile, SubBank bank, const std::filesystem::path& resolvedRoot = {});

class PresetLoadFlow {
 public:
  struct Outcome {
    enum class Status { Loaded, NotLoggedIn, MissingExecutable, Failed, Cancelled, Invalid } status = Status::Failed;
    std::string message;                 // for the user ("" when Loaded)
    std::filesystem::path file;          // the preset that was asked for
    std::filesystem::path loadedFile;    // what the processor loaded (the preset or its resolved file)
  };
  // All callbacks run on the message thread (through `post`, by default MessageManager::callAsync).
  struct Callbacks {
    std::function<void(int done, int total, const std::string& title)> onProgress;
    std::function<void(const Outcome&)> onFinished;
  };
  using Post = std::function<void(std::function<void()>)>;

  PresetLoadFlow(SawbladeProcessor& p, Callbacks cb, Post post = {});
  ~PresetLoadFlow();

  // Message thread. Does nothing (and returns false) while a resolve is running.
  bool load(const std::filesystem::path& presetFile, SubBank bank);
  void cancel() { tool_.cancel(); }
  bool resolving() const { return resolving_; }

 private:
  void finish(Outcome o);
  SawbladeProcessor& proc_;
  Callbacks cb_;
  Post post_;
  T3kTool tool_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  bool resolving_ = false;
};

}  // namespace sawblade::plugin
