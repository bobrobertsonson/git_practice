#pragma once

#include <cmath>

namespace subforge
{

constexpr double kPi = 3.14159265358979323846;

// Transposed direct form II biquad, run in double precision so very low cutoffs
// (15-30 Hz at 96 kHz) stay numerically stable.
class Biquad
{
public:
    void setHighPass (double sampleRate, double freq, double q) { design (sampleRate, freq, q, true); }
    void setLowPass  (double sampleRate, double freq, double q) { design (sampleRate, freq, q, false); }

    void reset() { z1 = z2 = 0.0; }

    double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

private:
    void design (double sampleRate, double freq, double q, bool highPass)
    {
        const double w0 = 2.0 * kPi * freq / sampleRate;
        const double cosw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;

        if (highPass)
        {
            b0 = (1.0 + cosw) * 0.5 / a0;
            b1 = -(1.0 + cosw) / a0;
        }
        else
        {
            b0 = (1.0 - cosw) * 0.5 / a0;
            b1 = (1.0 - cosw) / a0;
        }

        b2 = b0;
        a1 = -2.0 * cosw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
};

// 4th-order Butterworth (24 dB/oct) made from two cascaded biquads.
class Butterworth4
{
public:
    void setHighPass (double sampleRate, double freq)
    {
        s1.setHighPass (sampleRate, freq, kQ1);
        s2.setHighPass (sampleRate, freq, kQ2);
    }

    void setLowPass (double sampleRate, double freq)
    {
        s1.setLowPass (sampleRate, freq, kQ1);
        s2.setLowPass (sampleRate, freq, kQ2);
    }

    void reset() { s1.reset(); s2.reset(); }

    double process (double x) { return s2.process (s1.process (x)); }

private:
    static constexpr double kQ1 = 0.54119610014619698;
    static constexpr double kQ2 = 1.30656296487637652;
    Biquad s1, s2;
};

// Smooth limiter: linear up to the knee, then bends towards the ceiling and
// never exceeds it. Slope is continuous at the knee, so low-level material is
// untouched and peaks are rounded off instead of hard-clipped.
inline double softClip (double x, double ceiling, double kneeFraction = 0.8)
{
    const double knee = ceiling * kneeFraction;
    const double mag = std::abs (x);

    if (mag <= knee)
        return x;

    const double range = ceiling - knee;
    const double shaped = knee + range * std::tanh ((mag - knee) / range);
    return x < 0.0 ? -shaped : shaped;
}

inline double dbToGain (double db) { return std::pow (10.0, db / 20.0); }

inline double gainToDb (double gain) { return gain > 1.0e-9 ? 20.0 * std::log10 (gain) : -180.0; }

inline double midiNoteToHz (double note) { return 440.0 * std::pow (2.0, (note - 69.0) / 12.0); }

} // namespace subforge
