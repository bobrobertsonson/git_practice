#pragma once

// Runs the matcher (`sawblade-match`) and the NAM exporter (`sawblade-export`) as child processes for the
// plugin (docs/specs/phase6a_record_match_plugin.md). The runner lives in the processor, not the editor, so a
// job survives the panel closing; the job directory (<app data>/jobs/<timestamp>-match|export, with job.json)
// is the source of truth, and a runner that finds a running job on disk re-attaches to it.
//
// Nothing here blocks the message thread: every job has its own background threads (a launcher/monitor and a
// reader that drains the child's stdout/stderr pipe so it never fills), and the message thread only copies a
// snapshot. The audio thread never touches this.
//
// Cancel = SIGTERM, then SIGKILL (juce::ChildProcess::kill) after a short grace period; the job is then marked
// `cancelled` in job.json.
//
// POSIX note: juce::ChildProcess does not expose the child's pid, so the child is started through
// `/bin/sh -c 'echo $$; exec "$0" "$@"' <exe> <args...>`: the shell prints its pid (which `exec` keeps) as the
// first output line, and the reader strips it. That pid is what job.json stores and what a re-attached runner
// polls and signals.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <juce_data_structures/juce_data_structures.h>

namespace sawblade::plugin {

// Settings that are not part of the tone: they live in juce::PropertiesFile (application properties), never in
// the preset or the plugin state.
class MatchSettings {
 public:
  // The file is opened on first use (nothing is read while a plugin is merely instantiated).
  explicit MatchSettings(const std::filesystem::path& file);
  ~MatchSettings();
  void setFile(const std::filesystem::path& file);  // tests: a temp file

  std::filesystem::path matchExecutable() const;   // default <repo>/match/.venv/bin/sawblade-match
  std::filesystem::path exportExecutable() const;  // default <repo>/match/.venv/bin/sawblade-export
  std::filesystem::path poolManifest() const;      // default ~/.cache/sawblade/captures/pool_manifest.json
  std::string selectedTake() const;                // take name chosen for MATCH ("" = none)
  void setMatchExecutable(const std::filesystem::path& p);
  void setExportExecutable(const std::filesystem::path& p);
  void setPoolManifest(const std::filesystem::path& p);
  void setSelectedTake(const std::string& name);

  static std::filesystem::path defaultMatchExecutable();
  static std::filesystem::path defaultExportExecutable();
  static std::filesystem::path defaultPoolManifest();

