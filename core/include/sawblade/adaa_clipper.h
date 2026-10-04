#pragma once

namespace sawblade {

// Static piecewise-polynomial soft clipper, per sign with knee k (k+ for u >= 0, k- for u < 0):
//   c(u) = u - u^3 / (3 k^2)   for |u| < k,     c(u) = +-2k/3 beyond.
// C1 everywhere (c'(+-k) = 0), slope 1 at 0 so small signals pass linearly, symmetric when
// k+ == k-. F1 = integral of c, F2 = integral of F1, both with F(0) = 0 and continuous at +-k
// (closed forms in the .cpp).
//
// Order m = 2 (quintic, harder knee): c(u) = u - u^5/(5 k^4) for |u| < k, +-4k/5 beyond; slope
// 1 - (u/k)^4. F1 = u^2/2 - u^6/(30 k^4) inside, (4k/5)|u| - k^2/3 beyond; F2 = u^3/6 - u^7/(210 k^4)
// inside, +-((2k/5)u^2 - (k^2/3)|u| + 2k^3/21) beyond (continuous at 0 and +-k).
struct SoftClipShape {
  double kPos = 0.5, kNeg = 0.5;
  int order = 1;  // m in {1, 2}
  double f(double u) const noexcept;    // c(u)
  double f1(double u) const noexcept;   // first antiderivative
  double f2(double u) const noexcept;   // second antiderivative
};

// Second-order antiderivative anti-aliasing (ADAA2; Parker, Esqueda, Bilbao, "Reducing the
// aliasing of nonlinear waveshaping using continuous-time convolution", DAFx 2016) of the shape
// above. Run it at the oversampled rate.
//
//   y[n] = 2/(x[n]-x[n-2]) * ( D(x[n],x[n-1]) - D(x[n-1],x[n-2]) ),  D(a,b) = (F2(a)-F2(b))/(a-b)
//
// Ill-conditioning (kEps = 1e-5, evaluated in double): D(a,b) = F1((a+b)/2) when |a-b| < eps, and
// when |x[n]-x[n-2]| < eps the exact limit y = 2 (F1(xm) - D(xm,x[n-1])) / (xm - x[n-1]) with
// xm = (x[n]+x[n-2])/2 (and c((x[n]+x[n-1]+x[n-2])/3) if x[n-1] is close as well). The limit is
// consistent with the regular branch: for a linear c the output is the 3-tap mean
// (x[n]+x[n-1]+x[n-2])/3 in every branch.
//
// ADAA2 delays the signal by exactly kLatency = 1 sample (at the rate it runs at). With ADAA
// switched off (test-only) the clipper is the plain memoryless c(u) with no delay.
class AdaaClipper {
 public:
  static constexpr double kEps = 1e-5;
  static constexpr int kLatency = 1;

  void setShape(double kPos, double kNeg, int order = 1) noexcept {
    shape_.kPos = kPos;
    shape_.kNeg = kNeg;
    shape_.order = order == 2 ? 2 : 1;
  }
  void setAdaa(bool on) noexcept { adaa_ = on; }
  bool adaa() const noexcept { return adaa_; }
  int latencySamples() const noexcept { return adaa_ ? kLatency : 0; }
  const SoftClipShape& shape() const noexcept { return shape_; }

  void reset() noexcept;
  float processSample(float x) noexcept;
  void process(float* io, int n) noexcept;

 private:
  SoftClipShape shape_;
  bool adaa_ = true;
  double x1_ = 0.0, x2_ = 0.0;  // x[n-1], x[n-2]
  double dPrev_ = 0.0;          // D(x[n-1], x[n-2])
};

}  // namespace sawblade
