#include "EngineLoader.h"

#include <exception>

namespace sawblade::plugin {

EngineLoader::EngineLoader(SwapSlot<EngineRef>& slot, Callback onOutcome) : slot_(slot), callback_(std::move(onOutcome)) {
  thread_ = std::thread([this] { run(); });
}

EngineLoader::~EngineLoader() {
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

std::uint64_t EngineLoader::submit(Request r) {
  std::uint64_t id;
  {
    std::lock_guard<std::mutex> lk(m_);
    id = ++nextId_;
    pendingId_ = id;
    pending_ = std::move(r);
  }
  cv_.notify_all();
  return id;
}

bool EngineLoader::waitFor(std::uint64_t id, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(m_);
  return doneCv_.wait_for(lk, timeout, [&] { return doneId_ >= id; });
}

bool EngineLoader::waitIdle(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(m_);
  return doneCv_.wait_for(lk, timeout, [&] { return !pending_ && !busy_; });
}

void EngineLoader::run() {
  for (;;) {
    Request req;
    std::uint64_t id = 0;
    {
      std::unique_lock<std::mutex> lk(m_);
      // The timeout is only for garbage collection of engines the audio thread has replaced.
      cv_.wait_for(lk, std::chrono::milliseconds(200), [&] { return stop_ || pending_.has_value(); });
      if (stop_) return;
      if (!pending_) {
        lk.unlock();
        collect();
        continue;
      }
      req = std::move(*pending_);
      id = pendingId_;
      pending_.reset();
      busy_ = true;
    }

    Outcome out;
    out.id = id;
    out.hostRate = req.hostRate;
    out.maxBlock = req.maxBlock;
    out.wanted = req.wanted;
    std::unique_ptr<Engine> engine;
    try {
      engine = Engine::build(req.preset, req.hostRate, req.maxBlock, &cache_, req.calibration);
      out.built = true;
    } catch (const std::exception& e) {
      out.error = e.what();
    } catch (...) {
      out.error = "unknown error while building the engine";
    }
    if (!engine && req.fallbackToInit) {
      try {
        engine = Engine::build(initRequest(req).preset, req.hostRate, req.maxBlock, &cache_);
      } catch (...) {
      }
    }

    bool superseded;
    {
      std::lock_guard<std::mutex> lk(m_);
      superseded = pending_.has_value();
    }
    if (engine && out.built && superseded) {  // stale: a newer request is waiting
      engine.reset();
      out.superseded = true;
      out.built = false;
    }
    if (engine) {
      out.latencySamples = engine->latencySamples();
      out.modelRate = engine->modelRate();
      out.info = engine->chainInfo();
      out.calibration = engine->calibrationSummary();
      out.presetName = engine->presetName();
      engine->setGeneration(id);
      if (out.built && req.configure) req.configure(*engine);
      if (out.built && req.beforePublish) req.beforePublish(id);
      std::shared_ptr<Engine> shared = std::move(engine);
      owned_.push_back(shared);
      out.engine = shared;
      slot_.publish(std::make_unique<EngineRef>(EngineRef{std::move(shared)}));
      out.published = true;
      builds_.fetch_add(1);
    }
    collect();
    if (callback_) callback_(out);
    {
      std::lock_guard<std::mutex> lk(m_);
      doneId_ = id;
      busy_ = false;
    }
    doneCv_.notify_all();
  }
}

void EngineLoader::collect() {
  slot_.collectGarbage();  // frees the retired slot nodes (their shared_ptr references)
  // An engine nobody else references (the audio thread has finished fading it out, or it was
  // superseded before being picked up) is destroyed here. The newest is always kept.
  //
  // Why use_count() == 1 is a sound gate: a new reference to an engine can only be made by copying
  // an existing one. The only copiers are this thread (which does not copy owned_ entries) and the
  // audio thread, which copies exclusively from the EngineRef node that is current in the SwapSlot
  // (and moves its own cur_/fading_ around). Once the loader's reference is the only one left, no
  // slot node (pending, current or retired) and no audio-thread member refers to the engine any
  // more, so nothing can acquire a reference again and the count cannot rise. A stale (non-1)
  // reading only delays destruction to a later pass; the audio thread never holds the last
  // reference because this vector always holds one.
  for (std::size_t i = 0; i + 1 < owned_.size();) {
    if (owned_[i].use_count() == 1)
      owned_.erase(owned_.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
}

EngineLoader::Request EngineLoader::initRequest(const Request& like) {
  Request r = like;
  r.preset = makeInitPreset();
  return r;
}

}  // namespace sawblade::plugin
