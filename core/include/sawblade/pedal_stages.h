#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace sawblade::stages {

// Clip types of the chainsaw-family pedals (phase 7c, docs/specs/phase7c_chainsaw_family.md).
// Same names and knees as phase 7b's `ClipType` / `clipShapeSpec`; 7c keeps its own copy in this
// namespace so both branches compile after the merge.
//
// Post-merge cleanup: replace this ClipType with 7b's (pedal_common.h); `led` there uses the
// quintic shape (order 2) where this header feeds the cubic `SoftClipShape` of AdaaClipper
// unchanged. The cubic shape is NOT changed here.
enum class ClipType { Silicon, Led, Asymmetric };

struct ClipKnees {
  double kPos, kNeg;  // for AdaaClipper::setShape(kPos, kNeg)
};

ClipKnees clipKnees(ClipType) noexcept;
const char* clipTypeName(ClipType) noexcept;  // "silicon" | "led" | "asymmetric"
std::optional<ClipType> parseClipType(std::string_view);

// Integer delay of 0..kMax base-rate samples, in place, allocation-free (std::array storage).
// Carries the dry signal of a `mix` control by exactly the pedal's latencySamples().
class DryDelay {
 public:
  static constexpr int kMax = 64;
  void set(int d) noexcept {
    d_ = d < 0 ? 0 : (d > kMax ? kMax : d);
    reset();
  }
  void reset() noexcept {
    buf_.fill(0.0f);
    w_ = 0;
  }
  int delay() const noexcept { return d_; }
  void process(float* io, int n) noexcept {
    if (d_ == 0) return;
    for (int i = 0; i < n; ++i) {
      const float x = io[i];
      io[i] = buf_[static_cast<std::size_t>((w_ + kSize - d_) % kSize)];
      buf_[static_cast<std::size_t>(w_)] = x;
      w_ = (w_ + 1) % kSize;
    }
  }

 private:
  static constexpr int kSize = kMax + 1;
  std::array<float, kSize> buf_{};
  int w_ = 0, d_ = 0;
};

}  // namespace sawblade::stages
