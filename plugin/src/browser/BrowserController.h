#pragma once

// The logic of the capture browser, without any widgets (CaptureBrowser draws it): login state, the query,
// the records, the selection and its models, USE (fetch + swap through the processor's normal loader) and
// PREVIEW (fetch + candidate render on a worker + hand-over to the PreviewPlayer). Message thread only;
// onChange fires after every state change.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "BrowserSettings.h"
#include "PluginProcessor.h"
#include "PreviewWorker.h"
#include "SlotTarget.h"
#include "T3kClient.h"
#include "sawblade/capture_cache.h"
#include "sawblade/wav_io.h"

namespace sawblade::plugin {

class BrowserController {
 public:
  enum class View { Checking, LoginRequired, LoggingIn, Browse };
  using Source = T3kClient::Source;

  struct State {
    View view = View::Checking;
    std::string loginUri, loginCode, loginMessage;
    Source source = Source::Favorites;
    std::string gear;  // pedal | amp | ir
    bool passesOnly = true;
    std::string query;
    bool loading = false;
    std::optional<t3k::ErrorInfo> listError;
    std::vector<t3k::CaptureRecord> records;  // as returned
    std::int64_t selectedId = 0;
    std::vector<t3k::ModelInfo> models;
    bool modelsLoading = false;
    int modelIndex = 0;
    std::string status;     // fetching / rendering / playing / loaded / error text
    bool statusIsError = false;
    bool busy = false;      // a fetch or render for USE / PREVIEW is in flight
    bool previewing = false;
  };

  BrowserController(SawbladeProcessor& p, BrowserSettings& s, Slot slot);
  ~BrowserController();
  BrowserController(const BrowserController&) = delete;
  BrowserController& operator=(const BrowserController&) = delete;

  std::function<void()> onChange;
  // Test seam: replaces the preview render (runs on the worker thread).
  PreviewWorker::RenderFn previewRender;

  const State& state() const noexcept { return st_; }
  Slot slot() const noexcept { return slot_; }
  std::vector<const t3k::CaptureRecord*> visible() const;
  const t3k::CaptureRecord* selected() const;
  // What this session knows about the gain ladder of a tone: -1 unknown, 0 none, n >= 2 steps (the processor's `ladder` tool answers). The
  // browser marks a card only when this is >= 2. select() asks about the tone it selects (lazily, once per tone per session, through the
  // processor's own ladder tool: no extra work per row, nothing queued on the browser's own tool runner).
  int ladderSteps(std::int64_t toneId) const;
  // The slot's targets in the current preset (and why not, if none).
  std::vector<SlotTarget> targets(std::string* why = nullptr) const;
  std::string executable() const { return settings_.executable(); }
  void setExecutable(const std::string& path);  // "" = reset; re-checks the login

  void start();  // whoami; then the first query
  void setSource(Source s);
  void setGear(const std::string& g);
  void setPassesOnly(bool b);
  void setQuery(const std::string& q);  // runs a Search query when the source is Search
  void reload();
  void select(std::int64_t toneId);
  void selectModel(int index);
  void logIn();
  void cancelLogin();

  // targetIndex: index into targets() (0 unless the cab is per-path). Needs a selection.
  void use(int targetIndex = 0);
  void preview();
  void stopPreview();

  // Message thread, periodic (~10 Hz): reports load failures after USE and the end of a preview.
  void poll();

  T3kClient& client() { return client_; }

 private:
  void changed();
  void fail(const std::string& text);
  void setStatus(const std::string& text, bool error = false);
  void handleError(const t3k::ErrorInfo& e);
  void fetchSelected(std::function<void(const t3k::FetchResult&)> then);
  void loadSwapped(Preset p);

  SawbladeProcessor& proc_;
  BrowserSettings& settings_;
  Slot slot_;
  State st_;
  T3kClient client_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::uint64_t listSeq_ = 0, modelsSeq_ = 0, fetchSeq_ = 0, previewGen_ = 0, useSeq_ = 0;
  bool awaitingLoad_ = false;
  std::string loadedTitle_, levelNote_;
  bool previewStarted_ = false, previewSeenPlaying_ = false;
  juce::uint32 previewStartMs_ = 0;

  std::shared_ptr<PreviewWorker> worker_;
};

}  // namespace sawblade::plugin
