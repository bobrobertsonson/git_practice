#include "PresetLoadFlow.h"

#include <system_error>

#include <juce_events/juce_events.h>

namespace sawblade::plugin {
namespace fs = std::filesystem;

namespace {
std::string lowerName(SubBank b) {
  std::string s = subBankName(b);
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<const Capture*> capturesOf(const Preset& p) {
  std::vector<const Capture*> v;
  for (const PathPreset* path : {&p.a, &p.b})
    for (const auto& b : path->blocks)
      if (const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get())) v.push_back(&nam->model);
  if (p.cab.enabled) {
    if (p.cab.mode == CabMode::Shared) v.push_back(&p.cab.ir);
    else {
      v.push_back(&p.cab.irA);
      v.push_back(&p.cab.irB);
    }
  }
  return v;
}

bool present(const Capture& c) {
  std::error_code ec;
  return fs::exists(locateCapture(c), ec);
}
}  // namespace

LoadPlan planPresetLoad(const fs::path& presetFile, SubBank bank, const fs::path& resolvedRoot) {
  LoadPlan plan;
  plan.load = presetFile;
  plan.resolvedOut = (resolvedRoot.empty() ? appDataDir() / "resolved" : resolvedRoot) / lowerName(bank) / (presetFile.stem().string() + ".resolved.json");
  Preset p;
  try {
    p = sawblade::loadPresetFile(presetFile);
  } catch (const std::exception& e) {
    plan.kind = LoadPlan::Kind::Invalid;
    plan.error = e.what();
    return plan;
  }
  bool missing = false;
  for (const Capture* c : capturesOf(p))
    if (c->source && c->source->provider == "tone3000" && !present(*c)) missing = true;
  if (!missing) return plan;  // Direct
  std::error_code ec;
  if (fs::exists(plan.resolvedOut, ec) && fs::last_write_time(plan.resolvedOut, ec) > fs::last_write_time(presetFile, ec)) {
    try {
      const Preset r = sawblade::loadPresetFile(plan.resolvedOut);
      bool all = true;
      for (const Capture* c : capturesOf(r)) all = all && present(*c);
      if (all) {
        plan.kind = LoadPlan::Kind::UseResolved;
        plan.load = plan.resolvedOut;
        return plan;
      }
    } catch (const std::exception&) {
    }
  }
  plan.kind = LoadPlan::Kind::NeedsResolve;
  return plan;
}

PresetLoadFlow::PresetLoadFlow(SawbladeProcessor& p, Callbacks cb, Post post) : proc_(p), cb_(std::move(cb)), post_(std::move(post)) {
  if (!post_) post_ = [](std::function<void()> f) { juce::MessageManager::callAsync(std::move(f)); };
}

PresetLoadFlow::~PresetLoadFlow() { *alive_ = false; }

void PresetLoadFlow::finish(Outcome o) {
  resolving_ = false;
  if (cb_.onFinished) cb_.onFinished(o);
}

bool PresetLoadFlow::load(const fs::path& presetFile, SubBank bank) {
  if (resolving_) return false;
  const LoadPlan plan = planPresetLoad(presetFile, bank);
  Outcome o;
  o.file = presetFile;
  if (plan.kind == LoadPlan::Kind::Invalid) {
    o.status = Outcome::Status::Invalid;
    o.message = plan.error;
    finish(o);
    return true;
  }
  if (plan.kind != LoadPlan::Kind::NeedsResolve) {
    std::string err;
    if (proc_.loadPresetFile(plan.load, &err, /*undoable=*/true)) {
      o.status = Outcome::Status::Loaded;
      o.loadedFile = plan.load;
    } else {
      o.message = err;
    }
    finish(o);
    return true;
  }
  std::error_code ec;
  fs::create_directories(plan.resolvedOut.parent_path(), ec);
  resolving_ = true;
  const auto alive = alive_;
  const Post post = post_;
  const bool started = tool_.start(
      {"resolve", presetFile.string(), "-o", plan.resolvedOut.string(), "--progress-json"},
      [this, alive, post](const T3kTool::Progress& p) {
        post([this, alive, p] {
          if (*alive && cb_.onProgress) cb_.onProgress(p.done, p.total, p.name);
        });
      },
      [this, alive, post, o, plan](const T3kTool::Result& r) mutable {
        post([this, alive, o, plan, r]() mutable {
          if (!*alive) return;
          o.message = r.message;
          switch (r.status) {
            case T3kTool::Status::Ok: {
              std::string err;
              if (proc_.loadPresetFile(plan.resolvedOut, &err, /*undoable=*/true)) {
                o.status = Outcome::Status::Loaded;
                o.loadedFile = plan.resolvedOut;
                o.message.clear();
              } else {
                o.status = Outcome::Status::Failed;
                o.message = err;
              }
              break;
            }
            case T3kTool::Status::NotLoggedIn: o.status = Outcome::Status::NotLoggedIn; break;
            case T3kTool::Status::MissingExecutable: o.status = Outcome::Status::MissingExecutable; break;
            case T3kTool::Status::Cancelled: o.status = Outcome::Status::Cancelled; break;
            case T3kTool::Status::Failed: o.status = Outcome::Status::Failed; break;
          }
          finish(o);
        });
      });
  if (!started) resolving_ = false;
  return started;
}

}  // namespace sawblade::plugin
