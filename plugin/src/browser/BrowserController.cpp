#include "BrowserController.h"

#include <algorithm>

#include "PreviewRender.h"

namespace sawblade::plugin {

BrowserController::BrowserController(SawbladeProcessor& p, BrowserSettings& s, Slot slot)
    : proc_(p), settings_(s), slot_(slot), client_([&s] { return s.executable(); }) {
  st_.gear = slotGear(slot);
  pthread_ = std::thread([this] { previewWorker(); });
}

BrowserController::~BrowserController() {
  alive_->store(false);
  proc_.previewPlayer().stop();
  {
    std::lock_guard<std::mutex> lk(pm_);
    pstop_ = true;
    pending_.reset();
  }
  pcv_.notify_all();
  client_.cancelAll();
  if (pthread_.joinable()) pthread_.join();
}

void BrowserController::changed() {
  if (onChange) onChange();
}

void BrowserController::setStatus(const std::string& text, bool error) {
  st_.status = text;
  st_.statusIsError = error;
}

void BrowserController::fail(const std::string& text) {
  st_.busy = false;
  st_.previewing = false;
  setStatus(text, true);
  changed();
}

std::vector<const t3k::CaptureRecord*> BrowserController::visible() const {
  std::vector<const t3k::CaptureRecord*> v;
  for (const auto& r : st_.records)
    if (!st_.passesOnly || r.passes) v.push_back(&r);
  return v;
}

const t3k::CaptureRecord* BrowserController::selected() const {
  for (const auto& r : st_.records)
    if (r.toneId == st_.selectedId) return &r;
  return nullptr;
}

std::vector<SlotTarget> BrowserController::targets(std::string* why) const { return slotTargets(proc_.currentPreset(), slot_, why); }

void BrowserController::setExecutable(const std::string& path) {
  settings_.setExecutable(path);
  start();
}

// --- login / queries -----------------------------------------------------------------------------
void BrowserController::start() {
  st_.view = View::Checking;
  st_.listError.reset();
  changed();
  client_.whoami([this](Reply<t3k::WhoAmI> r) {
    if (r.ok) {
      st_.view = View::Browse;
      reload();
    } else if (r.error.code == "auth" || r.error.code == "exit") {
      st_.view = View::LoginRequired;
      changed();
    } else {
      st_.view = View::Browse;  // cannot run / network / timeout: show it with the settings row
      st_.records.clear();
      st_.listError = r.error;
      changed();
    }
  });
}

void BrowserController::handleError(const t3k::ErrorInfo& e) {
  if (e.code == "auth") {
    st_.view = View::LoginRequired;
    st_.listError.reset();
  } else {
    st_.listError = e;
  }
}

void BrowserController::reload() {
  const std::uint64_t seq = ++listSeq_;
  st_.listError.reset();
  if (st_.source == Source::Search && st_.query.empty()) {
    st_.records.clear();
    st_.loading = false;
    changed();
    return;
  }
  st_.loading = true;
  changed();
  client_.list(st_.source, st_.query, st_.gear, 100, [this, seq](Reply<T3kClient::Records> r) {
    if (seq != listSeq_) return;
    st_.loading = false;
    if (r.ok) {
      st_.records = std::move(r.value);
      if (selected() == nullptr) {
        st_.selectedId = 0;
        st_.models.clear();
      }
    } else {
      st_.records.clear();
      handleError(r.error);
    }
    changed();
  });
}

void BrowserController::setSource(Source s) {
  st_.source = s;
  if (st_.view == View::Browse) reload();
  else changed();
}
void BrowserController::setGear(const std::string& g) {
  st_.gear = g;
  if (st_.view == View::Browse) reload();
  else changed();
}
void BrowserController::setPassesOnly(bool b) {
  st_.passesOnly = b;
  changed();
}
void BrowserController::setQuery(const std::string& q) {
  st_.query = q;
  if (st_.view == View::Browse) reload();
  else changed();
}

void BrowserController::select(std::int64_t toneId) {
  if (toneId == st_.selectedId) return;
  st_.selectedId = toneId;
  st_.models.clear();
  st_.modelIndex = 0;
  st_.modelsLoading = toneId != 0;
  setStatus({});
  changed();
  if (toneId == 0) return;
  const std::uint64_t seq = ++modelsSeq_;
  client_.models(toneId, [this, seq, toneId](Reply<t3k::ModelsResult> r) {
    if (seq != modelsSeq_ || toneId != st_.selectedId) return;
    st_.modelsLoading = false;
    if (r.ok) st_.models = std::move(r.value.models);
    else setStatus(describe(r.error), true);
    changed();
  });
}

void BrowserController::selectModel(int index) {
  if (index < 0 || index >= static_cast<int>(st_.models.size())) return;
  st_.modelIndex = index;
  changed();
}

void BrowserController::logIn() {
  st_.view = View::LoggingIn;
  st_.loginUri.clear();
  st_.loginCode.clear();
  st_.loginMessage = "Starting the login...";
  changed();
  client_.login(
      [this](const t3k::LoginEvent& ev) {
        if (ev.kind == t3k::LoginEvent::Kind::DeviceCode) {
          st_.loginUri = ev.verificationUriComplete.empty() ? ev.verificationUri : ev.verificationUriComplete;
          st_.loginCode = ev.userCode;
          st_.loginMessage = "Open the page, check the code and approve. Waiting for you...";
          changed();
        }
      },
      [this](Reply<bool> r) {
        if (st_.view != View::LoggingIn) return;
        if (r.ok) {
          st_.view = View::Browse;
          reload();
        } else {
          st_.view = View::LoginRequired;
          st_.loginMessage = describe(r.error);
          changed();
        }
      });
}

void BrowserController::cancelLogin() {
  client_.cancelAll();
  st_.view = View::LoginRequired;
  st_.loginMessage.clear();
  changed();
}

// --- USE / PREVIEW -------------------------------------------------------------------------------
void BrowserController::fetchSelected(std::function<void(const t3k::FetchResult&)> then) {
  const auto* rec = selected();
  if (rec == nullptr) return;
  const std::int64_t model = st_.modelIndex < static_cast<int>(st_.models.size()) ? st_.models[static_cast<std::size_t>(st_.modelIndex)].modelId : 0;
  const std::uint64_t seq = ++fetchSeq_;
  st_.busy = true;
  loadedTitle_ = rec->title;
  setStatus("Fetching " + rec->title + "...");
  changed();
  client_.fetch(rec->toneId, model, [this, seq, then = std::move(then)](Reply<t3k::FetchResult> r) {
    if (seq != fetchSeq_) return;
    if (!r.ok) {
      if (r.error.code == "auth") {  // the token expired: back to the login view; the user retries
        st_.view = View::LoginRequired;
        st_.busy = false;
        setStatus({});
        changed();
        return;
      }
      fail(describe(r.error));
      return;
    }
    then(r.value);
  });
}

void BrowserController::use(int targetIndex) {
  fetchSelected([this, targetIndex](const t3k::FetchResult& f) {
    const Preset cur = proc_.currentPreset();
    std::string why;
    const auto ts = slotTargets(cur, slot_, &why);
    if (targetIndex < 0 || targetIndex >= static_cast<int>(ts.size())) return fail(why.empty() ? "no such target" : why);
    std::string err;
    auto np = withCapture(cur, ts[static_cast<std::size_t>(targetIndex)], f, err);
    if (!np) return fail(err);
    proc_.loadPreset(std::move(*np));
    st_.busy = false;
    awaitingLoad_ = true;
    setStatus("Loading " + loadedTitle_ + "...");
    changed();
  });
}

void BrowserController::preview() {
  fetchSelected([this](const t3k::FetchResult& f) {
    const Preset cur = proc_.currentPreset();
    std::string why;
    const auto ts = slotTargets(cur, slot_, &why);
    if (ts.empty()) return fail(why);
    Preset np = cur;
    std::string err;
    for (const auto& t : ts) {  // a per-path cab: the candidate is heard in both paths
      auto r = withCapture(np, t, f, err);
      if (!r) return fail(err);
      np = std::move(*r);
    }
    PreviewJob job;
    job.preset = std::move(np);
    const double hr = proc_.status().hostRate;
    job.hostRate = hr > 0.0 ? hr : 48000.0;
    job.gen = ++previewGen_;
    proc_.previewPlayer().stop();
    st_.previewing = false;
    setStatus("Rendering the preview...");
    changed();
    {
      std::lock_guard<std::mutex> lk(pm_);
      pending_ = std::move(job);
    }
    pcv_.notify_one();
  });
}

void BrowserController::stopPreview() {
  ++previewGen_;  // a render in flight is discarded
  proc_.previewPlayer().stop();
  st_.previewing = false;
  st_.busy = false;
  previewStarted_ = false;
  if (!st_.statusIsError) setStatus({});
  changed();
}

void BrowserController::previewWorker() {
  for (;;) {
    PreviewJob job;
    {
      std::unique_lock<std::mutex> lk(pm_);
      pcv_.wait(lk, [&] { return pstop_ || pending_.has_value(); });
      if (pstop_) return;
      job = std::move(*pending_);
      pending_.reset();
    }
    std::string err;
    if (riff_.interleaved.empty()) {
      try {
        riff_ = embeddedPreviewRiff();
      } catch (const std::exception& e) {
        err = std::string("preview riff: ") + e.what();
      }
    }
    std::vector<float> out;
    if (err.empty()) out = renderPreview(job.preset, riff_, job.hostRate, &cache_, err);
    juce::MessageManager::callAsync([this, alive = alive_, gen = job.gen, rate = job.hostRate, err, out = std::move(out)]() mutable {
      if (!alive->load() || gen != previewGen_) return;
      if (!err.empty() || out.empty()) return fail(err.empty() ? "the preview render is empty" : "Preview failed: " + err);
      proc_.previewPlayer().start(std::move(out), rate);
      st_.busy = false;
      st_.previewing = true;
      previewStarted_ = true;
      previewSeenPlaying_ = false;
      previewStartMs_ = juce::Time::getMillisecondCounter();
      setStatus("Playing the preview");
      changed();
    });
  }
}

void BrowserController::poll() {
  proc_.previewPlayer().collect();
  bool dirty = false;
  if (awaitingLoad_) {
    const auto s = proc_.status();
    if (!s.loading) {
      awaitingLoad_ = false;
      if (!s.error.empty()) setStatus(s.error, true);
      else setStatus("Using " + loadedTitle_);
      dirty = true;
    }
  }
  if (st_.previewing && previewStarted_ && proc_.previewPlayer().playing()) previewSeenPlaying_ = true;
  // Over once the audio thread has played it and stopped, or (no audio running) after the riff's length.
  if (st_.previewing && previewStarted_ && ((previewSeenPlaying_ && !proc_.previewPlayer().playing()) || juce::Time::getMillisecondCounter() - previewStartMs_ > 7500)) {
    previewSeenPlaying_ = false;
    st_.previewing = false;
    previewStarted_ = false;
    if (!st_.statusIsError) setStatus({});
    dirty = true;
  }
  if (dirty) changed();
}

}  // namespace sawblade::plugin
