#pragma once

// v0.2 Task C: the asynchronous half of "BLEND fills path B with a suggested body path". RigController applies the immediate
// part (TS boost + the fallback amp if it is cached) in one edit and calls begin(); BodyFill then asks
// `sawblade-t3k suggest-body --a-title <path A amp> --cache-dir <cache> --json` (offline) on a background thread and, if that
// names a different amp and path B is exactly what the first edit made (nothing edited since), swaps the amp in (fetching its
// model with `sawblade-t3k fetch` if it is not cached). No suggestion (tool missing, `null`, error): the fallback amp, fetched
// if it is not cached. Every step is a tool run through T3kTool, one at a time; nothing runs when no tool is configured.
// The swap does not add an undo step (v0.3 Task D): it belongs to the BLEND edit's step, and the history's stored snapshots of the fill
// get the amp too (SawbladeProcessor::patchHistory), so one Undo of the BLEND edit restores the pre-BLEND preset either way.
// Never silent (v0.3 Task C): while path B has no amp, status() says what is happening (Downloading, with the amp's name) or why it
// failed (Failed: not logged in / network / no capture / tool missing ...). The amp head shows it; a fill that cannot start (no tool,
// network tools off) is Failed from begin(), not an empty return. The swap is applied while path B's STRUCTURE is what the fill made:
// the parameter-backed values (BLEND, LEVEL, the amp knobs) moving meanwhile do not count as an edit.
// Threading: begin() / tick() / cancel() on the message thread; tool completions are queued and applied by tick(). tick() must be
// called regularly whether or not the rig editor is open (the editor's refresh calls RigController::sync()).

#include <chrono>
#include <functional>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "PluginProcessor.h"
#include "presets/T3kTool.h"

namespace sawblade::plugin::rig {

// The tool's answers, parsed (pure; tested).
struct BodySuggestion {
  std::string toneId, modelId, title;
  bool cached = false;
};
// `null` or garbage: nullopt. Ids are strings.
std::optional<BodySuggestion> parseSuggestBody(const std::string& output);
// The capture a `fetch --json` answer describes (absolute path, sha256, source); nullopt if it is not one.
std::optional<Capture> parseFetchedCapture(const std::string& output);

// What the body head says about the fill (message thread).
enum class FillReason { None, NoTool, NetworkOff, NotLoggedIn, Network, NoCapture, License, Other };
struct FillStatus {
  enum class Kind { Idle, Downloading, Failed } kind = Kind::Idle;
  std::string name;  // Downloading: the amp being fetched (its title)
  FillReason reason = FillReason::None;
  std::string detail;  // Failed: the tool's message (for the log / tooltip)
};

class BodyFill {
 public:
  explicit BodyFill(SawbladeProcessor& p);
  ~BodyFill();
  BodyFill(const BodyFill&) = delete;
  BodyFill& operator=(const BodyFill&) = delete;

  // `applied` is the preset the BLEND edit submitted (path B = what the edit made). Replaces a run in progress.
  void begin(const Preset& applied);
  void tick();
  void cancel();
  bool active() const noexcept { return step_ != Step::Idle; }
  const FillStatus& status() const noexcept { return status_; }
  // Start the fill again from path B as it is now (after a failure: the user fixed the cause and touched BLEND).
  void retry();
  std::uint64_t toolRuns() const noexcept { return runs_; }
  // Tests: blocks until the tool run in flight (if any) has finished; its result still waits for tick().
  bool waitToolIdle(std::chrono::milliseconds timeout);

 private:
  enum class Step { Idle, Suggest, FetchSuggested, FetchFallback };
  struct Done {
    std::uint64_t run;
    bool ok;
    T3kTool::Status status;
    std::string message, output;
  };
  void fail(FillReason r, std::string detail);
  void failFrom(const Done& d, FillReason dflt);
  void start(Step s, std::vector<std::string> args);
  void fallback();
  void applyAmp(const Capture& model);
  bool bodyUntouched(const Preset& cur) const;
  void launch(Step s, std::vector<std::string> args);

  SawbladeProcessor& proc_;
  Step step_ = Step::Idle;
  PathPreset expectedB_;  // path B (blocks, controls, EQ, level ...) and the blend as the last BodyFill edit left them
  double expectedBlend_ = 0.0;
  std::optional<std::pair<Step, std::vector<std::string>>> queued_;  // a step waiting for the tool to finish
  std::string aTitle_;
  FillStatus status_;
  std::uint64_t run_ = 0, runs_ = 0;
  std::mutex m_;
  std::vector<Done> done_;
  T3kTool tool_;  // LAST: destroyed first (cancels and joins), so its completion callback never meets a destroyed m_ / done_
};

}  // namespace sawblade::plugin::rig
