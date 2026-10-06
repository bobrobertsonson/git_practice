#include "rig/BodyFill.h"

#include <filesystem>

#include "rig/RigModel.h"

namespace sawblade::plugin::rig {
namespace fs = std::filesystem;

std::optional<BodySuggestion> parseSuggestBody(const std::string& output) {
  const auto lo = output.find('{');
  const auto hi = output.rfind('}');
  if (lo == std::string::npos || hi == std::string::npos || hi < lo) return std::nullopt;
  const nlohmann::json j = nlohmann::json::parse(output.substr(lo, hi - lo + 1), nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return std::nullopt;
  const auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
  BodySuggestion s;
  s.toneId = str("tone_id");
  s.modelId = str("model_id");
  s.title = str("title");
  s.cached = j.contains("cached") && j["cached"].is_boolean() && j["cached"].get<bool>();
  if (s.toneId.empty() || s.modelId.empty()) return std::nullopt;
  return s;
}

std::optional<Capture> parseFetchedCapture(const std::string& output) {
  const auto lo = output.find('{');
  const auto hi = output.rfind('}');
  if (lo == std::string::npos || hi == std::string::npos || hi < lo) return std::nullopt;
  const nlohmann::json j = nlohmann::json::parse(output.substr(lo, hi - lo + 1), nullptr, /*allow_exceptions=*/false);
  if (!j.is_object() || !j.contains("path") || !j["path"].is_string() || !j.contains("source") || !j["source"].is_object()) return std::nullopt;
  const auto& s = j["source"];
  const auto str = [&](const char* k) { return s.contains(k) && s[k].is_string() ? s[k].get<std::string>() : std::string(); };
  Capture c;
  c.file = j["path"].get<std::string>();
  c.resolvedPath = c.file;
  std::error_code ec;
  if (c.file.empty() || !fs::exists(c.resolvedPath, ec)) return std::nullopt;
  if (j.contains("sha256") && j["sha256"].is_string()) c.sha256 = j["sha256"].get<std::string>();
  CaptureSource src;
  src.provider = str("provider");
  src.id = str("id");
  src.modelId = str("modelId");
  src.url = str("url");
  src.title = str("title");
  src.creator = str("creator");
  src.license = str("license");
  if (src.provider.empty() || src.id.empty()) return std::nullopt;
  c.source = src;
  return c;
}

BodyFill::BodyFill(SawbladeProcessor& p) : proc_(p) {}
BodyFill::~BodyFill() = default;  // T3kTool cancels and joins

void BodyFill::cancel() {
  step_ = Step::Idle;
  status_ = {};
  queued_.reset();
  ++run_;  // results of runs in flight are stale
  tool_.cancel();
  std::lock_guard<std::mutex> lk(m_);
  done_.clear();
}

bool BodyFill::waitToolIdle(std::chrono::milliseconds timeout) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (tool_.running() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return !tool_.running();
}

void BodyFill::begin(const Preset& applied) {
  cancel();
  expectedB_ = applied.b;
  expectedBlend_ = applied.blend;
  const int a = ampIndex(applied.a);
  aTitle_.clear();
  if (a >= 0)
    if (const auto* nam = dynamic_cast<const NamBlockParams*>(applied.a.blocks[static_cast<std::size_t>(a)].params.get())) aTitle_ = captureTitle(nam->model);
  // An immediate fill that already has an amp: the suggestion may swap it, but nothing is missing, so nothing is said.
  const bool missingAmp = ampIndex(applied.b) < 0;
  if (missingAmp) status_ = {FillStatus::Kind::Downloading, {}, FillReason::None, {}};  // no name yet: "CHOOSING A BODY AMP..."
  std::error_code ec;
  if (networkToolsDisabled()) {
    if (missingAmp) fail(FillReason::NetworkOff, "network tools are disabled (SAWBLADE_NO_NETWORK)");
    return;  // the immediate fill is all there is
  }
  const fs::path exe = settings::t3kExecutable();
  if (exe.has_parent_path() && !fs::exists(exe, ec)) {  // a bare name is looked up on PATH by the tool run itself
    if (missingAmp) fail(FillReason::NoTool, "cannot find the sawblade-t3k tool at " + exe.string());
    return;
  }
  start(Step::Suggest, {"suggest-body", "--a-title", aTitle_, "--cache-dir", captureCacheRoot().string(), "--json"});
}

void BodyFill::retry() {
  const Preset cur = proc_.editBasePreset();
  if (!cur.b.enabled) return;
  begin(cur);
}

void BodyFill::fail(FillReason r, std::string detail) {
  step_ = Step::Idle;
  queued_.reset();
  status_ = {FillStatus::Kind::Failed, {}, r, std::move(detail)};
}

// A tool run that did not give a capture: why, from the exit status and the `--json` {"error","code"} line.
void BodyFill::failFrom(const Done& d, FillReason dflt) {
  FillReason r = dflt;
  std::string code;
  const auto lo = d.output.find('{');
  const auto hi = d.output.rfind('}');
  if (lo != std::string::npos && hi != std::string::npos && hi > lo) {
    const nlohmann::json j = nlohmann::json::parse(d.output.substr(lo, hi - lo + 1), nullptr, /*allow_exceptions=*/false);
    if (j.is_object() && j.contains("code") && j["code"].is_string()) code = j["code"].get<std::string>();
  }
  if (d.status == T3kTool::Status::NotLoggedIn || code == "auth") r = FillReason::NotLoggedIn;
  else if (d.status == T3kTool::Status::MissingExecutable) r = FillReason::NoTool;
  else if (code == "network") r = FillReason::Network;
  else if (code == "not_found") r = FillReason::NoCapture;
  else if (code == "license") r = FillReason::License;
  else if (!d.ok) r = FillReason::Other;
  fail(r, d.message.empty() ? d.output : d.message);
}

void BodyFill::start(Step s, std::vector<std::string> args) {
  step_ = s;
  if (networkToolsDisabled()) return fail(FillReason::NetworkOff, "network tools are disabled (SAWBLADE_NO_NETWORK)");
  // The previous run's completion is what led here, but T3kTool is still "running" until its thread ends: wait for the next tick.
  if (tool_.running()) {
    queued_ = std::make_pair(s, std::move(args));
    return;
  }
  launch(s, std::move(args));
}

void BodyFill::launch(Step s, std::vector<std::string> args) {
  step_ = s;
  const std::uint64_t run = ++run_;
  ++runs_;
  const bool started = tool_.start(std::move(args), nullptr, [this, run](const T3kTool::Result& r) {  // background thread
    std::lock_guard<std::mutex> lk(m_);
    done_.push_back({run, r.status == T3kTool::Status::Ok, r.status, r.message, r.output});
  });
  if (!started) fail(FillReason::Other, "could not start the sawblade-t3k tool");
}

// Path B is still what the fill made. A swap of an amp the fill already put there keeps the v0.2 rule: any edit of path B or the blend
// drops the suggestion. A fill that is still COMPLETING a boost-only path (no amp yet: it is downloading) only needs the structure to be
// unchanged: the parameter-backed values (LEVEL, the amp knobs, BLEND) moving meanwhile are not edits of the path. A player turning
// the BLEND knob while the amp downloads is the normal case, and dropping the amp then left a silent boost-only path (v0.3 Task C).
bool BodyFill::bodyUntouched(const Preset& cur) const {
  if (!cur.b.enabled) return false;  // the topology was switched off: the suggestion no longer applies
  if (ampIndex(expectedB_) >= 0) return cur.b == expectedB_ && cur.blend == expectedBlend_;
  PathPreset b = cur.b;
  b.levelDb = expectedB_.levelDb;
  b.ampControls = expectedB_.ampControls;
  return b == expectedB_;
}

void BodyFill::applyAmp(const Capture& model) {
  Preset cur = proc_.editBasePreset();
  if (!bodyUntouched(cur)) {  // path B was edited (or the blend topology turned off) since: the suggestion is dropped
    status_ = {};
    return;
  }
  const int amp = ampIndex(cur.b);
  if (amp >= 0)
    if (const auto* nam = dynamic_cast<const NamBlockParams*>(cur.b.blocks[static_cast<std::size_t>(amp)].params.get()))
      if (nam->model.source && model.source && nam->model.source->id == model.source->id && nam->model.source->modelId == model.source->modelId) {  // already there
        status_ = {};
        return;
      }
  // The arrival belongs to the BLEND fill's undo step: it adds none, and the stored snapshots that hold the fill as it was (boost only, or
  // the fallback amp) get the amp too, so an undo of a later edit does not hand back a path B without it.
  // The match is STRUCTURE-ONLY: bodyUntouched() compares path B with what the fill made (blocks, enabled, and the blend / level / amp knobs
  // when an amp is already there), so a snapshot that holds the fill as it was gets the amp, while a snapshot with a different path B (the
  // pre-BLEND preset, path B edited since) is left alone.
  proc_.patchHistory([&](Preset& snap) {
    if (bodyUntouched(snap)) setBodyAmp(snap, model);
  });
  setBodyAmp(cur, model);
  proc_.loadPreset(std::move(cur), /*keepMonitor=*/true);  // not a user edit: loadPreset() records no undo step
  // What the rig is now: loadPreset clamps the values to the parameter grid, so record that (off-grid values must not make the
  // untouched check fail).
  const Preset now = proc_.editBasePreset();
  expectedB_ = now.b;
  expectedBlend_ = now.blend;
  status_ = {};
}

void BodyFill::fallback() {
  const Preset cur = proc_.editBasePreset();
  if (!bodyUntouched(cur)) {
    step_ = Step::Idle;
    status_ = {};
    return;
  }
  if (const int amp = ampIndex(cur.b); amp >= 0 && !cur.b.blocks.empty() && dynamic_cast<const NamBlockParams*>(cur.b.blocks[static_cast<std::size_t>(amp)].params.get())) {
    step_ = Step::Idle;  // the fallback amp is already there
    status_ = {};
    return;
  }
  status_ = {FillStatus::Kind::Downloading, kFallbackBodyTitle, FillReason::None, {}};
  start(Step::FetchFallback, {"fetch", kFallbackBodyTone, "--json", "--cache-dir", captureCacheRoot().string()});
}

void BodyFill::tick() {
  if (queued_ && !tool_.running()) {
    auto q = std::move(*queued_);
    queued_.reset();
    launch(q.first, std::move(q.second));
  }
  Done d;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (done_.empty()) return;
    d = std::move(done_.front());
    done_.erase(done_.begin());
  }
  if (d.run != run_ || step_ == Step::Idle) return;  // stale (cancelled / superseded)
  switch (step_) {
    case Step::Suggest: {
      if (!d.ok && (d.status == T3kTool::Status::MissingExecutable)) return failFrom(d, FillReason::NoTool);
      const auto s = d.ok ? parseSuggestBody(d.output) : std::nullopt;
      if (!s) return fallback();  // the pool is empty / no high-gain amp in it / the tool failed: the shortlist amp
      const std::string name = s->title.empty() ? "tone " + s->toneId : s->title;
      if (const auto c = cachedToneCapture(s->toneId, s->modelId)) {
        Capture cap = *c;
        if (cap.source->title.empty()) cap.source->title = s->title;
        step_ = Step::Idle;
        return applyAmp(cap);
      }
      status_ = {FillStatus::Kind::Downloading, name, FillReason::None, {}};
      start(Step::FetchSuggested, {"fetch", s->toneId, "--model", s->modelId, "--json", "--cache-dir", captureCacheRoot().string()});
      return;
    }
    case Step::FetchSuggested: {
      const auto c = d.ok ? parseFetchedCapture(d.output) : std::nullopt;
      if (!c) return fallback();
      step_ = Step::Idle;
      return applyAmp(*c);
    }
    case Step::FetchFallback: {
      step_ = Step::Idle;
      if (const auto c = d.ok ? parseFetchedCapture(d.output) : std::nullopt) return applyAmp(*c);
      return failFrom(d, FillReason::NoCapture);  // never silent: the head says why, and what to do
    }
    case Step::Idle: return;
  }
}

}  // namespace sawblade::plugin::rig
