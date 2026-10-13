// Offline tests for the SubForge DSP. No JUCE needed: builds and runs anywhere.
//
//   subforge_tests [output-dir]
//
// With an output dir, demo renders are also written as 24-bit WAV files.

#include "BassDuck.h"
#include "SubEngine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace subforge;

namespace
{

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;

struct NoteEvent
{
    double time;
    int note;
    float velocity; // 0 = note off
};

std::vector<float> render (const EngineSettings& settings, const std::vector<NoteEvent>& events, double seconds,
                           double bpm = 120.0, const std::vector<float>* sidechain = nullptr)
{
    SubEngine engine;
    engine.setTempo (bpm);
    engine.prepare (kSampleRate);
    engine.setSettings (settings);

    const int total = static_cast<int> (seconds * kSampleRate);
    std::vector<float> out (static_cast<size_t> (total), 0.0f);
    size_t nextEvent = 0;

    for (int pos = 0; pos < total;)
    {
        int end = std::min (total, pos + kBlock);

        // Split blocks at events, just like the plugin does with MIDI.
        while (nextEvent < events.size())
        {
            const int eventSample = static_cast<int> (events[nextEvent].time * kSampleRate);

            if (eventSample > pos)
            {
                end = std::min (end, eventSample);
                break;
            }

            const auto& e = events[nextEvent++];
            if (e.velocity > 0.0f)
                engine.noteOn (e.note, e.velocity);
            else
                engine.noteOff (e.note);
        }

        const float* sc = sidechain != nullptr ? sidechain->data() + pos : nullptr;
        engine.process (out.data() + pos, nullptr, sc, nullptr, end - pos);
        pos = end;
    }

    return out;
}

double peak (const std::vector<float>& x, double from = 0.0, double to = 1.0e9)
{
    double p = 0.0;
    const size_t a = static_cast<size_t> (from * kSampleRate);
    const size_t b = std::min (x.size(), static_cast<size_t> (to * kSampleRate));
    for (size_t i = a; i < b; ++i)
        p = std::max (p, static_cast<double> (std::abs (x[i])));
    return p;
}

double rms (const std::vector<float>& x, double from, double to)
{
    double sum = 0.0;
    const size_t a = static_cast<size_t> (from * kSampleRate);
    const size_t b = std::min (x.size(), static_cast<size_t> (to * kSampleRate));
    for (size_t i = a; i < b; ++i)
        sum += static_cast<double> (x[i]) * x[i];
    return b > a ? std::sqrt (sum / static_cast<double> (b - a)) : 0.0;
}

// Frequency from rising zero crossings; fine for a clean sine.
double zeroCrossingHz (const std::vector<float>& x, double from, double to)
{
    const size_t a = static_cast<size_t> (from * kSampleRate);
    const size_t b = std::min (x.size(), static_cast<size_t> (to * kSampleRate));
    long first = -1, last = -1;
    int crossings = 0;

    for (size_t i = a + 1; i < b; ++i)
    {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            if (first < 0)
                first = static_cast<long> (i);
            else
                ++crossings;
            last = static_cast<long> (i);
        }
    }

    return crossings > 0 ? crossings * kSampleRate / static_cast<double> (last - first) : 0.0;
}

bool allFinite (const std::vector<float>& x)
{
    for (float v : x)
        if (! std::isfinite (v))
            return false;
    return true;
}

EngineSettings pureSine()
{
    EngineSettings s;
    s.voice.harmonics = 0.0;
    s.voice.clickLevel = 0.0;
    s.driveDb = 0.0;
    s.ceilingDb = 0.0;
    s.lowCutHz = 15.0;
    return s;
}

