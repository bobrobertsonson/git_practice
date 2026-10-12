// Offline tests for the SubForge DSP. No JUCE needed: builds and runs anywhere.
//
//   subforge_tests [output-dir]
//
// With an output dir, demo renders are also written as 24-bit WAV files.

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
