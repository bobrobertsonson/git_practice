#include "sawblade/adaa_clipper.h"

#include <cmath>

namespace sawblade {

double SoftClipShape::f(double u) const noexcept {
  const double k = u >= 0.0 ? kPos : kNeg;
  if (u >= k) return 2.0 * k / 3.0;
  if (u <= -k) return -2.0 * k / 3.0;
  return u - u * u * u / (3.0 * k * k);
}

// F1: u^2/2 - u^4/(12k^2) inside; (2k/3)|u| - k^2/4 beyond (value 5k^2/12 at the knee).
double SoftClipShape::f1(double u) const noexcept {
  const double k = u >= 0.0 ? kPos : kNeg;
  const double a = std::fabs(u);
  if (a < k) return 0.5 * u * u - u * u * u * u / (12.0 * k * k);
  return (2.0 * k / 3.0) * a - 0.25 * k * k;
}

// F2: u^3/6 - u^5/(60k^2) inside; +-((k/3)u^2 - (k^2/4)|u| + k^3/15) beyond (odd in each branch,
// value +-3k^3/20 at the knee, so F2 and F1 are continuous at 0 and +-k).
double SoftClipShape::f2(double u) const noexcept {
  const double k = u >= 0.0 ? kPos : kNeg;
  const double a = std::fabs(u);
  if (a < k) return u * u * u / 6.0 - u * u * u * u * u / (60.0 * k * k);
  const double m = (k / 3.0) * a * a - 0.25 * k * k * a + k * k * k / 15.0;
  return u >= 0.0 ? m : -m;
}

void AdaaClipper::reset() noexcept {
  x1_ = x2_ = 0.0;
  dPrev_ = 0.0;
}

float AdaaClipper::processSample(float xin) noexcept {
  const double a = xin;
  if (!adaa_) return static_cast<float>(shape_.f(a));
  const double b = x1_, c = x2_;
  const double dab = std::fabs(a - b) < kEps ? shape_.f1(0.5 * (a + b)) : (shape_.f2(a) - shape_.f2(b)) / (a - b);
  double y;
  if (std::fabs(a - c) < kEps) {
    if (std::fabs(a - b) < kEps) {
      y = shape_.f((a + b + c) / 3.0);
    } else {
      const double xm = 0.5 * (a + c);
      const double d = (shape_.f2(xm) - shape_.f2(b)) / (xm - b);
      y = 2.0 * (shape_.f1(xm) - d) / (xm - b);
    }
  } else {
    y = 2.0 * (dab - dPrev_) / (a - c);
  }
  x2_ = b;
  x1_ = a;
  dPrev_ = dab;
  return static_cast<float>(y);
}

void AdaaClipper::process(float* io, int n) noexcept {
  for (int i = 0; i < n; ++i) io[i] = processSample(io[i]);
}

}  // namespace sawblade