void writeWav (const std::string& path, const std::vector<float>& x)
{
    std::ofstream f (path, std::ios::binary);
    const auto u32 = [&f] (uint32_t v) { f.write (reinterpret_cast<const char*> (&v), 4); };
    const auto u16 = [&f] (uint16_t v) { f.write (reinterpret_cast<const char*> (&v), 2); };
    const uint32_t dataBytes = static_cast<uint32_t> (x.size() * 3);

    f.write ("RIFF", 4); u32 (36 + dataBytes); f.write ("WAVE", 4);
    f.write ("fmt ", 4); u32 (16); u16 (1); u16 (1); u32 (static_cast<uint32_t> (kSampleRate));
    u32 (static_cast<uint32_t> (kSampleRate) * 3); u16 (3); u16 (24);
    f.write ("data", 4); u32 (dataBytes);

    for (float v : x)
    {
        const auto s = static_cast<int32_t> (std::lround (std::clamp (static_cast<double> (v), -1.0, 1.0) * 8388607.0));
        const char bytes[3] = { static_cast<char> (s & 0xff), static_cast<char> ((s >> 8) & 0xff), static_cast<char> ((s >> 16) & 0xff) };
        f.write (bytes, 3);
    }
}

std::vector<float> kickTrack (double seconds, double bpm)
{
    std::vector<float> x (static_cast<size_t> (seconds * kSampleRate), 0.0f);
    const double beat = 60.0 / bpm;

    for (double start = 0.0; start < seconds; start += beat)
    {
        double phase = 0.0;
        for (int i = 0; i < static_cast<int> (0.25 * kSampleRate); ++i)
        {
            const size_t idx = static_cast<size_t> (start * kSampleRate) + static_cast<size_t> (i);
            if (idx >= x.size())
                break;
            const double t = i / kSampleRate;
            const double freq = 50.0 + 150.0 * std::exp (-t / 0.03);
            phase += freq / kSampleRate;
            x[idx] = static_cast<float> (0.9 * std::exp (-t / 0.12) * std::sin (2.0 * kPi * phase));
        }
    }

    return x;
}

// Tight metal-style kicks at a fixed interval between start and end.
std::vector<float> kickRun (double seconds, double start, double end, double interval, double decaySeconds = 0.04)
{
    std::vector<float> x (static_cast<size_t> (seconds * kSampleRate), 0.0f);

    for (double t0 = start; t0 < end; t0 += interval)
    {
        double phase = 0.0;
        for (int i = 0; i < static_cast<int> (0.2 * kSampleRate); ++i)
        {
            const size_t idx = static_cast<size_t> (t0 * kSampleRate) + static_cast<size_t> (i);
            if (idx >= x.size())
                break;
            const double t = i / kSampleRate;
            phase += (60.0 + 120.0 * std::exp (-t / 0.015)) / kSampleRate;
            x[idx] += static_cast<float> (0.9 * std::exp (-t / decaySeconds) * std::sin (2.0 * kPi * phase));
        }
    }

    return x;
}

// Gain of `processed` relative to `reference` in consecutive 10 ms windows, in dB.
std::vector<double> gainCurveDb (const std::vector<float>& processed, const std::vector<float>& reference, double from, double to)
{
    std::vector<double> g;
    for (double t = from; t + 0.01 <= to; t += 0.01)
        g.push_back (gainToDb (rms (processed, t, t + 0.01)) - gainToDb (rms (reference, t, t + 0.01)));
    return g;
}

// Level of one frequency in a signal (single-bin DFT), in dB.
double toneLevelDb (const std::vector<float>& x, double freq, double from, double to)
{
    const size_t a = static_cast<size_t> (from * kSampleRate);
    const size_t b = std::min (x.size(), static_cast<size_t> (to * kSampleRate));
    double re = 0.0, im = 0.0;
    for (size_t i = a; i < b; ++i)
    {
        const double ph = 2.0 * kPi * freq * static_cast<double> (i) / kSampleRate;
        re += x[i] * std::cos (ph);
        im += x[i] * std::sin (ph);
    }
    return gainToDb (2.0 * std::sqrt (re * re + im * im) / static_cast<double> (b - a));
}

std::vector<float> sines (double seconds, std::initializer_list<std::pair<double, double>> partials)
{
    std::vector<float> x (static_cast<size_t> (seconds * kSampleRate), 0.0f);
    for (size_t i = 0; i < x.size(); ++i)
        for (auto [freq, amp] : partials)
            x[i] += static_cast<float> (amp * std::sin (2.0 * kPi * freq * static_cast<double> (i) / kSampleRate));
    return x;
}

