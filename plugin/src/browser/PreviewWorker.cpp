#include "PreviewWorker.h"

#include <list>

#include <juce_events/juce_events.h>

#include "PreviewRender.h"
#include "sawblade/auto_trim.h"

namespace sawblade::plugin {
namespace {
struct Graveyard {
  struct Item {
    std::thread t;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::mutex m;
  std::list<Item> items;
  ~Graveyard() {
    for (auto& i : items)
      if (i.t.joinable()) i.t.join();
  }
};
Graveyard& graveyard() {
  static Graveyard g;
  return g;
}
}  // namespace

PreviewWorker::PreviewWorker() { thread_ = std::thread([this] { run(); }); }

PreviewWorker::~PreviewWorker() {
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
    pending_.reset();
  }
  cancelled_.store(true);
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void PreviewWorker::submit(Job job) {
  {
    std::lock_guard<std::mutex> lk(m_);
    pending_ = std::move(job);
  }
  cv_.notify_one();
}

void PreviewWorker::cancel() {
  cancelled_.store(true);
  std::lock_guard<std::mutex> lk(m_);
  pending_.reset();
}

void PreviewWorker::retire(std::shared_ptr<PreviewWorker> w) {
  w->cancel();
  auto& g = graveyard();
  auto done = std::make_shared<std::atomic<bool>>(false);
  std::thread t([w = std::move(w), done]() mutable {
    w.reset();  // joins the worker thread, off the caller's thread
    done->store(true);
  });
  std::lock_guard<std::mutex> lk(g.m);
  for (auto it = g.items.begin(); it != g.items.end();) {
    if (it->done->load()) {
      it->t.join();
      it = g.items.erase(it);
    } else {
      ++it;
    }
  }
  g.items.push_back({std::move(t), std::move(done)});
}

void PreviewWorker::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [&] { return stop_ || pending_.has_value(); });
      if (stop_) return;
      job = std::move(*pending_);
      pending_.reset();
    }
    if (cancelled_.load()) return;
    std::string err;
    std::vector<float> out;
    if (job.render) {
      out = job.render(job.preset, job.hostRate, err);
    } else {
      if (riff_.interleaved.empty()) {
        try {
          riff_ = embeddedPreviewRiff();
        } catch (const std::exception& e) {
          err = std::string("preview riff: ") + e.what();
        }
      }
      Preset p = job.preset;
      if (err.empty() && job.levelMatch.on) {
        try {
          if (job.levelMatch.path >= 0) {
            const Preset zero = withSlotMakeup(p, job.levelMatch.path, job.levelMatch.block, 0.0);
            if (const auto mk = slotMakeupDb(job.levelMatch.before, zero, job.levelMatch.path, &cache_))
              p = withSlotMakeup(p, job.levelMatch.path, job.levelMatch.block, *mk);
          }
          if (cancelled_.load()) return;
          ensureAutoTrim(p, &cache_);
        } catch (const std::exception& e) {
          err = std::string("level match: ") + e.what();
        }
      }
      if (err.empty()) out = renderPreview(p, riff_, job.hostRate, &cache_, err, job.levelMatch.on);
    }
    if (cancelled_.load() || !job.alive || !job.alive->load()) continue;
    juce::MessageManager::callAsync([alive = job.alive, cb = std::move(job.onDone), err = std::move(err), out = std::move(out)]() mutable {
      if (alive->load()) cb(std::move(out), std::move(err));
    });
  }
}

}  // namespace sawblade::plugin
