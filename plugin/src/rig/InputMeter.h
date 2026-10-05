#pragma once

// Lock-free ring of per-block input peaks (linear) for the gate LEARN function. The audio thread
// pushes one value per processed chunk (summed mono input, before the engine); the message thread
// reads what arrived since a counter it noted earlier. Only the last kSize blocks are kept.
// push() allocates nothing and takes no lock.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace sawblade::plugin::rig {

class InputMeter {
 public:
  static constexpr std::uint32_t kSize = 512;

  // Audio thread (single producer).
  void push(float peak) noexcept {
    const std::uint32_t c = counter_.load(std::memory_order_relaxed);
    ring_[c % kSize].store(peak, std::memory_order_relaxed);
    counter_.store(c + 1, std::memory_order_release);
  }

  // Number of blocks pushed so far (wraps at 2^32).
  std::uint32_t counter() const noexcept { return counter_.load(std::memory_order_acquire); }

  // The peaks pushed since `since` (a value counter() returned earlier), oldest first; at most the
  // last kSize. Message thread.
  std::vector<float> since(std::uint32_t since) const {
    const std::uint32_t now = counter_.load(std::memory_order_acquire);
    const std::uint32_t count = std::min<std::uint32_t>(now - since, kSize);
    std::vector<float> out;
    out.reserve(count);
    for (std::uint32_t i = now - count; i != now; ++i) out.push_back(ring_[i % kSize].load(std::memory_order_relaxed));
    return out;
  }

 private:
  std::array<std::atomic<float>, kSize> ring_{};
  std::atomic<std::uint32_t> counter_{0};
};

}  // namespace sawblade::plugin::rig