std::vector<float> runBassDuck (const BassDuckSettings& settings, std::vector<float> bass, const std::vector<float>* key)
{
    BassDuck duck;
    duck.prepare (kSampleRate);
    duck.setSettings (settings);
    for (size_t pos = 0; pos < bass.size(); pos += kBlock)
    {
        const int n = static_cast<int> (std::min<size_t> (kBlock, bass.size() - pos));
        duck.process (bass.data() + pos, nullptr, key != nullptr ? key->data() + pos : nullptr, nullptr, n);
    }
    return bass;
}

struct Stereo
{
    std::vector<float> left, right;
};

Stereo renderStereo (const EngineSettings& settings, const std::vector<NoteEvent>& events, double seconds)
{
    SubEngine engine;
    engine.prepare (kSampleRate);
    engine.setSettings (settings);

    const int total = static_cast<int> (seconds * kSampleRate);
    Stereo out { std::vector<float> (static_cast<size_t> (total)), std::vector<float> (static_cast<size_t> (total)) };
    size_t nextEvent = 0;

    for (int pos = 0; pos < total;)
    {
        int end = std::min (total, pos + kBlock);
        while (nextEvent < events.size())
        {
            const int at = static_cast<int> (events[nextEvent].time * kSampleRate);
            if (at > pos) { end = std::min (end, at); break; }
            const auto& e = events[nextEvent++];
            if (e.velocity > 0.0f) engine.noteOn (e.note, e.velocity); else engine.noteOff (e.note);
        }
        engine.process (out.left.data() + pos, out.right.data() + pos, nullptr, nullptr, end - pos);
        pos = end;
    }
    return out;
}

std::vector<float> filtered (const std::vector<float>& x, double freq, bool highPass)
{
    Butterworth4 f;
    if (highPass) f.setHighPass (kSampleRate, freq); else f.setLowPass (kSampleRate, freq);
    std::vector<float> y (x.size());
    for (size_t i = 0; i < x.size(); ++i)
        y[i] = static_cast<float> (f.process (x[i]));
    return y;
}

int failures = 0;

void check (bool ok, const std::string& name, const std::string& detail)
{
    std::printf ("[%s] %s  (%s)\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    if (! ok)
        ++failures;
}

std::string fmt (const char* format, double a, double b = 0.0)
{
    char buf[128];
    std::snprintf (buf, sizeof (buf), format, a, b);
    return buf;
}

} // namespace

