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
  expectedB_ = applied.b.blocks;
  const int a = ampIndex(applied.a);
  aTitle_.clear();
  if (a >= 0)
    if (const auto* nam = dynamic_cast<const NamBlockParams*>(applied.a.blocks[static_cast<std::size_t>(a)].params.get())) aTitle_ = captureTitle(nam->model);
  std::error_code ec;
  if (networkToolsDisabled() || !fs::exists(settings::t3kExecutable(), ec)) return;  // no tool: the immediate fill is all there is
  start(Step::Suggest, {"suggest-body", "--a-title", aTitle_, "--cache-dir", captureCacheRoot().string(), "--json"});
}

void BodyFill::start(Step s, std::vector<std::string> args) {
  // The previous run has finished (its completion is what led here), but T3kTool joins its thread lazily: retry briefly.
  for (int i = 0; i < 500 && tool_.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  if (networkToolsDisabled()) {
    step_ = Step::Idle;
    return;
  }
  step_ = s;
  const std::uint64_t run = ++run_;
  ++runs_;
  const bool started = tool_.start(std::move(args), nullptr, [this, run](const T3kTool::Result& r) {  // background thread
    std::lock_guard<std::mutex> lk(m_);
    done_.push_back({run, r.status == T3kTool::Status::Ok, r.output});
  });
  if (!started) step_ = Step::Idle;
}

bool BodyFill::bodyUntouched(const Preset& cur) const { return cur.b.enabled && cur.b.blocks == expectedB_; }

void BodyFill::applyAmp(const Capture& model) {
  Preset cur = proc_.editBasePreset();
  if (!bodyUntouched(cur)) return;  // path B was edited (or BLEND turned off) since: the suggestion is dropped
  const int amp = ampIndex(cur.b);
  if (amp >= 0)
    if (const auto* nam = dynamic_cast<const NamBlockParams*>(cur.b.blocks[static_cast<std::size_t>(amp)].params.get()))
      if (nam->model.source && model.source && nam->model.source->id == model.source->id && nam->model.source->modelId == model.source->modelId) return;  // already there
  setBodyAmp(cur, model);
  expectedB_ = cur.b.blocks;
  proc_.loadPreset(std::move(cur), /*keepMonitor=*/true);  // coalesced with the BLEND edit: no undo entry of its own
}

void BodyFill::fallback() {
  const Preset cur = proc_.editBasePreset();
  if (!bodyUntouched(cur)) {
    step_ = Step::Idle;
    return;
  }
  if (const int amp = ampIndex(cur.b); amp >= 0 && !cur.b.blocks.empty() && dynamic_cast<const NamBlockParams*>(cur.b.blocks[static_cast<std::size_t>(amp)].params.get())) {
    step_ = Step::Idle;  // the fallback amp is already there
    return;
  }
  start(Step::FetchFallback, {"fetch", kFallbackBodyTone, "--json", "--cache-dir", captureCacheRoot().string()});
}

void BodyFill::tick() {
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
      const auto s = d.ok ? parseSuggestBody(d.output) : std::nullopt;
      if (!s) return fallback();
      if (const auto c = cachedToneCapture(s->toneId, s->modelId)) {
        Capture cap = *c;
        cap.source->title = s->title;
        step_ = Step::Idle;
        return applyAmp(cap);
      }
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
      if (const auto c = d.ok ? parseFetchedCapture(d.output) : std::nullopt) applyAmp(*c);
      return;
    }
    case Step::Idle: return;
  }
}

}  // namespace sawblade::plugin::rig
