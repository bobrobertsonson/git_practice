#include "JobRunner.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <regex>
#include <sstream>

#include <nlohmann/json.hpp>

#include "AppPaths.h"
#include "Sha256.h"

#if !JUCE_WINDOWS
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !JUCE_WINDOWS
#if defined(__APPLE__)
#include <crt_externs.h>
#define SAWBLADE_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define SAWBLADE_ENVIRON environ
#endif
#endif

#ifndef SAWBLADE_SOURCE_DIR
#define SAWBLADE_SOURCE_DIR ""
#endif

namespace sawblade::plugin {
namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kLogTailLines = 40;
constexpr auto kPollPeriod = 100ms;

std::int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string utcIso(std::int64_t ms) {
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
#if JUCE_WINDOWS
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void writeAtomic(const fs::path& p, const std::string& text) {
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << text;
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
}

bool isExecutableFile(const fs::path& p) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return false;
#if JUCE_WINDOWS
  return true;
#else
  return ::access(p.c_str(), X_OK) == 0;
#endif
}

// When did process `pid` start (epoch ms)? -1 if unknown.
std::int64_t processStartMs(std::int64_t pid) {
#if JUCE_WINDOWS
  (void)pid;
  return -1;
#elif defined(__APPLE__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
  struct kinfo_proc kp {};
  std::size_t len = sizeof kp;
  if (::sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len == 0) return -1;
  const auto& t = kp.kp_proc.p_starttime;
  return static_cast<std::int64_t>(t.tv_sec) * 1000 + t.tv_usec / 1000;
#else
  std::ifstream st("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(st, line)) return -1;
  const auto close = line.rfind(')');  // the command name may contain spaces and parentheses
  if (close == std::string::npos) return -1;
  std::istringstream rest(line.substr(close + 2));
  std::string field;
  long long startTicks = -1;
  for (int i = 3; i <= 22 && (rest >> field); ++i)  // fields after "pid (comm)" start at 3 (state)
    if (i == 22) startTicks = std::atoll(field.c_str());
  if (startTicks < 0) return -1;
  std::ifstream stat("/proc/stat");
  long long btime = -1;
  while (std::getline(stat, line))
    if (line.rfind("btime ", 0) == 0) btime = std::atoll(line.c_str() + 6);
  const long hz = ::sysconf(_SC_CLK_TCK);
  if (btime < 0 || hz <= 0) return -1;
  return btime * 1000 + startTicks * 1000 / hz;
#endif
}

// Is `pid` still the tool this job.json describes, and alive? A child of this process is waited for (so a finished
// one is reaped) and needs no identity check. For any other pid the process group must be the recorded one and the
// start time must match the moment the tool was spawned: a pid that was reused by an unrelated process fails
// this, and such a process is never signalled.
bool jobProcessAlive(std::int64_t pid, std::int64_t pgid, std::int64_t spawnedMs) {
#if JUCE_WINDOWS
  (void)pid;
  (void)pgid;
  (void)spawnedMs;
  return false;
#else
  if (pid <= 1) return false;
  int status = 0;
  const pid_t r = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
  if (r == static_cast<pid_t>(pid)) return false;
  if (r == 0) return true;  // ours, running
  if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM) return false;
  if (::getpgid(static_cast<pid_t>(pid)) != static_cast<pid_t>(pgid)) return false;
  const std::int64_t started = processStartMs(pid);
  constexpr std::int64_t kToleranceMs = 4000;  // /proc btime is whole seconds
  return started >= 0 && spawnedMs > 0 && std::llabs(started - spawnedMs) <= kToleranceMs;
#endif
}

// The tool runs as the leader of its own process group (pgid == pid), so this reaches its helper processes too.
enum class Sig { Int, Term, Kill };
void signalGroup(std::int64_t pgid, Sig sig) {
#if JUCE_WINDOWS
  (void)pgid;
  (void)sig;
#else
  if (pgid > 1) ::kill(-static_cast<pid_t>(pgid), sig == Sig::Kill ? SIGKILL : sig == Sig::Term ? SIGTERM : SIGINT);
#endif
}

std::string trim(std::string s) {
  const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

fs::path homeDir() {
  const char* h = std::getenv("HOME");
  return (h != nullptr && *h != '\0') ? fs::path(h) : fs::path("/tmp");
}

double num(const json& j, const char* key, double def) {
  if (auto it = j.find(key); it != j.end() && it->is_number()) return it->get<double>();
  return def;
}

}  // namespace

const char* jobKindName(JobKind k) { return k == JobKind::Match ? "match" : "export"; }
const char* jobStateName(JobState s) {
  switch (s) {
    case JobState::None: return "none";
    case JobState::Starting: return "starting";
    case JobState::Running: return "running";
    case JobState::Succeeded: return "succeeded";
    case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
  }
  return "none";
}

// ---- settings ----------------------------------------------------------------------------------------------------
fs::path MatchSettings::defaultMatchExecutable() { return fs::path(SAWBLADE_SOURCE_DIR) / "match" / ".venv" / "bin" / "sawblade-match"; }
fs::path MatchSettings::defaultExportExecutable() { return fs::path(SAWBLADE_SOURCE_DIR) / "match" / ".venv" / "bin" / "sawblade-export"; }
fs::path MatchSettings::defaultPoolManifest() { return homeDir() / ".cache" / "sawblade" / "captures" / "pool_manifest.json"; }

MatchSettings::MatchSettings(const fs::path& file) : file_(file) {}
MatchSettings::~MatchSettings() = default;

void MatchSettings::setFile(const fs::path& file) {
  std::lock_guard<std::mutex> lk(m_);
  file_ = file;
  props_.reset();
}

std::shared_ptr<juce::PropertiesFile> MatchSettings::props() const {
  // The caller keeps the file alive: a concurrent setFile() only replaces the member.
  std::lock_guard<std::mutex> lk(m_);
  if (!props_) {
    juce::PropertiesFile::Options o;
    o.applicationName = "Sawblade";
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    o.millisecondsBeforeSaving = 0;
    props_ = std::make_shared<juce::PropertiesFile>(juce::File(file_.string()), o);
  }
  return props_;
}

namespace {
fs::path pathSetting(juce::PropertiesFile& p, const char* key, const fs::path& def) {
  const juce::String v = p.getValue(key, juce::String());
  return v.isEmpty() ? def : fs::path(v.toStdString());
}
void saveSetting(juce::PropertiesFile& p, const char* key, const juce::String& value) {
  p.getFile().getParentDirectory().createDirectory();
  p.setValue(key, value);
  p.saveIfNeeded();
}
}  // namespace

fs::path MatchSettings::matchExecutable() const { return pathSetting(*props(), "matchExecutable", defaultMatchExecutable()); }
fs::path MatchSettings::exportExecutable() const { return pathSetting(*props(), "exportExecutable", defaultExportExecutable()); }
fs::path MatchSettings::poolManifest() const { return pathSetting(*props(), "poolManifest", defaultPoolManifest()); }
std::string MatchSettings::selectedTake() const { return props()->getValue("selectedTake", juce::String()).toStdString(); }

double MatchSettings::exportWallSeconds(const std::string& size) const {
  return props()->getDoubleValue(juce::String("exportWallSeconds.") + juce::String(size), 0.0);
}
void MatchSettings::setExportWallSeconds(const std::string& size, double seconds) {
  saveSetting(*props(), (std::string("exportWallSeconds.") + size).c_str(), juce::String(seconds, 1));
}

void MatchSettings::setMatchExecutable(const fs::path& p) { saveSetting(*props(), "matchExecutable", juce::String(p.string())); }
void MatchSettings::setExportExecutable(const fs::path& p) { saveSetting(*props(), "exportExecutable", juce::String(p.string())); }
void MatchSettings::setPoolManifest(const fs::path& p) { saveSetting(*props(), "poolManifest", juce::String(p.string())); }
void MatchSettings::setSelectedTake(const std::string& name) { saveSetting(*props(), "selectedTake", juce::String(name)); }

// ---- pure helpers -------------------------------------------------------------------------------------------------
bool parseProgressJson(const std::string& text, JobProgress& out) {
  const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return false;
  JobProgress p;
  if (auto it = j.find("stage"); it != j.end() && it->is_string()) p.stage = it->get<std::string>();
  if (auto it = j.find("message"); it != j.end() && it->is_string()) p.message = it->get<std::string>();
  p.fraction = num(j, "fraction", -1.0);
  if (p.fraction > 1.0) p.fraction = 1.0;
  p.etaSeconds = num(j, "etaSeconds", -1.0);
  if (auto it = j.find("bestErrorDb"); it != j.end() && it->is_number()) p.bestErrorDb = it->get<double>();
  // sawblade-export's --progress-json (phase 12): epoch, epochs, bestEsr (null before the first epoch), outDir, resumable.
  p.epoch = static_cast<int>(num(j, "epoch", 0.0));
  p.epochs = static_cast<int>(num(j, "epochs", 0.0));
  if (auto it = j.find("bestEsr"); it != j.end() && it->is_number()) p.bestEsr = it->get<double>();
  if (auto it = j.find("resumable"); it != j.end() && it->is_boolean()) p.resumable = it->get<bool>();
  if (auto it = j.find("outDir"); it != j.end() && it->is_string()) p.outDir = it->get<std::string>();
  out = std::move(p);
  return true;
}

bool parseExportProgress(const std::string& text, JobProgress& out) {
  const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return false;
  JobProgress p;
  p.stage = "training";
  const double epoch = num(j, "epoch", 0.0), elapsed = num(j, "elapsedTrainingS", 0.0);
  double epochs = 0.0, maxMin = 0.0;
  if (auto c = j.find("config"); c != j.end() && c->is_object()) {
    epochs = num(*c, "epochs", 0.0);
    maxMin = num(*c, "maxMinutes", 0.0);
  }
  double f = -1.0;
  if (epochs > 0.0) f = epoch / epochs;
  if (maxMin > 0.0) f = std::max(f, elapsed / (maxMin * 60.0));
  if (f >= 0.0) p.fraction = std::min(f, 0.99);
  if (p.fraction > 0.02 && elapsed > 0.0) p.etaSeconds = elapsed * (1.0 - p.fraction) / p.fraction;
  p.epoch = static_cast<int>(epoch);
  p.epochs = static_cast<int>(epochs);
  p.resumable = true;
  std::ostringstream m;
  m << "epoch " << static_cast<int>(epoch);
  if (auto it = j.find("bestValEsr"); it != j.end() && it->is_number()) {
    p.bestEsr = it->get<double>();
    m << ", best val ESR " << it->get<double>();
  }
  p.message = m.str();
  out = std::move(p);
  return true;
}

CheckpointInfo readCheckpoint(const fs::path& outDir) {
  CheckpointInfo c;
  if (outDir.empty()) return c;
  const json j = json::parse(readFile(outDir / "checkpoint" / "progress.json"), nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return c;
  if (auto it = j.find("complete"); it != j.end() && it->is_boolean() && it->get<bool>()) return c;
  c.resumable = true;
  c.epoch = static_cast<int>(num(j, "epoch", 0.0));
  if (auto cfg = j.find("config"); cfg != j.end() && cfg->is_object()) c.epochs = static_cast<int>(num(*cfg, "epochs", 0.0));
  return c;
}

ExportResult readExportResult(const fs::path& outDir) {
  ExportResult r;
  if (outDir.empty()) return r;
  std::error_code ec;
  const json j = json::parse(readFile(outDir / "export_report.json"), nullptr, /*allow_exceptions=*/false);
  if (j.is_object()) {
    r.haveReport = true;
    r.nonCommercial = j.value("nonCommercial", false);
    r.wallSeconds = num(j, "totalWallSeconds", 0.0);
    if (auto t = j.find("training"); t != j.end() && t->is_object()) {
      if (r.wallSeconds <= 0.0) r.wallSeconds = num(*t, "wallSeconds", 0.0);
      if (auto f = t->find("namFile"); f != t->end() && f->is_string()) r.namFile = f->get<std::string>();
    }
    std::string status = "not judged";
    if (auto v = j.find("validation"); v != j.end() && v->is_object())
      if (auto a = v->find("acceptance"); a != v->end() && a->is_object()) {
        if (auto it = a->find("status"); it != a->end() && it->is_string()) status = it->get<std::string>();
        if (auto it = a->find("summary"); it != a->end() && it->is_string()) r.summary = it->get<std::string>();
        auto opt = [&](const char* k, std::optional<double>& dst) {
          if (auto it = a->find(k); it != a->end() && it->is_number()) dst = it->get<double>();
        };
        opt("heldOutEsr", r.heldOutEsr);
        opt("diLtasDb", r.diLtasDb);
        opt("esrLimit", r.esrLimit);
        opt("ltasLimitDb", r.ltasLimitDb);
      }
    const std::string l = lower(trim(status));
    r.status = l.rfind("not met", 0) == 0 ? "NOT MET" : l.rfind("met", 0) == 0 ? "MET" : "NOT JUDGED";
  }
  if (r.namFile.empty() || !fs::exists(outDir / r.namFile, ec)) {
    r.namFile.clear();
    for (fs::directory_iterator it(outDir, ec), end; !ec && it != end; it.increment(ec))
      if (it->path().extension() == ".nam") r.namFile = it->path().filename().string();
  }
  for (const char* ext : {".mp3", ".wav"}) {
    const fs::path f = outDir / "listen" / (std::string("ab_original_then_export") + ext);
    if (fs::is_regular_file(f, ec)) {
      r.listen = f;
      break;
    }
  }
  return r;
}

void parseLogLine(JobKind kind, const std::string& rawLine, JobProgress& p) {
  std::string line = trim(rawLine);
  if (line.empty()) return;
  // The matcher prefixes its lines with "[   12.3s] ".
  static const std::regex prefix(R"(^\[\s*[0-9.]+s\]\s*)");
  line = std::regex_replace(line, prefix, "");
  if (line.empty()) return;
  p.message = line;
  const std::string l = lower(line);
  if (kind == JobKind::Match) {
    if (l.find("stage3") != std::string::npos || l.find("stage 3") != std::string::npos || l.rfind("selected:", 0) == 0) p.stage = "stage 3: verifying";
    else if (l.find("stage2") != std::string::npos || l.find("stage 2") != std::string::npos) p.stage = "stage 2: fine-tuning";
    else if (l.find("stage1") != std::string::npos || l.find("stage 1") != std::string::npos) p.stage = "stage 1: screening";
    else if (l.rfind("done in", 0) == 0) p.stage = "done";
    else if (p.stage.empty()) p.stage = "preparing";
  } else {
    if (l.find("epoch") != std::string::npos) p.stage = "training";
    else if (l.rfind("trained in", 0) == 0 || l.find("validat") != std::string::npos) p.stage = "validating";
    else if (p.stage.empty()) p.stage = "preparing";
  }
  p.fraction = -1.0;  // the log carries no fraction: indeterminate
}

namespace {
std::string captureTitle(const json& c) {
  if (!c.is_object()) return {};
  if (auto it = c.find("title"); it != c.end() && it->is_string()) return it->get<std::string>();
  if (auto it = c.find("model"); it != c.end() && it->is_string()) return it->get<std::string>();
  return {};
}

std::string summarise(const std::string& topology, const json& caps) {
  if (!caps.is_object()) return {};
  auto get = [&](const char* k) {
    auto it = caps.find(k);
    return it == caps.end() ? std::string() : captureTitle(*it);
  };
  auto join = [](std::initializer_list<std::string> parts, const char* sep) {
    std::string out;
    for (const auto& p : parts) {
      if (p.empty()) continue;
      if (!out.empty()) out += sep;
      out += p;
    }
    return out;
  };
  if (topology == "blend") {
    const std::string a = join({get("a_pedal"), get("a_amp")}, " + "), b = join({get("b_pedal"), get("b_amp")}, " + ");
    return join({a.empty() ? "" : "SAW " + a, b.empty() ? "" : "BODY " + b, get("cab")}, " | ");
  }
  return join({join({get("pedal"), get("pedal1"), get("pedal2"), get("amp")}, " + "), get("cab")}, " | ");
}

MatchCandidate candidateFrom(const json& j, int rank, const fs::path& dir, const char* fileKey) {
  MatchCandidate c;
  c.rank = rank;
  c.errorDb = num(j, "loss", 0.0);
  c.topology = j.value("topology", std::string());
  c.blend = num(j, "blend", 0.0);
  if (auto it = j.find("captures"); it != j.end()) c.captures = summarise(c.topology, *it);
  if (auto it = j.find(fileKey); it != j.end() && it->is_string()) {
    fs::path p = it->get<std::string>();
    c.preset = p.is_absolute() ? p : dir / p;
    std::error_code ec;
    c.presetExists = fs::is_regular_file(c.preset, ec);
  }
  return c;
}
}  // namespace

std::vector<MatchCandidate> parseMatchResult(const fs::path& resultJson, std::string* error) {
  std::vector<MatchCandidate> out;
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return std::vector<MatchCandidate>{};
  };
  const std::string text = readFile(resultJson);
  if (text.empty()) return fail("result.json is missing");
  const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return fail("result.json is not valid JSON");
  const fs::path dir = resultJson.parent_path();
  auto best = j.find("best");
  if (best == j.end() || !best->is_object()) return fail("result.json has no best candidate");
  out.push_back(candidateFrom(*best, 1, dir, "preset"));
  if (auto alts = j.find("alternatives"); alts != j.end() && alts->is_array()) {
    int rank = 2;
    for (const auto& a : *alts)
      if (a.is_object()) out.push_back(candidateFrom(a, rank++, dir, "file"));
  }
  return out;
}

ReferenceChoice chooseReferenceFile(const fs::path& folder) {
  ReferenceChoice r;
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) return r;
  std::vector<fs::path> files;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string e = lower(it->path().extension().string());
    if (e == ".wav" || e == ".flac" || e == ".mp3" || e == ".aif" || e == ".aiff") files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  auto find = [&](std::initializer_list<const char*> stems) -> const fs::path* {
    for (const char* s : stems)
      for (const auto& f : files)
        if (lower(f.stem().string()) == s) return &f;
    return nullptr;
  };
  if (const fs::path* f = find({"guitar", "guitars"})) {
    r = {*f, "guitar stem: " + f->filename().string(), true};
  } else if (const fs::path* f2 = find({"other"})) {
    r = {*f2, "'other' stem (no guitar stem): " + f2->filename().string(), true};
  } else if (const fs::path* f3 = find({"mix", "mixture", "song", "full", "master"})) {
    r = {*f3, "full mix: " + f3->filename().string(), true};
  } else if (!files.empty()) {
    r = {files.front(), "first audio file (no guitar or mix found): " + files.front().filename().string(), true};
  }
  return r;
}

