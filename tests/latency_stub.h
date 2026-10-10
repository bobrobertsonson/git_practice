#pragma once

// Test-only block type "test.latency": a Processor that really delays by N samples and reports N.
// Not NAM-trainable. Call registerLatencyStub() before parsing a preset that uses it.
#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

#include "sawblade/block_registry.h"

namespace sawblade::test {

// ---- test-only block type: a Processor that really delays by N samples and reports N --------
struct LatencyParams : BlockParams {
  int latency = 0;
  bool equals(const BlockParams& o) const override {
    const auto* p = dynamic_cast<const LatencyParams*>(&o);
    return p && p->latency == latency;
  }
  nlohmann::json toJson() const override { return {{"latency", latency}}; }
};

class LatencyStub : public Processor {
 public:
  explicit LatencyStub(int n) : n_(n) {}
  void prepare(const ProcessSpec&) override { buf_.assign(static_cast<std::size_t>(n_) + 1, 0.0f); w_ = 0; }
  void reset() override { std::fill(buf_.begin(), buf_.end(), 0.0f); w_ = 0; }
  void process(float* io, int n) noexcept override {
    const int size = static_cast<int>(buf_.size());
    for (int i = 0; i < n; ++i) {
      buf_[static_cast<std::size_t>(w_)] = io[i];
      int r = w_ - n_;
      if (r < 0) r += size;
      io[i] = buf_[static_cast<std::size_t>(r)];
      if (++w_ == size) w_ = 0;
    }
  }
  int latencySamples() const noexcept override { return n_; }

 private:
  int n_;
  std::vector<float> buf_;
  int w_ = 0;
};

inline void registerLatencyStub() {
  static const bool once = [] {
    BlockType t;
    t.traits.namTrainable = false;  // pretend it is a time-based effect
    t.parse = [](JsonObject& o, const std::filesystem::path&) -> std::shared_ptr<const BlockParams> {
      auto p = std::make_shared<LatencyParams>();
      p->latency = o.requireInteger("latency", 0, 10000);
      return p;
    };
    t.create = [](const Block& b, const BlockBuildContext&) -> std::unique_ptr<Processor> {
      return std::make_unique<LatencyStub>(static_cast<const LatencyParams&>(*b.params).latency);
    };
    BlockRegistry::instance().add("test.latency", std::move(t));
    return true;
  }();
  (void)once;
}


}  // namespace sawblade::test
