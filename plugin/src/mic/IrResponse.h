#pragma once

#include <complex>
#include <string>
#include <vector>

#include "sawblade/preset.h"

// Magnitude response of an IR (or of the mix of two) for the mic page's plot. JUCE-free; message-thread work
// (an IR is at most 2 s, so evaluating the spectrum directly costs a few tens of milliseconds). The spectrum is
// linear in the IR, so the response of mixIrs(a, b, mix) is the mix of the two spectra: moving the MIX fader
// never touches the files again.
namespace sawblade::plugin::mic {

constexpr double kPlotLowHz = 20.0, kPlotHighHz = 20000.0;
constexpr double kResponseRate = 48000.0;  // IRs are evaluated at this rate (like the chain, they are resampled at load)

using Spectrum = std::vector<std::complex<double>>;

struct Response {
  std::vector<float> db;  // one value per log-spaced point from kPlotLowHz to kPlotHighHz (inclusive)
  bool empty() const { return db.empty(); }
};

// The frequency of plot point i of n (log spacing).
double plotFrequency(int i, int n);

// H(f) of the IR at `points` log-spaced frequencies (the points above 0.45 fs repeat the last value). Empty IR -> empty.
Spectrum irSpectrum(const std::vector<float>& ir, double sampleRate, int points = 240);

// 10 log10 |(1 - mix) A + mix B|^2, smoothed to about 1/3 octave (power average, +-1/6 octave). `b` may be null.
Response responseFromSpectrum(const Spectrum& a, const Spectrum* b = nullptr, double mix = 0.0);

Response magnitudeResponse(const std::vector<float>& ir, double sampleRate, int points = 240);

// Loads the cab IR exactly as the chain does (sawblade::loadIr at 48 kHz, L2-normalised when `normalize`) and returns
// its spectrum. Never throws: on failure the spectrum is empty and *error says why.
Spectrum spectrumOfCapture(const Capture& c, bool normalize, std::string* error = nullptr);

}  // namespace sawblade::plugin::mic