// ---- job ---------------------------------------------------------------------------------------------------------
struct JobRunner::HelpCache {
  std::mutex m;
  std::map<std::string, bool> listsProgressJson;
};

struct JobRunner::Job {
  JobKind kind = JobKind::Match;
  fs::path dir, outDir, progressFile;  // outDir: monitor thread only (export: "" until the exporter reports it)
  fs::path exportsRoot, sourcePreset;  // export
  MatchSettings* settings = nullptr;   // export: the wall time of a finished run is recorded there
  std::string exe;
  std::vector<std::string> args;          // after the executable
  bool wantProgressJson = false;          // match: probe `--help` for --progress-json
  std::shared_ptr<HelpCache> help;
  std::chrono::milliseconds grace{2500};

  mutable std::mutex m;      // snap
  mutable std::mutex fileM;  // job.json writes
  JobSnapshot snap;
  std::int64_t startedMs = 0;
  std::atomic<std::int64_t> finishedMs{0};  // written by the monitor, read by snapshot()
  bool progressFileSeen = false;          // snap.progressJson and the file has been read at least once
  std::vector<std::string> commandLine;

  std::atomic<bool> cancelRequested{false}, stopMonitoring{false}, owned{true};
  int cancelStep = 0;                      // monitor thread: how far the cancel has escalated
  std::chrono::steady_clock::time_point cancelAt;
  std::int64_t lastScanMs = 0;             // export: when the exports root was last searched for the run's folder
  std::atomic<std::int64_t> pid{0};
  std::atomic<std::int64_t> pgid{0};
  std::atomic<std::int64_t> spawnedMs{0};  // when the tool was spawned (identity check on re-attach)
  std::string logPartial;                 // an incomplete last line of log.txt
  std::thread monitor;
  std::int64_t logOffset = 0;             // how much of log.txt was read

