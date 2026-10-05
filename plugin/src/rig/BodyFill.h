#pragma once

// v0.2 Task C: the asynchronous half of "BLEND fills path B with a suggested body path". RigController applies the immediate
// part (TS boost + the fallback amp if it is cached) in one edit and calls begin(); BodyFill then asks
// `sawblade-t3k suggest-body --a-title <path A amp> --cache-dir <cache> --json` (offline) on a background thread and, if that
// names a different amp and path B is exactly what the first edit made (nothing edited since), swaps the amp in (fetching its
// model with `sawblade-t3k fetch` if it is not cached). No suggestion (tool missing, `null`, error): the fallback amp, fetched
// if it is not cached. Every step is a tool run through T3kTool, one at a time; nothing runs when no tool is configured.
// The swap does not add an undo entry: one Undo of the BLEND edit restores the pre-BLEND preset either way.
// Threading: begin() / tick() / cancel() on the message thread; tool completions are queued and applied by tick().

#include <chrono>
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
  std::uint64_t toolRuns() const noexcept { return runs_; }
  // Tests: blocks until the tool run in flight (if any) has finished; its result still waits for tick().
  bool waitToolIdle(std::chrono::milliseconds timeout);

 private:
  enum class Step { Idle, Suggest, FetchSuggested, FetchFallback };
  struct Done {
    std::uint64_t run;
    bool ok;
    std::string output;
  };
  void start(Step s, std::vector<std::string> args);
  void fallback();
  void applyAmp(const Capture& model);
  bool bodyUntouched(const Preset& cur) const;

  SawbladeProcessor& proc_;
  T3kTool tool_;
  Step step_ = Step::Idle;
  std::vector<Block> expectedB_;  // path B as the last BodyFill edit left it
  std::string aTitle_;
  std::uint64_t run_ = 0, runs_ = 0;
  std::mutex m_;
  std::vector<Done> done_;
};

}  // namespace sawblade::plugin::rig