 private:
  juce::PropertiesFile& props() const;
  std::filesystem::path file_;
  mutable std::unique_ptr<juce::PropertiesFile> props_;
};

enum class JobKind { Match, Export };
enum class JobState { None, Starting, Running, Succeeded, Failed, Cancelled };
const char* jobKindName(JobKind k);
const char* jobStateName(JobState s);

struct JobProgress {
  std::string stage, message;
  double fraction = -1.0;       // 0..1, < 0 = indeterminate
  double etaSeconds = -1.0;     // < 0 = unknown
  std::optional<double> bestErrorDb;
};

// One row of the match results (best first).
struct MatchCandidate {
  int rank = 0;                 // 1 = the matcher's choice
  double errorDb = 0.0;         // result.json `loss`, shown as error dB
  std::string topology;         // blend / single / single2
  std::string captures;         // one-line summary of the captures
  double blend = 0.0;
  std::filesystem::path preset; // resolved preset JSON
  bool presetExists = false;
  std::string name() const { return rank == 1 ? "best" : "alt" + std::to_string(rank - 1); }
};

struct JobSnapshot {
  JobKind kind = JobKind::Match;
  JobState state = JobState::None;
  std::filesystem::path dir;          // job directory
  std::string message;                // why it failed / what happened ("" while fine)
  JobProgress progress;
  bool progressJson = false;          // progress comes from --progress-json (else from the log lines)
  double elapsedSeconds = 0.0;
  std::int64_t pid = 0;
  int exitCode = -1;
  std::vector<std::string> logTail;   // last lines of the child's output
  std::string reference, di;          // match: what it was started with (for the screen)
  std::vector<MatchCandidate> results;  // match, once succeeded
  std::filesystem::path outDir;       // export: the result folder (revealed in the file manager)
  std::string exportMode, exportSize;
  bool active() const { return state == JobState::Starting || state == JobState::Running; }
};

struct MatchRequest {
  std::filesystem::path di;            // DI take WAV
  std::filesystem::path ref;           // reference audio (a stem file)
  std::optional<double> offsetMs;      // matcher sign; none = search
  std::string referenceLabel, diLabel; // for the screen
};
struct ExportRequest {
  std::filesystem::path preset;        // resolved preset JSON
  std::string mode = "nocab";          // nocab / withcab
  std::string size = "standard";       // feather / lite / standard
  std::optional<std::filesystem::path> di;  // validation DI (the selected take)
};

// What the user must locate before a job can start.
struct ToolCheck {
  enum class Missing { None, Executable, Pool };
  Missing missing = Missing::None;
  std::string message;                 // clear text for the screen
  bool ok() const { return missing == Missing::None; }
};

// Pure helpers (unit-tested).
bool parseProgressJson(const std::string& text, JobProgress& out);       // tolerant: false on partial / bad JSON
void parseLogLine(JobKind kind, const std::string& line, JobProgress& p); // the log-line fallback
bool parseExportProgress(const std::string& text, JobProgress& out);     // <out>/checkpoint/progress.json
std::vector<MatchCandidate> parseMatchResult(const std::filesystem::path& resultJson, std::string* error = nullptr);

// The reference file for a song folder: the guitar stem, else `other`, else a mix / the first audio file.
struct ReferenceChoice {
  std::filesystem::path file;
  std::string label;                   // e.g. "guitar stem: guitar.wav"
  bool found = false;
};
ReferenceChoice chooseReferenceFile(const std::filesystem::path& songFolder);

class JobRunner {
 public:
  JobRunner(MatchSettings& settings, const std::filesystem::path& jobsDir);
  ~JobRunner();  // stops monitoring; never kills a running child (the job dir lets the next runner re-attach)
  JobRunner(const JobRunner&) = delete;
  JobRunner& operator=(const JobRunner&) = delete;

  void setJobsDir(const std::filesystem::path& dir);
  std::filesystem::path jobsDir() const;
  // Adopts the newest match and export job found on disk (a running one is monitored again, a finished one is
  // loaded). Cheap; call it when a panel opens. Does nothing for a kind that already has a job.
  void attachExisting();

  ToolCheck checkTools(JobKind kind) const;
  // False (and *error set) if a tool is missing or a job of this kind is already running.
  bool startMatch(const MatchRequest& r, std::string* error = nullptr);
  bool startExport(const ExportRequest& r, std::string* error = nullptr);
  void cancel(JobKind kind);

  JobSnapshot snapshot(JobKind kind) const;
  // Blocks (tests) until the job of `kind` is no longer active.
  bool waitFinished(JobKind kind, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));

  // Cancel = SIGTERM, then SIGKILL after this grace period (default 2.5 s).
  void setCancelGrace(std::chrono::milliseconds g) { graceMs_.store(g.count()); }

 private:
  struct Job;
  struct HelpCache;
  bool launch(JobKind kind, std::shared_ptr<Job> job, std::string* error);
  std::shared_ptr<Job>& slot(JobKind k) { return k == JobKind::Match ? match_ : export_; }
  const std::shared_ptr<Job>& slot(JobKind k) const { return k == JobKind::Match ? match_ : export_; }
  void adopt(JobKind kind, const std::filesystem::path& dir);
  void retire(std::shared_ptr<Job>& j);
  static void finalizeJob(Job& job);
  static void monitorAttached(std::shared_ptr<Job> job);

  MatchSettings& settings_;
  mutable std::mutex m_;                       // jobsDir_, slots
  std::filesystem::path jobsDir_;
  std::shared_ptr<Job> match_, export_;
  std::shared_ptr<HelpCache> help_;
  std::atomic<std::chrono::milliseconds::rep> graceMs_{2500};
};

}  // namespace sawblade::plugin