  void pushLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(m);
    snap.logTail.push_back(line);
    if (snap.logTail.size() > kLogTailLines) snap.logTail.erase(snap.logTail.begin());
    if (!snap.progressJson || !progressFileSeen) {  // log lines also stand in until the first progress.json arrives
      JobProgress p = snap.progress;
      parseLogLine(kind, line, p);
      snap.progress = std::move(p);
    }
  }

  void setState(JobState s, const std::string& message = std::string()) {
    std::lock_guard<std::mutex> lk(m);
    snap.state = s;
    if (!message.empty()) snap.message = message;
  }
  // A final state: job.json is written first, so whoever sees the state in snapshot() finds the file up to date.
  void finish(JobState s, const std::string& message) {
    JobSnapshot fin;
    {
      std::lock_guard<std::mutex> lk(m);
      fin = snap;
    }
    fin.state = s;
    if (!message.empty()) fin.message = message;
    writeJobJson(&fin);
    setState(s, message);
  }

  // `state` overrides the snapshot's (so a final state can reach job.json before it is visible through snapshot()).
  void writeJobJson(const JobSnapshot* state = nullptr) const {
    json j;
    JobSnapshot s;
    if (state != nullptr) {
      s = *state;
    } else {
      std::lock_guard<std::mutex> lk(m);
      s = snap;
    }
    j["version"] = 1;
    j["kind"] = jobKindName(kind);
    j["state"] = jobStateName(s.state);
    j["pid"] = pid.load();
    j["pgid"] = pgid.load();
    j["spawnedEpochMs"] = spawnedMs.load();
    j["startedEpochMs"] = startedMs;
    j["startedUtc"] = utcIso(startedMs);
    if (finishedMs > 0) {
      j["finishedUtc"] = utcIso(finishedMs.load());
      j["finishedEpochMs"] = finishedMs.load();
    }
    j["exitCode"] = s.exitCode;
    j["commandLine"] = commandLine;
    j["jobDir"] = dir.string();
    j["outDir"] = (state != nullptr && !s.outDir.empty() ? s.outDir : outDir).string();
    j["progressMode"] = s.progressJson ? "json" : "log";
    j["message"] = s.message;
    j["reference"] = s.reference;
    j["di"] = s.di;
    j["exportMode"] = s.exportMode;
    j["exportSize"] = s.exportSize;
    if (kind == JobKind::Export) {
      j["sourcePreset"] = sourcePreset.string();
      j["sourceSha256"] = s.sourceSha256;
      j["exportsRoot"] = exportsRoot.string();
      j["allowInexact"] = s.allowInexact;
      j["diBuiltin"] = s.diBuiltin;
      j["accepted"] = s.accepted;
      j["resumable"] = s.resumable;
      j["sidecar"] = s.sidecar.string();
    }
    std::lock_guard<std::mutex> fl(fileM);
    writeAtomic(dir / "job.json", j.dump(2) + "\n");
  }

  void pollProgress() {
    if (kind == JobKind::Match) {
      bool useJson;
      {
        std::lock_guard<std::mutex> lk(m);
        useJson = snap.progressJson;
      }
      if (!useJson) return;
      JobProgress p;
      if (parseProgressJson(readFile(progressFile), p)) {
        std::lock_guard<std::mutex> lk(m);
        snap.progress = std::move(p);
        progressFileSeen = true;
      }
    } else {
      bool useJson;
      {
        std::lock_guard<std::mutex> lk(m);
        useJson = snap.progressJson;
      }
      JobProgress p;
      if (useJson) {
        // sawblade-export --progress-json: the whole snapshot, plus the final output folder.
        if (!parseProgressJson(readFile(progressFile), p)) return;
        const bool learned = !p.outDir.empty() && outDir != fs::path(p.outDir);
        if (!p.outDir.empty()) outDir = p.outDir;
        {
          std::lock_guard<std::mutex> lk(m);
          snap.progress = std::move(p);
          progressFileSeen = true;
          if (!outDir.empty()) snap.outDir = outDir;
        }
        if (learned) writeJobJson();  // job.json names the run folder as soon as it is known (a later runner re-attaches by it)
        return;
      }
      // Fallback (an exporter without --progress-json): the 4.1 checkpoint's progress.json, in the run's folder,
      // which is found by looking for the newest folder the exporter created in the exports root.
      if (outDir.empty()) locateOutDir(false);
      if (outDir.empty() || !parseExportProgress(readFile(outDir / "checkpoint" / "progress.json"), p)) return;
      std::lock_guard<std::mutex> lk(m);
      // Keep the log-derived message when the file carries nothing newer; the file is authoritative for numbers.
      snap.progress.fraction = p.fraction;
      snap.progress.etaSeconds = p.etaSeconds;
      snap.progress.bestErrorDb = p.bestErrorDb;
      snap.progress.stage = p.stage;
      snap.progress.message = p.message;
      snap.progress.epoch = p.epoch;
      snap.progress.epochs = p.epochs;
      snap.progress.bestEsr = p.bestEsr;
      snap.progress.resumable = true;
      snap.outDir = outDir;
    }
  }

  // Export without a reported output folder: the newest folder in the exports root that was created after the job
  // started (once a second unless `force`). Monitor thread.
  void locateOutDir(bool force) {
    if (exportsRoot.empty()) return;
    const std::int64_t now = nowMs();
    if (!force && now - lastScanMs < 1000) return;
    lastScanMs = now;
    std::error_code ec;
    fs::path best;
    long long bestTime = 0;
    for (fs::directory_iterator it(exportsRoot, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_directory(ec)) continue;
#if !JUCE_WINDOWS
      struct stat st {};
      if (::stat(it->path().c_str(), &st) != 0) continue;
      const long long mt = static_cast<long long>(st.st_mtime);
#else
      continue;
#endif
      if (mt * 1000 + 1999 < startedMs) continue;  // older than the job (whole-second timestamps)
      if (best.empty() || mt > bestTime || (mt == bestTime && it->path() > best)) {
        best = it->path();
        bestTime = mt;
      }
    }
    if (!best.empty()) {
      outDir = best;
      {
        std::lock_guard<std::mutex> lk(m);
        snap.outDir = best;
      }
      writeJobJson();
    }
  }

  // Cancel, escalating: a match job gets SIGTERM, then SIGKILL after the grace period; an export job gets SIGINT (the
  // exporter stops at the end of its batch and keeps the checkpoint), SIGTERM after the grace period, SIGKILL after
  // another. Monitor thread.
  void driveCancel(std::int64_t pg) {
    if (!cancelRequested) return;
    const auto now = std::chrono::steady_clock::now();
    const bool exp = kind == JobKind::Export;
    if (cancelStep == 0) {
      signalGroup(pg, exp ? Sig::Int : Sig::Term);
      cancelStep = 1;
      cancelAt = now;
      return;
    }
    if (now - cancelAt <= grace) return;
    const int last = exp ? 3 : 2;
    if (cancelStep >= last) return;
    signalGroup(pg, exp && cancelStep == 1 ? Sig::Term : Sig::Kill);
    ++cancelStep;
    cancelAt = now;
  }

  // The tool's stdout / stderr go straight to <job>/log.txt (no pipe, so nothing can fill and nothing needs a reader
  // thread, and a re-attached runner reads the same file). Complete lines only, until `flush`.
  void pollLogFile(bool flush = false) {
    std::ifstream f(dir / "log.txt", std::ios::binary);
    if (f) {
      f.seekg(0, std::ios::end);
      const std::int64_t size = f.tellg();
      if (size > logOffset) {
        f.seekg(logOffset);
        std::string chunk(static_cast<std::size_t>(size - logOffset), '\0');
        f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        logOffset = size;
        logPartial += chunk;
      }
    }
    std::size_t nl;
    while ((nl = logPartial.find('\n')) != std::string::npos) {
      std::string line = logPartial.substr(0, nl);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      logPartial.erase(0, nl + 1);
      pushLine(line);
    }
    if (flush && !logPartial.empty()) {
      pushLine(logPartial);
      logPartial.clear();
    }
  }
};

