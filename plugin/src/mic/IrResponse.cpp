#include "IrResponse.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "PresetMapping.h"
#include "sawblade/ir.h"

namespace sawblade::plugin::mic {

double plotFrequency(int i, int n) {
  const double t = n > 1 ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.0;
  return kPlotLowHz * std::pow(kPlotHighHz / kPlotLowHz, t);
}

Spectrum irSpectrum(const std::vector<float>& ir, double sampleRate, int points) {
  Spectrum out;
  if (ir.empty() || points < 2 || sampleRate <= 0.0) return out;
  const double fMax = 0.45 * sampleRate;
  out.resize(static_cast<std::size_t>(points));
  for (int p = 0; p < points; ++p) {
    const double w = 2.0 * std::numbers::pi * std::min(plotFrequency(p, points), fMax) / sampleRate;
    const double c = std::cos(w), s = std::sin(w);
    double re = 0.0, im = 0.0, pr = 1.0, pi = 0.0;  // the phasor e^{-jwn} is (pr, -pi)
    for (float h : ir) {
      re += static_cast<double>(h) * pr;
      im += static_cast<double>(h) * pi;
      const double npr = pr * c + pi * s, npi = pi * c - pr * s;  // times e^{-jw}
      pr = npr;
      pi = npi;
    }
    out[static_cast<std::size_t>(p)] = {re, -im};
  }
  return out;
}

Response responseFromSpectrum(const Spectrum& a, const Spectrum* b, double mix) {
  Response r;
  const std::size_t n = a.size();
  if (n < 2 || (b != nullptr && b->size() != n)) return r;
  mix = std::clamp(mix, 0.0, 1.0);
  std::vector<double> power(n);
  for (std::size_t i = 0; i < n; ++i) power[i] = std::norm(b ? (1.0 - mix) * a[i] + mix * (*b)[i] : a[i]);
  const double octaves = std::log2(kPlotHighHz / kPlotLowHz);
  const int points = static_cast<int>(n);
  const int half = std::max(1, static_cast<int>(std::lround(static_cast<double>(points - 1) / octaves / 6.0)));
  r.db.resize(n);
  for (int p = 0; p < points; ++p) {
    const int lo = std::max(0, p - half), hi = std::min(points - 1, p + half);
    double sum = 0.0;
    for (int k = lo; k <= hi; ++k) sum += power[static_cast<std::size_t>(k)];
    sum /= static_cast<double>(hi - lo + 1);
    r.db[static_cast<std::size_t>(p)] = static_cast<float>(10.0 * std::log10(std::max(sum, 1e-20)));
  }
  return r;
}

Response magnitudeResponse(const std::vector<float>& ir, double sampleRate, int points) {
  return responseFromSpectrum(irSpectrum(ir, sampleRate, points));
}

Spectrum spectrumOfCapture(const Capture& c, bool normalize, std::string* error) {
  if (isNoCapture(c)) return {};  // no cab: nothing to read, and nothing to report
  try {
    const IrData ir = loadIr(locateCapture(c).empty() ? std::filesystem::path(c.file) : locateCapture(c), kResponseRate, normalize);
    return irSpectrum(ir.samples, kResponseRate);
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return {};
  }
}

}  // namespace sawblade::plugin::mic