int main (int argc, char** argv)
{
    const std::string outDir = argc > 1 ? argv[1] : "";

    // 1. A hot, harmonics-heavy, clicky boom still never passes the ceiling.
    {
        EngineSettings s;
        s.driveDb = 18.0;
        s.voice.harmonics = 1.0;
        s.voice.autoHarmonics = 1.0;
        s.voice.clickLevel = 1.0;
        s.ceilingDb = -6.0;
        const auto x = render (s, { { 0.0, 29, 1.0f } }, 2.0);
        const double ceiling = dbToGain (-6.0);
        check (allFinite (x) && peak (x) <= ceiling + 1.0e-6, "boom peak stays under ceiling",
               fmt ("peak %.2f dBFS, ceiling %.2f dBFS", gainToDb (peak (x)), -6.0));
    }

    // 2. Drop falls from the played note by the set amount, over the tempo-synced length.
    {
        EngineSettings s = pureSine();
        s.voice.mode = Mode::drop;
        s.voice.dropSemis = 24.0;
        s.dropBeats = 4.0;
        const auto x = render (s, { { 0.0, 48, 1.0f } }, 3.0, 120.0); // 4 beats @120 = 2 s
        const double startHz = zeroCrossingHz (x, 0.0, 0.1);
        const double endHz = zeroCrossingHz (x, 2.05, 2.6);
        check (std::abs (startHz - 130.8) < 8.0 && std::abs (endHz - 32.7) < 2.5, "drop pitch C3 -> C1 over 4 beats",
               fmt ("start %.1f Hz, end %.1f Hz", startHz, endHz));
    }

    // 3. Drop length follows the host tempo.
    {
        EngineSettings s = pureSine();
        s.voice.mode = Mode::drop;
        const auto fast = render (s, { { 0.0, 48, 1.0f } }, 2.6, 120.0);
        const auto slow = render (s, { { 0.0, 48, 1.0f } }, 2.6, 60.0);
        const double fastHz = zeroCrossingHz (fast, 2.05, 2.55);
        const double slowHz = zeroCrossingHz (slow, 2.05, 2.55);
        check (slowHz > fastHz * 1.5, "drop length syncs to tempo",
               fmt ("at 2 s: %.1f Hz @120 bpm vs %.1f Hz @60 bpm", fastHz, slowHz));
    }

    // 4. Boom pitch settles onto the played note after the punch.
    {
        EngineSettings s = pureSine();
        s.voice.decayMs = 3000.0;
        const auto x = render (s, { { 0.0, 33, 1.0f } }, 1.0); // A1 = 55 Hz
        const double hz = zeroCrossingHz (x, 0.4, 0.9);
        check (std::abs (hz - 55.0) < 1.0, "boom lands on the played note", fmt ("%.2f Hz (expect 55.00)", hz));
    }

    // 5. Harmonics make a low boom audible on a phone-sized speaker.
    {
        EngineSettings dry = pureSine();
        dry.phoneCheck = true;
        EngineSettings wet = dry;
        wet.voice.harmonics = 0.5;
        const auto a = render (dry, { { 0.0, 24, 1.0f } }, 1.0);
        const auto b = render (wet, { { 0.0, 24, 1.0f } }, 1.0);
        const double gainDb = gainToDb (rms (b, 0.1, 0.8)) - gainToDb (rms (a, 0.1, 0.8));
        check (gainDb > 12.0, "harmonics carry a C1 boom above 150 Hz", fmt ("+%.1f dB on phone check", gainDb));
    }

    // 6. Auto harmonics add more as the pitch drops.
    {
        EngineSettings off = pureSine();
        off.phoneCheck = true;
        off.voice.harmonics = 0.4;
        off.voice.autoHarmonics = 0.0;
        EngineSettings on = off;
        on.voice.autoHarmonics = 1.0;
        const auto a = render (off, { { 0.0, 26, 1.0f } }, 1.0);
        const auto b = render (on, { { 0.0, 26, 1.0f } }, 1.0);
        const double gainDb = gainToDb (rms (b, 0.1, 0.8)) - gainToDb (rms (a, 0.1, 0.8));
        check (gainDb > 4.0, "auto harmonics boost low notes", fmt ("+%.1f dB at D1", gainDb));
    }

    // 7. A new note mid-cycle chokes the old one without a click: the steepest
    //    sample-to-sample jump is no worse than a clean hit starting from silence.
    {
        EngineSettings s = pureSine();
        s.ceilingDb = -6.0;
        const auto maxStep = [] (const std::vector<float>& x)
        {
            double m = 0.0;
            for (size_t i = 1; i < x.size(); ++i)
                m = std::max (m, static_cast<double> (std::abs (x[i] - x[i - 1])));
            return m;
        };
        const double choked = maxStep (render (s, { { 0.0, 31, 1.0f }, { 0.2137, 36, 1.0f } }, 0.6));
        const double clean = maxStep (render (s, { { 0.0, 36, 1.0f } }, 0.6));
        check (choked <= clean * 1.05, "retrigger has no click",
               fmt ("max step %.4f vs %.4f for a clean hit", choked, clean));
    }

    // 8. Sub-audible energy is removed by the low cut.
    {
        EngineSettings s = pureSine();
        s.lowCutHz = 30.0;
        s.voice.mode = Mode::drop;
        s.voice.dropSemis = 36.0;
        s.dropBeats = 2.0;
        const auto x = render (s, { { 0.0, 36, 1.0f } }, 3.0); // ends near 12 Hz
        const double tailDb = gainToDb (rms (x, 1.5, 2.5));
        check (tailDb < -30.0, "low cut removes infrasonic tail", fmt ("tail RMS %.1f dBFS", tailDb));
    }

    // 9. The kick sidechain ducks the sub.
    {
        EngineSettings s = pureSine();
        s.voice.mode = Mode::drop;
        s.voice.dropSemis = 1.0;
        s.dropBeats = 16.0;
        s.duckDepthDb = 8.0;
        const auto kick = kickTrack (2.0, 120.0);
        const auto plain = render (s, { { 0.0, 36, 1.0f } }, 2.0);
        const auto ducked = render (s, { { 0.0, 36, 1.0f } }, 2.0, 120.0, &kick);
        const double reduction = gainToDb (rms (plain, 0.51, 0.56)) - gainToDb (rms (ducked, 0.51, 0.56));
        const double recovered = gainToDb (rms (plain, 0.95, 0.99)) - gainToDb (rms (ducked, 0.95, 0.99));
        check (reduction > 6.0 && recovered < 2.0, "kick ducks the sub, then it recovers",
               fmt ("-%.1f dB on the kick, -%.1f dB before the next", reduction, recovered));
    }

    // 10. One-shot off: note length gates the boom.
    {
        EngineSettings s = pureSine();
        s.voice.oneShot = false;
        s.voice.releaseMs = 50.0;
        s.voice.decayMs = 3000.0;
        const auto x = render (s, { { 0.0, 36, 1.0f }, { 0.3, 36, 0.0f } }, 1.0);
        check (rms (x, 0.5, 1.0) < 1.0e-4, "gated boom stops after note off", fmt ("RMS after release %.6f", rms (x, 0.5, 1.0)));
    }

    // 11. Glide slides between overlapping notes without restarting.
    {
        EngineSettings s = pureSine();
        s.voice.glideMs = 80.0;
        s.voice.decayMs = 4000.0;
        const auto x = render (s, { { 0.0, 36, 1.0f }, { 0.4, 43, 1.0f } }, 1.0);
        const double hz = zeroCrossingHz (x, 0.6, 0.95);
        check (std::abs (hz - 98.0) < 2.0, "glide lands on the new note", fmt ("%.1f Hz (expect 98.0)", hz));
    }

    // 12. The "Long 30 Hz Boom" preset settings: B-1 at -49 cents lands on 30 Hz
    //     after a fast dive (the reference averages ~75 Hz over its first 25 ms),
    //     and still sustains a second later.
    {
        EngineSettings s;
        s.tuneSemis = -0.49;
        s.voice.punchSemis = 28.0;
        s.voice.punchMs = 22.0;
        s.voice.decayMs = 8000.0;
        s.voice.oneShot = false;
        s.voice.releaseMs = 150.0;
        s.voice.clickLevel = 0.0;
        s.voice.harmonics = 0.0;
        s.lowCutHz = 18.0;
        s.driveDb = -3.0;
        s.ceilingDb = -4.0;
        const auto x = render (s, { { 0.0, 23, 1.0f }, { 2.4, 23, 0.0f } }, 3.0);
        const double startHz = zeroCrossingHz (x, 0.0, 0.025);
        const double settledHz = zeroCrossingHz (x, 0.4, 1.4);
        const double sustainDb = gainToDb (rms (x, 1.0, 1.1)) - gainToDb (rms (x, 0.15, 0.25));
        check (startHz > 60.0 && std::abs (settledHz - 30.0) < 0.3 && sustainDb > -8.0, "long 30 Hz boom preset",
               fmt ("first 25 ms avg %.0f Hz, settles at %.2f Hz", startHz, settledHz) + fmt (", %.1f dB after 1 s", sustainDb));
    }

    // 13. Roll Hold: a double-kick run ducks fully on its first hit, then holds a
    //     steady light reduction instead of pumping, and normal ducking returns after.
    {
        EngineSettings s = pureSine();
        s.voice.mode = Mode::drop;
        s.voice.dropSemis = 1.0;
        s.dropBeats = 16.0;
        s.duckDepthDb = 8.0;
        auto kicks = kickRun (3.2, 0.5, 2.0, 60.0 / 180.0 / 4.0); // 16ths at 180 bpm
        const auto single = kickRun (3.2, 2.6, 2.61, 1.0);
        for (size_t i = 0; i < kicks.size(); ++i)
            kicks[i] += single[i];

        const auto plain = render (s, { { 0.0, 36, 1.0f } }, 3.2);
        const auto buried = render (s, { { 0.0, 36, 1.0f } }, 3.2, 120.0, &kicks);
        s.duckReleaseMs = 40.0;   // a short release and higher threshold pump instead
        s.duckThresholdDb = -12.0;
        const auto pumping = render (s, { { 0.0, 36, 1.0f } }, 3.2, 120.0, &kicks);
        s.duckReleaseMs = 120.0;
        s.duckThresholdDb = -30.0;
        s.rollHold = true;
        s.rollDepthDb = 2.0;
        const auto held = render (s, { { 0.0, 36, 1.0f } }, 3.2, 120.0, &kicks);

        const auto range = [] (const std::vector<double>& g)
        {
            return *std::max_element (g.begin(), g.end()) - *std::min_element (g.begin(), g.end());
        };
        const auto mean = [] (const std::vector<double>& g)
        {
            double sum = 0.0;
            for (double v : g) sum += v;
            return sum / static_cast<double> (g.size());
        };

        const auto buriedCurve = gainCurveDb (buried, plain, 1.0, 1.9);
        const auto pumpCurve = gainCurveDb (pumping, plain, 1.0, 1.9);
        const auto holdCurve = gainCurveDb (held, plain, 1.0, 1.9);
        const auto deepest = [] (const std::vector<double>& g) { return -*std::min_element (g.begin(), g.end()); };
        const double firstHit = deepest (gainCurveDb (held, plain, 0.5, 0.56));
        const double afterRun = -mean (gainCurveDb (held, plain, 2.4, 2.55));
        const double laterHit = deepest (gainCurveDb (held, plain, 2.6, 2.66));

        check (-mean (buriedCurve) > 6.0 && range (pumpCurve) > 3.0 && range (holdCurve) < 1.5 && std::abs (-mean (holdCurve) - 2.0) < 1.0
                   && firstHit > 6.0 && afterRun < 1.0 && laterHit > 6.0,
               "roll hold stops double-kick pumping",
               fmt ("without it: buried at -%.1f dB or pumping %.1f dB", -mean (buriedCurve), range (pumpCurve))
                   + fmt ("; with it: %.1f dB swing", range (holdCurve))
                   + fmt (", held at -%.1f dB, first hit -%.1f dB", -mean (holdCurve), firstHit)
                   + fmt (", after run -%.1f dB, next single kick -%.1f dB", afterRun, laterHit));
    }

    // 14. SubForge Duck is transparent while nothing is ducking: every
    //     frequency comes out at the level it went in.
    {
        BassDuckSettings s;
        double worst = 0.0;
        for (double freq : { 30.0, 60.0, 100.0, 150.0, 400.0, 2000.0 })
        {
            const auto in = sines (1.0, { { freq, 0.5 } });
            const auto out = runBassDuck (s, in, nullptr);
            worst = std::max (worst, std::abs (toneLevelDb (out, freq, 0.5, 1.0) - toneLevelDb (in, freq, 0.5, 1.0)));
        }
        check (worst < 0.05, "bass duck is transparent when idle", fmt ("worst level change %.3f dB", worst));
    }

    // 15. Keyed by a SubForge boom, it ducks only the bass below the split.
    {
        BassDuckSettings s;
        s.splitHz = 100.0;
        s.duck.depthDb = 8.0;
        const auto bass = sines (2.0, { { 45.0, 0.4 }, { 400.0, 0.2 } });
        auto boom = sines (2.0, { { 31.0, 0.5 } });
        for (size_t i = 0; i < static_cast<size_t> (kSampleRate); ++i)
            boom[i] = 0.0f; // boom starts at 1 s
        const auto out = runBassDuck (s, bass, &boom);
        const double lowBefore = toneLevelDb (out, 45.0, 0.5, 0.95) - toneLevelDb (bass, 45.0, 0.5, 0.95);
        const double lowDuring = toneLevelDb (out, 45.0, 1.3, 1.9) - toneLevelDb (bass, 45.0, 1.3, 1.9);
        const double highDuring = toneLevelDb (out, 400.0, 1.3, 1.9) - toneLevelDb (bass, 400.0, 1.3, 1.9);
        check (std::abs (lowBefore) < 0.1 && lowDuring < -6.0 && std::abs (highDuring) < 0.3,
               "bass duck cuts only the low end under the boom",
               fmt ("45 Hz: %.1f dB before, %.1f dB during", lowBefore, lowDuring) + fmt ("; 400 Hz during: %+.2f dB", highDuring));
    }

    // 16. The impact layer adds a wide, bright burst and tail on top of the boom
    //     without touching the sub below 100 Hz, and the ceiling still holds.
    {
        EngineSettings dry;
        dry.voice.punchSemis = 24.0;
        dry.voice.decayMs = 1800.0;
        EngineSettings wet = dry;
        wet.impact = { 0.6, 4000.0, 400.0, 0.5 };

        const auto a = renderStereo (dry, { { 0.0, 29, 1.0f } }, 1.5);
        const auto b = renderStereo (wet, { { 0.0, 29, 1.0f } }, 1.5);

        const double addedHighDb = gainToDb (rms (filtered (b.left, 1000.0, true), 0.0, 0.5))
                                   - gainToDb (rms (filtered (a.left, 1000.0, true), 0.0, 0.5));
        const double lowChangeDb = std::abs (gainToDb (rms (filtered (b.left, 80.0, false), 0.1, 1.0))
                                             - gainToDb (rms (filtered (a.left, 80.0, false), 0.1, 1.0)));

        const auto hl = filtered (b.left, 1000.0, true), hr = filtered (b.right, 1000.0, true);
        double lr = 0.0, ll = 0.0, rr = 0.0;
        for (size_t i = static_cast<size_t> (0.2 * kSampleRate); i < static_cast<size_t> (0.6 * kSampleRate); ++i)
        {
            lr += hl[i] * hr[i]; ll += hl[i] * hl[i]; rr += hr[i] * hr[i];
        }
        const double correlation = lr / std::sqrt (ll * rr);
        const double ceiling = dbToGain (wet.ceilingDb);

        check (addedHighDb > 10.0 && lowChangeDb < 1.0 && correlation < 0.9
                   && std::max (peak (b.left), peak (b.right)) <= ceiling + 1.0e-6,
               "impact layer adds a wide top without touching the sub",
               fmt ("+%.0f dB above 1 kHz, sub below 80 Hz changes %.2f dB", addedHighDb, lowChangeDb)
                   + fmt (", L/R tail correlation %.2f, peak %.2f dBFS", correlation, gainToDb (std::max (peak (b.left), peak (b.right)))));
    }

    // 17. Full-band duck turns the whole signal down (for a guitar or music bus).
    {
        BassDuckSettings s;
        s.fullBand = true;
        s.duck.depthDb = 4.0;
        const auto bus = sines (2.0, { { 45.0, 0.3 }, { 400.0, 0.3 }, { 2000.0, 0.2 } });
        auto boom = sines (2.0, { { 31.0, 0.5 } });
        for (size_t i = 0; i < static_cast<size_t> (kSampleRate); ++i)
            boom[i] = 0.0f;
        const auto out = runBassDuck (s, bus, &boom);
        double worst = 0.0;
        for (double freq : { 45.0, 400.0, 2000.0 })
            worst = std::max (worst, std::abs (toneLevelDb (out, freq, 1.3, 1.9) - toneLevelDb (bus, freq, 1.3, 1.9) + 4.0));
        const double before = toneLevelDb (out, 400.0, 0.5, 0.95) - toneLevelDb (bus, 400.0, 0.5, 0.95);
        check (worst < 0.5 && std::abs (before) < 0.1, "full-band duck dips everything evenly",
               fmt ("every band within %.2f dB of -4 dB during the boom; %+.2f dB before it", worst, before));
    }

    // 18. Look-ahead empties the low end before the hit: the bass fades down
    //     over the first half of the window, sits at full depth for the gap,
    //     and the boom lands on an already-clear low end. The track comes out
    //     exactly the reported latency late, so the host can line it back up.
    {
        BassDuckSettings s;
        s.splitHz = 100.0;
        s.duck.depthDb = 12.0;
        s.lookAheadMs = 250.0;  // an 8th note at 120 BPM
        s.gap = 0.5;
        const double hit = 1.5;
        const auto bass = sines (3.0, { { 45.0, 0.4 }, { 400.0, 0.2 } });
        auto boom = sines (3.0, { { 31.0, 0.5 } });
        for (size_t i = 0; i < static_cast<size_t> (hit * kSampleRate); ++i)
            boom[i] = 0.0f;

        BassDuck probe;
        probe.prepare (kSampleRate);
        probe.setSettings (s);
        const int latency = probe.latencySamples();

        auto out = runBassDuck (s, bass, &boom);
        out.erase (out.begin(), out.begin() + latency);  // undo the delay, as the host does

        const auto lowDb = [&] (double from, double to)
        { return toneLevelDb (out, 45.0, from, to) - toneLevelDb (bass, 45.0, from, to); };
        const double early = lowDb (hit - 0.6, hit - 0.3);      // before the window
        const double fading = lowDb (hit - 0.21, hit - 0.15);   // inside the fade
        const double gapDb = lowDb (hit - 0.10, hit - 0.01);    // the gap right before the hit
        const double highGap = toneLevelDb (out, 400.0, hit - 0.10, hit - 0.01) - toneLevelDb (bass, 400.0, hit - 0.10, hit - 0.01);

        check (latency == lookAheadLatencySamples (kSampleRate) && std::abs (early) < 0.3 && fading < -1.0
                   && fading > gapDb + 2.0 && gapDb < -9.0 && std::abs (highGap) < 0.3,
               "look-ahead clears the low end before the hit",
               fmt ("45 Hz: %+.1f dB early, %.1f dB fading", early, fading) + fmt (", %.1f dB in the gap; 400 Hz %+.2f dB", gapDb, highGap));

        BassDuckSettings off = s;
        off.lookAheadMs = 0.0;
        const auto plain = runBassDuck (off, bass, &boom);
        const double plainGap = toneLevelDb (plain, 45.0, hit - 0.10, hit - 0.01) - toneLevelDb (bass, 45.0, hit - 0.10, hit - 0.01);
        check (std::abs (plainGap) < 0.3, "without look-ahead nothing moves before the hit", fmt ("45 Hz in the gap %+.2f dB", plainGap));
    }

    if (! outDir.empty())
    {
        EngineSettings boom;
        writeWav (outDir + "/boom_F1.wav", render (boom, { { 0.0, 29, 1.0f } }, 2.0));

        EngineSettings drop;
        drop.voice.mode = Mode::drop;
        drop.dropBeats = 8.0;
        writeWav (outDir + "/drop_C3_to_C1_140bpm.wav", render (drop, { { 0.0, 48, 1.0f } }, 5.0, 140.0));

        const auto kick = kickTrack (4.0, 140.0);
        EngineSettings pattern;
        pattern.voice.decayMs = 1500.0;
        auto bass = render (pattern, { { 0.0, 29, 1.0f }, { 0.857, 29, 0.8f }, { 1.714, 32, 1.0f }, { 2.571, 27, 0.9f } },
                            4.0, 140.0, &kick);
        for (size_t i = 0; i < bass.size(); ++i)
            bass[i] = static_cast<float> (bass[i] * 0.7 + kick[i] * 0.5);
        writeWav (outDir + "/808_pattern_with_kick_140bpm.wav", bass);

        std::printf ("Demo renders written to %s\n", outDir.c_str());
    }

    std::printf ("%s: %d failure(s)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
