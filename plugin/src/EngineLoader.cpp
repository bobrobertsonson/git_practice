#include "EngineLoader.h"

#include <exception>

namespace sawblade::plugin {

EngineLoader::EngineLoader(SwapSlot<Engine>& slot, Callback onOutcome) : slot_(slot), callback_(std::move(onOutcome)) {
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
        slot_.collectGarbage();
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
    std::unique_ptr<Engine> engine;
    try {
      engine = Engine::build(req.preset, req.hostRate, req.maxBlock);
      out.built = true;
    } catch (const std::exception& e) {
      out.error = e.what();
    } catch (...) {
      out.error = "unknown error while building the engine";
    }
    if (!engine && req.fallbackToInit) {
      try {
        engine = Engine::build(initRequest(req).preset, req.hostRate, req.maxBlock);
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
      out.presetName = engine->presetName();
      slot_.publish(std::move(engine));
      out.published = true;
      builds_.fetch_add(1);
    }
    slot_.collectGarbage();
    if (callback_) callback_(out);
    {
      std::lock_guard<std::mutex> lk(m_);
      doneId_ = id;
      busy_ = false;
    }
    doneCv_.notify_all();
  }
}

EngineLoader::Request EngineLoader::initRequest(const Request& like) {
  Request r = like;
  r.preset = makeInitPreset();
  return r;
}

}  // namespace sawblade::plugin