namespace {

// Does `exe --help` list --progress-json? Runs the program once per (path, mtime); called on a job thread.
bool probeProgressJson(const std::string& exe, std::atomic<bool>& stop) {
  juce::ChildProcess p;
  if (!p.start(juce::StringArray{juce::String(exe), "--help"}, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return false;
  // The help text is small (it fits the pipe), so waiting for exit first cannot deadlock.
  for (int waited = 0; waited < 600 && p.isRunning(); ++waited) {  // up to 60 s: Python + numpy start-up on a cold disk
    if (stop.load()) {
      p.kill();
      return false;
    }
    std::this_thread::sleep_for(100ms);
  }
  if (p.isRunning()) {
    p.kill();
    return false;
  }
  return p.readAllProcessOutput().contains("--progress-json");
}

}  // namespace

// ---- runner --------------------------------------------------------------------------------------------------------
JobRunner::JobRunner(MatchSettings& settings, const fs::path& jobsDir) : settings_(settings), jobsDir_(jobsDir), help_(std::make_shared<HelpCache>()) {}

JobRunner::~JobRunner() {
  std::shared_ptr<Job> a, b;
  {
    std::lock_guard<std::mutex> lk(m_);
    a = match_;
    b = export_;
  }
  for (auto* j : {&a, &b}) {
    if (!*j) continue;
    (*j)->stopMonitoring = true;
    if ((*j)->monitor.joinable()) (*j)->monitor.join();
  }
}

void JobRunner::setJobsDir(const fs::path& dir) {
  std::lock_guard<std::mutex> lk(m_);
  jobsDir_ = dir;
}
fs::path JobRunner::jobsDir() const {
  std::lock_guard<std::mutex> lk(m_);
  return jobsDir_;
}

ToolCheck JobRunner::checkTools(JobKind kind) const {
  ToolCheck t;
  const fs::path exe = kind == JobKind::Match ? settings_.matchExecutable() : settings_.exportExecutable();
  const char* name = kind == JobKind::Match ? "sawblade-match" : "sawblade-export";
  if (!isExecutableFile(exe)) {
    t.missing = ToolCheck::Missing::Executable;
    t.message = std::string(name) + " was not found at " + exe.string() + ". Use Locate... to choose the executable (the match venv's bin folder).";
    return t;
  }
  if (kind == JobKind::Match) {
    std::error_code ec;
    const fs::path pool = settings_.poolManifest();
    if (!fs::is_regular_file(pool, ec)) {
      t.missing = ToolCheck::Missing::Pool;
      t.message = "The capture pool manifest was not found at " + pool.string() + ". Pull a pool with sawblade-t3k, or use Locate... to choose pool_manifest.json.";
    }
  }
  return t;
}

namespace {
fs::path newJobDirIn(const fs::path& root, const char* suffix) {
  std::error_code ec;
  fs::create_directories(root, ec);
  std::time_t t = std::time(nullptr);
  for (;; ++t) {
    std::tm tm{};
#if JUCE_WINDOWS
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    const fs::path d = root / (std::string(buf) + "-" + suffix);
    if (!fs::exists(d, ec) && fs::create_directories(d, ec)) return d;
    if (t > std::time(nullptr) + 600) return {};
  }
}
}  // namespace

void JobRunner::retire(std::shared_ptr<Job>& j) {
  if (!j) return;
  j->stopMonitoring = true;
  if (j->monitor.joinable()) j->monitor.join();
  j.reset();
}

bool JobRunner::startMatch(const MatchRequest& r, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  if (const ToolCheck t = checkTools(JobKind::Match); !t.ok()) return fail(t.message);
  std::error_code ec;
  if (!fs::is_regular_file(r.di, ec)) return fail("The DI take was not found: " + r.di.string());
  if (!fs::is_regular_file(r.ref, ec)) return fail("The reference file was not found: " + r.ref.string());
  auto job = std::make_shared<Job>();
  job->kind = JobKind::Match;
  job->exe = settings_.matchExecutable().string();
  job->wantProgressJson = true;
  job->args = {"--di", r.di.string(), "--ref", r.ref.string(), "--ref-channel", "mid", "--pool", settings_.poolManifest().string()};
  if (r.offsetMs) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", *r.offsetMs);
    job->args.insert(job->args.end(), {"--offset-ms", buf});
  }
  job->snap.reference = r.referenceLabel.empty() ? r.ref.filename().string() : r.referenceLabel;
  job->snap.di = r.diLabel.empty() ? r.di.filename().string() : r.diLabel;
  return launch(JobKind::Match, std::move(job), error);
}

bool JobRunner::startExport(const ExportRequest& r, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  if (const ToolCheck t = checkTools(JobKind::Export); !t.ok()) return fail(t.message);
  std::error_code ec;
  if (!fs::is_regular_file(r.preset, ec)) return fail("The preset to export was not found: " + r.preset.string());
  auto job = std::make_shared<Job>();
  job->kind = JobKind::Export;
  job->exe = settings_.exportExecutable().string();
  job->settings = &settings_;
  job->wantProgressJson = true;  // probe --help for --progress-json (the checkpoint's progress.json is the fallback)
  job->args = {r.preset.string(), "--mode", r.mode, "--size", r.size, "--device", "auto", "--require-accept"};
  if (r.di) job->args.insert(job->args.end(), {"--di", r.di->string()});
  else if (r.diBuiltin) job->args.insert(job->args.end(), {"--di", "builtin"});
  if (r.allowInexact) job->args.push_back("--allow-inexact");
  if (!r.resumeDir.empty()) {
    if (!fs::is_directory(r.resumeDir, ec)) return fail("The run to resume was not found: " + r.resumeDir.string());
    job->args.insert(job->args.end(), {"--resume", r.resumeDir.string()});
    job->outDir = r.resumeDir;  // a resumed run continues in its own folder
  }
  job->sourcePreset = r.preset;
  {
    const std::string bytes = readFile(r.preset);
    job->snap.sourceSha256 = sha256Hex(bytes);
  }
  job->exportsRoot = r.exportsRoot;  // "" = <job dir>/export (launch)
  job->snap.allowInexact = r.allowInexact;
  job->snap.diBuiltin = r.diBuiltin && !r.di;
  job->snap.source = r.preset;
  job->snap.exportMode = r.mode;
  job->snap.exportSize = r.size;
  job->snap.reference = r.preset.filename().string();
  job->snap.di = r.di ? r.di->filename().string() : r.diBuiltin ? "built-in signal" : "";
  return launch(JobKind::Export, std::move(job), error);
}

bool JobRunner::launch(JobKind kind, std::shared_ptr<Job> job, std::string* error) {
  std::lock_guard<std::mutex> lk(m_);
  if (slot(kind) && slot(kind)->snap.active()) {  // snap is only written by the job's threads under its own mutex
    std::lock_guard<std::mutex> jl(slot(kind)->m);
    if (slot(kind)->snap.active()) {
      if (error) *error = std::string("A ") + jobKindName(kind) + " job is already running.";
      return false;
    }
  }
  const fs::path dir = newJobDirIn(jobsDir_, jobKindName(kind));
  if (dir.empty()) {
    if (error) *error = "Could not create a job folder in " + jobsDir_.string();
    return false;
  }
  job->dir = dir;
  if (kind == JobKind::Match) {
    job->outDir = dir;
    job->args.insert(job->args.end(), {"--out", dir.string()});
  } else {
    // The exporter names its own folder, <exports root>/<name>-<mode>-<size>-<ts>, and reports it in the progress file
    // (a resumed run continues in the folder it was given). The default root is inside the job folder.
    if (job->exportsRoot.empty()) job->exportsRoot = dir / "export";
    std::error_code ec;
    fs::create_directories(job->exportsRoot, ec);
    if (job->outDir.empty()) job->args.insert(job->args.end(), {"--exports-root", job->exportsRoot.string()});
  }
  job->snap.exportsRoot = job->exportsRoot;
  job->progressFile = dir / "progress.json";
  job->help = help_;
  job->grace = std::chrono::milliseconds(kind == JobKind::Export ? exportGraceMs_.load() : graceMs_.load());
  job->startedMs = nowMs();
  job->snap.kind = kind;
  job->snap.dir = dir;
  job->snap.outDir = job->outDir;
  job->snap.state = JobState::Starting;
  job->commandLine.push_back(job->exe);
  for (const auto& a : job->args) job->commandLine.push_back(a);
  job->writeJobJson();
  retire(slot(kind));
  slot(kind) = job;
  job->monitor = std::thread([job]() mutable {
    // --- probe the executable for --progress-json
    if (job->wantProgressJson) {
      const std::string key = job->exe + "|" + std::to_string(juce::File(job->exe).getLastModificationTime().toMilliseconds());
      bool has;
      bool cached;
      {
        std::lock_guard<std::mutex> hl(job->help->m);
        const auto it = job->help->listsProgressJson.find(key);
        cached = it != job->help->listsProgressJson.end();
        has = cached && it->second;
      }
      if (!cached) {
        has = probeProgressJson(job->exe, job->stopMonitoring);
        if (job->stopMonitoring) return;
        std::lock_guard<std::mutex> hl(job->help->m);
        job->help->listsProgressJson[key] = has;
      }
      if (has) {
        job->args.insert(job->args.end(), {"--progress-json", job->progressFile.string()});
        job->commandLine.push_back("--progress-json");
        job->commandLine.push_back(job->progressFile.string());
      }
      std::lock_guard<std::mutex> sl(job->m);
      job->snap.progressJson = has;
    }
    if (job->cancelRequested) {
      job->finishedMs = nowMs();
      job->finish(JobState::Cancelled, "Cancelled before it started.");
      return;
    }
    // Match jobs write into their own folder; the exporter into <job>/export.
    job->commandLine.clear();
    job->commandLine.push_back(job->exe);
    for (const auto& a : job->args) job->commandLine.push_back(a);
#if JUCE_WINDOWS
    job->finishedMs = nowMs();
    job->finish(JobState::Failed, "Running the match tools is not supported on this platform.");
    return;
#else
    // posix_spawn: the tool is the leader of a new process group (cancel signals the whole group), its stdout and
    // stderr are appended to <job>/log.txt, and its pid is known at once.
    pid_t pid = 0;
    {
      posix_spawn_file_actions_t fa;
      posix_spawnattr_t at;
      posix_spawn_file_actions_init(&fa);
      posix_spawnattr_init(&at);
      const std::string logPath = (job->dir / "log.txt").string();
      posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
      posix_spawn_file_actions_addopen(&fa, 1, logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
      posix_spawn_file_actions_adddup2(&fa, 1, 2);
      posix_spawnattr_setpgroup(&at, 0);
      // The tool starts clean: no signal mask, default SIGPIPE / SIGTERM / SIGINT, and no inherited descriptors
      // beyond 0, 1, 2 (the host's sockets, devices and files stay out of it).
      sigset_t none, defaults;
      sigemptyset(&none);
      sigemptyset(&defaults);
      sigaddset(&defaults, SIGPIPE);
      sigaddset(&defaults, SIGTERM);
      sigaddset(&defaults, SIGINT);
      posix_spawnattr_setsigmask(&at, &none);
      posix_spawnattr_setsigdefault(&at, &defaults);
      short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
      flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
      posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#endif
#endif
      posix_spawnattr_setflags(&at, flags);
      std::vector<std::string> store;
      store.push_back(job->exe);
      for (const auto& a : job->args) store.push_back(a);
      std::vector<char*> argv;
      for (auto& s : store) argv.push_back(s.data());
      argv.push_back(nullptr);
      const int rc = posix_spawn(&pid, job->exe.c_str(), &fa, &at, argv.data(), SAWBLADE_ENVIRON);
      posix_spawn_file_actions_destroy(&fa);
      posix_spawnattr_destroy(&at);
      if (rc != 0) {
        job->finishedMs = nowMs();
        job->finish(JobState::Failed, "Could not start " + job->exe + ": " + std::strerror(rc));
        return;
      }
    }
    job->pid.store(pid);
    job->pgid.store(pid);
    job->spawnedMs.store(nowMs());
    job->setState(JobState::Running);
    job->writeJobJson();

    // --- monitor: log lines, progress, cancel (group SIGTERM, then SIGKILL after the grace period), exit
    bool exited = false;
    int status = 0;
    while (!job->stopMonitoring) {
      job->driveCancel(pid);
      job->pollLogFile();
      job->pollProgress();
      const pid_t r = ::waitpid(pid, &status, WNOHANG);
      if (r == pid || (r < 0 && errno != EINTR)) {
        exited = true;
        break;
      }
      std::this_thread::sleep_for(kPollPeriod);
    }
    if (!exited) return;  // runner destroyed: the child keeps running; the job dir lets the next runner re-attach
    if (job->cancelRequested) signalGroup(pid, Sig::Kill);  // sweep helpers that outlived the tool
    job->pollLogFile(/*flush=*/true);
    job->pollProgress();
    {
      std::lock_guard<std::mutex> sl(job->m);
      job->snap.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    }
#endif
    finalizeJob(*job);
  });
  return true;
}

// ---- finishing, re-attaching, queries -----------------------------------------------------------------------------
void JobRunner::finalizeJob(Job& job) {
  JobSnapshot s;
  {
    std::lock_guard<std::mutex> lk(job.m);
    s = job.snap;
  }
  job.finishedMs = nowMs();
  std::error_code ec;
  const bool exporting = job.kind == JobKind::Export;
  // Did the tool leave its result behind?
  std::vector<MatchCandidate> results;
  std::string resultError;
  bool artifacts;
  bool haveReport = false;
  if (!exporting) {
    results = parseMatchResult(job.dir / "result.json", &resultError);
    artifacts = !results.empty();
  } else {
    if (job.outDir.empty()) job.locateOutDir(/*force=*/true);
    haveReport = !job.outDir.empty() && fs::exists(job.outDir / "export_report.json", ec);
    artifacts = haveReport;
    if (!artifacts && !job.outDir.empty())
      for (fs::directory_iterator it(job.outDir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".nam") artifacts = true;
    if (!artifacts) resultError = "The export finished but wrote no model.";
  }
  // Exit 0 = finished; exit 2 = trained but acceptance NOT MET (the files are still written; needs the report);
  // exit 130 = the exporter was interrupted (SIGINT).
  const bool exportDone = exporting && artifacts && (s.exitCode == 0 || (s.exitCode == 2 && haveReport));
  JobState state;
  std::string message;
  if (exportDone && job.cancelRequested) {
    state = JobState::Succeeded;  // it finished before the cancel could land
  } else if (job.cancelRequested || (exporting && s.exitCode == 130)) {
    state = JobState::Cancelled;
    message = "Cancelled.";
  } else if ((exporting ? exportDone : s.exitCode == 0 && artifacts) || (!job.owned && s.exitCode < 0 && artifacts)) {
    state = JobState::Succeeded;
  } else {
    state = JobState::Failed;
    // The CLIs print "error: ..." / "refused: ..." as their last words.
    for (auto it = s.logTail.rbegin(); it != s.logTail.rend(); ++it) {
      const std::string l = lower(trim(*it));
      if (l.rfind("error", 0) == 0 || l.rfind("refused", 0) == 0) {
        message = trim(*it);
        break;
      }
    }
    if (message.empty() && exporting && s.progress.stage == "error" && !s.progress.message.empty()) message = s.progress.message;
    if (message.empty()) {
      if (s.exitCode > 0) message = "The process exited with code " + std::to_string(s.exitCode) + ".";
      else if (!resultError.empty() && job.owned) message = resultError;
      else message = "The job was interrupted: its process is gone (the app was closed or it was killed) and it left no result.";
      for (auto it = s.logTail.rbegin(); it != s.logTail.rend(); ++it)
        if (!trim(*it).empty()) {
          message += " Last output: " + trim(*it);
          break;
        }
    }
  }
  JobSnapshot fin;
  {
    std::lock_guard<std::mutex> lk(job.m);
    fin = job.snap;
  }
  fin.state = state;
  if (!message.empty()) fin.message = message;
  if (exporting) {
    if (!job.outDir.empty()) fin.outDir = job.outDir;
    fin.resumable = state != JobState::Succeeded && readCheckpoint(job.outDir).resumable;
    if (state == JobState::Succeeded) {
      fin.result = readExportResult(job.outDir);
      fin.accepted = fin.result.status == "MET" ? "met" : fin.result.status == "NOT MET" ? "NOT MET" : "not judged";
      // The sidecar: the resolved preset that was exported, byte for byte, next to the model.
      const std::string stem = !fin.result.namFile.empty() ? fs::path(fin.result.namFile).stem().string() : job.outDir.filename().string();
      const fs::path sidecar = job.outDir / (stem + ".sawblade.json");
      if (!job.sourcePreset.empty() && fs::copy_file(job.sourcePreset, sidecar, fs::copy_options::overwrite_existing, ec) && !ec) fin.sidecar = sidecar;
      if (job.settings != nullptr && fin.result.wallSeconds > 0.0) job.settings->setExportWallSeconds(fin.exportSize, fin.result.wallSeconds);
    }
  }
  if (state == JobState::Succeeded) {
    fin.results = std::move(results);
    fin.progress.fraction = 1.0;
    fin.progress.etaSeconds = 0.0;
    fin.progress.stage = "done";
  }
  job.writeJobJson(&fin);  // the file first: whoever sees the final state in snapshot() finds job.json up to date
  std::lock_guard<std::mutex> lk(job.m);
  job.snap.state = fin.state;
  job.snap.message = fin.message;
  job.snap.results = std::move(fin.results);
  job.snap.progress.fraction = fin.progress.fraction;
  job.snap.progress.etaSeconds = fin.progress.etaSeconds;
  job.snap.progress.stage = fin.progress.stage;
  job.snap.outDir = fin.outDir;
  job.snap.resumable = fin.resumable;
  job.snap.result = std::move(fin.result);
  job.snap.accepted = fin.accepted;
  job.snap.sidecar = fin.sidecar;
}

void JobRunner::monitorAttached(std::shared_ptr<Job> job) {
  auto alive = [&] { return jobProcessAlive(job->pid.load(), job->pgid.load(), job->spawnedMs.load()); };
  while (!job->stopMonitoring) {
    job->pollLogFile();
    job->pollProgress();
    if (!alive()) break;  // gone, or not our tool any more: never signalled
    job->driveCancel(job->pgid.load());
    std::this_thread::sleep_for(kPollPeriod);
  }
  if (job->stopMonitoring) return;
  job->pollLogFile(/*flush=*/true);
  job->pollProgress();
  finalizeJob(*job);
}

void JobRunner::adopt(JobKind kind, const fs::path& dir) {
  const json j = json::parse(readFile(dir / "job.json"), nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return;
  auto job = std::make_shared<Job>();
  job->kind = kind;
  job->owned = false;
  job->dir = dir;
  job->outDir = j.value("outDir", dir.string());
  job->progressFile = dir / "progress.json";
  job->grace = std::chrono::milliseconds(graceMs_.load());
  job->startedMs = j.value("startedEpochMs", static_cast<std::int64_t>(0));
  job->pid = j.value("pid", static_cast<std::int64_t>(0));
  job->pgid = j.value("pgid", job->pid.load());
  job->spawnedMs = j.value("spawnedEpochMs", job->startedMs);
  if (auto it = j.find("commandLine"); it != j.end() && it->is_array())
    for (const auto& a : *it)
      if (a.is_string()) job->commandLine.push_back(a.get<std::string>());
  const std::string state = j.value("state", std::string());
  job->snap.kind = kind;
  job->snap.dir = dir;
  job->snap.outDir = job->outDir;
  job->snap.progressJson = j.value("progressMode", std::string("log")) == "json";
  job->snap.message = j.value("message", std::string());
  job->snap.reference = j.value("reference", std::string());
  job->snap.di = j.value("di", std::string());
  job->snap.exportMode = j.value("exportMode", std::string());
  job->snap.exportSize = j.value("exportSize", std::string());
  job->snap.exitCode = j.value("exitCode", -1);
  if (kind == JobKind::Export) {
    job->sourcePreset = j.value("sourcePreset", std::string());
    job->exportsRoot = j.value("exportsRoot", std::string());
    job->snap.source = job->sourcePreset;
    job->snap.sourceSha256 = j.value("sourceSha256", std::string());
    job->snap.exportsRoot = job->exportsRoot;
    job->snap.allowInexact = j.value("allowInexact", false);
    job->snap.diBuiltin = j.value("diBuiltin", false);
    job->snap.accepted = j.value("accepted", std::string());
    job->snap.resumable = j.value("resumable", false);
    job->snap.sidecar = j.value("sidecar", std::string());
    job->snap.di = j.value("di", std::string());
    job->settings = &settings_;
  }
  job->snap.pid = job->pid.load();
  job->finishedMs = j.value("finishedEpochMs", static_cast<std::int64_t>(0));
  const bool wasActive = state == "starting" || state == "running";
  if (wasActive && jobProcessAlive(job->pid.load(), job->pgid.load(), job->spawnedMs.load())) {
    job->snap.state = JobState::Running;
    job->snap.progress.stage = "running";
    job->pollLogFile();  // pick up what the log already says
    job->pollProgress();
    slot(kind) = job;
    job->monitor = std::thread([job] { monitorAttached(job); });
    return;
  }
  if (wasActive) {
    job->snap.exitCode = -1;
    finalizeJob(*job);  // the process is gone: succeeded if it left its result, else interrupted
  } else if (state == "succeeded") {
    job->snap.state = JobState::Succeeded;
    if (kind == JobKind::Match) job->snap.results = parseMatchResult(dir / "result.json");
    else job->snap.result = readExportResult(job->outDir);
    job->snap.progress.fraction = 1.0;
    job->snap.progress.stage = "done";
  } else if (state == "cancelled") {
    job->snap.state = JobState::Cancelled;
    if (kind == JobKind::Export) job->snap.resumable = readCheckpoint(job->outDir).resumable;
  } else {
    job->snap.state = JobState::Failed;
  }
  slot(kind) = job;
}

void JobRunner::attachExisting() {
  std::lock_guard<std::mutex> lk(m_);
  std::error_code ec;
  if (!fs::is_directory(jobsDir_, ec)) return;
  std::vector<fs::path> dirs;
  for (fs::directory_iterator it(jobsDir_, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_directory(ec) && fs::exists(it->path() / "job.json", ec)) dirs.push_back(it->path());
  std::sort(dirs.begin(), dirs.end(), std::greater<>());
  for (JobKind kind : {JobKind::Match, JobKind::Export}) {
    if (slot(kind)) continue;
    const std::string suffix = std::string("-") + jobKindName(kind);
    for (const auto& d : dirs) {
      const std::string n = d.filename().string();
      if (n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
        adopt(kind, d);
        if (slot(kind)) break;
      }
    }
  }
}

void JobRunner::cancel(JobKind kind) {
  std::shared_ptr<Job> j;
  {
    std::lock_guard<std::mutex> lk(m_);
    j = slot(kind);
  }
  if (j) j->cancelRequested = true;
}

JobSnapshot JobRunner::snapshot(JobKind kind) const {
  std::shared_ptr<Job> j;
  {
    std::lock_guard<std::mutex> lk(m_);
    j = slot(kind);
  }
  JobSnapshot s;
  s.kind = kind;
  if (!j) return s;
  {
    std::lock_guard<std::mutex> lk(j->m);
    s = j->snap;
  }
  s.pid = j->pid.load();
  const std::int64_t end = s.active() || j->finishedMs.load() == 0 ? nowMs() : j->finishedMs.load();
  s.elapsedSeconds = j->startedMs > 0 ? std::max(0.0, static_cast<double>(end - j->startedMs) / 1000.0) : 0.0;
  return s;
}

bool JobRunner::waitFinished(JobKind kind, std::chrono::milliseconds timeout) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const JobSnapshot s = snapshot(kind);
    if (!s.active()) return true;
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(20ms);
  }
}

}  // namespace sawblade::plugin
