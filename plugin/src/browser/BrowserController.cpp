#include "BrowserController.h"

#include <algorithm>

#include "PreviewRender.h"
#include "sawblade/auto_trim.h"

namespace sawblade::plugin {

BrowserController::BrowserController(SawbladeProcessor& p, BrowserSettings& s, Slot slot)
    : proc_(p), settings_(s), slot_(slot), client_([&s] { return s.executable(); }) {
  st_.gear = slotGear(slot);
}

BrowserController::~BrowserController() {
  alive_->store(false);
  proc_.previewPlayer().stop();
  client_.cancelAll();
  if (worker_) PreviewWorker::retire(std::move(worker_));  // never joins here
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

int BrowserController::ladderSteps(std::int64_t toneId) const { return proc_.ladderSteps(std::to_string(toneId)); }

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
  if (st_.gear == "amp") proc_.requestLadderLookup(std::to_string(toneId));  // a ladder is an amp thing: not for pedals or cab IRs
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

void BrowserController::loadSwapped(Preset p) {
  // The swap keeps the rig's current trim until the new rig's measurement lands: no level jump (a hash change drops to 0 otherwise).
  const auto s = proc_.status();
  // One undo step (restored as an edit: the rig stays, the old capture and its make-up come back); the make-up that arrived with the swap is part of it.
  proc_.loadPresetUndoable(std::move(p), SawbladeProcessor::HistoryKind::Edit, /*keepMonitor=*/false, s.levelMatchOn ? std::optional<double>(s.trimDb) : std::nullopt);
  st_.busy = false;
  awaitingLoad_ = true;
  setStatus("Loading " + loadedTitle_ + "..." + levelNote_);
  changed();
}

void BrowserController::use(int targetIndex) {
  fetchSelected([this, targetIndex](const t3k::FetchResult& f) {
    const Preset cur = proc_.currentPreset();
    std::string why;
    const auto ts = slotTargets(cur, slot_, &why);
    if (targetIndex < 0 || targetIndex >= static_cast<int>(ts.size())) return fail(why.empty() ? "no such target" : why);
    const SlotTarget target = ts[static_cast<std::size_t>(targetIndex)];
    std::string err;
    auto np = withCapture(cur, target, f, err);
    if (!np) return fail(err);
    levelNote_.clear();
    if (target.isIr() || !proc_.levelMatchEnabled()) return loadSwapped(std::move(*np));
    // LEVEL MATCH: the new capture must not change the slot's loudness on the reference DI. The make-up is computed in the
    // background (the old capture keeps playing meanwhile) and the swap is loaded with it, so there is no jump.
    const int path = target.path == 'a' ? 0 : 1;
    const Preset after = withSlotMakeup(*np, path, target.blockIndex, 0.0);
    const std::uint64_t seq = ++useSeq_;
    st_.busy = true;
    setStatus("LEVEL MATCHING... (" + loadedTitle_ + ")");
    changed();
    proc_.computeSlotMakeup(cur, after, path, [this, alive = alive_, seq, target, f, path](const LevelWorker::MakeupResult& r) {
      juce::MessageManager::callAsync([this, alive, seq, target, f, path, mk = r.makeupDb] {
        if (!alive->load() || seq != useSeq_) return;
        // Edits made while the level was measured are kept: the swap is applied to the rig as it is now.
        const Preset now = proc_.currentPreset();
        const auto ts2 = slotTargets(now, slot_);
        std::string e2;
        const SlotTarget* t2 = nullptr;
        for (const auto& t : ts2)
          if (t.kind == target.kind && t.path == target.path) t2 = &t;
        if (t2 == nullptr) return fail("the slot is no longer in the rig");
        auto swapped = withCapture(now, *t2, f, e2);
        if (!swapped) return fail(e2);
        levelNote_ = mk ? "" : " (no level match: could not be measured)";
        loadSwapped(mk ? withSlotMakeup(*swapped, path, t2->blockIndex, *mk) : std::move(*swapped));
      });
    });
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
    PreviewWorker::Job job;
    if (proc_.levelMatchEnabled() && !previewRender) {  // LEVEL MATCH: the candidate is previewed at the rig's level
      job.levelMatch.on = true;
      job.levelMatch.before = cur;
      if (!ts.front().isIr()) {
        job.levelMatch.path = ts.front().path == 'a' ? 0 : 1;
        job.levelMatch.block = ts.front().blockIndex;
      }
    }
    job.preset = std::move(np);
    const double hr = proc_.status().hostRate;
    job.hostRate = hr > 0.0 ? hr : 48000.0;
    const std::uint64_t gen = ++previewGen_;
    proc_.previewPlayer().stop();
    st_.previewing = false;
    setStatus("Rendering the preview...");
    changed();
    job.alive = alive_;
    job.render = previewRender;
    job.onDone = [this, gen, rate = job.hostRate](std::vector<float> out, std::string err) {  // alive_ checked by the worker's callAsync
      if (gen != previewGen_) return;
      if (!err.empty() || out.empty()) return fail(err.empty() ? "the preview render is empty" : "Preview failed: " + err);
      proc_.previewPlayer().start(std::move(out), rate);
      st_.busy = false;
      st_.previewing = true;
      previewStarted_ = true;
      previewSeenPlaying_ = false;
      previewStartMs_ = juce::Time::getMillisecondCounter();
      setStatus("Playing the preview");
      changed();
    };
    if (!worker_) worker_ = std::make_shared<PreviewWorker>();
    worker_->submit(std::move(job));
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

void BrowserController::poll() {
  proc_.previewPlayer().collect();
  bool dirty = false;
  if (awaitingLoad_) {
    const auto s = proc_.status();
    if (!s.loading) {
      awaitingLoad_ = false;
      if (!s.error.empty()) setStatus(s.error, true);
      else setStatus("Using " + loadedTitle_ + levelNote_);
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
