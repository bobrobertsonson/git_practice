// Headless checks of the RASP plugin class (no DAW needed):
//   1. plugin output == the bare DSP engine (to -100 dB: presets pass through
//      the host's 0..1 parameter scale, which rounds in the last bit)
//   2. host block size doesn't change the output (Logic varies it)
//   3. 44.1 .. 192 kHz: finite output, same loudness, delay buffers don't wrap
//   4. presets + saved state restore the same sound
//   5. mono-in/mono-out sums correctly
//   6. the editor builds; a snapshot is written to argv[1] if given
#include "PluginProcessor.h"
#include "presets.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdio>
#include <random>

static int failures = 0;
static void check(bool ok, const char* what, double v = 0) {
    std::printf("%s  %-58s %g\n", ok ? "PASS" : "FAIL", what, v);
    if (!ok) failures++;
}

// A scream-ish test signal: bright buzz + noise in phrases, peaks near -6 dBFS.
static std::vector<float> testSignal(double sr, double seconds) {
    std::vector<float> x((size_t)(sr * seconds));
    std::mt19937 rng(7); std::normal_distribution<float> nd(0.f, 1.f);
    double ph = 0; float lp = 0;
    for (size_t i = 0; i < x.size(); i++) {
        double t = i / sr;
        double f = 190 + 25 * std::sin(2 * M_PI * 3.1 * t);
        ph += f / sr; if (ph >= 1) ph -= 1;
        float saw = (float)(2 * ph - 1), n = nd(rng);
        lp += 0.3f * (saw + 0.5f * n - lp);
        float env = std::fmod(t, 1.5) < 1.0 ? (float)std::min(1.0, std::fmod(t, 1.5) * 20) : 0.f;
        x[i] = 0.3f * env * (saw + 0.5f * n - 0.6f * lp);
    }
    return x;
}

struct Out { std::vector<float> L, R; };

static Out runPlugin(RaspProcessor& p, const std::vector<float>& in, double sr, int inCh, int outCh,
                     std::function<int()> blockSize) {
    juce::AudioProcessor::BusesLayout lay;
    lay.inputBuses.add(inCh == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
    lay.outputBuses.add(outCh == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
    p.setBusesLayout(lay);
    p.prepareToPlay(sr, 4096);
    Out o; o.L.resize(in.size()); o.R.resize(in.size());
    juce::MidiBuffer midi;
    size_t pos = 0;
    while (pos < in.size()) {
        int n = std::min<int>(blockSize(), (int)(in.size() - pos));
        juce::AudioBuffer<float> b(std::max(inCh, outCh), n);
        for (int c = 0; c < inCh; c++) b.copyFrom(c, 0, in.data() + pos, n);
        p.processBlock(b, midi);
        for (int i = 0; i < n; i++) { o.L[pos + i] = b.getSample(0, i); o.R[pos + i] = b.getSample(outCh > 1 ? 1 : 0, i); }
        pos += n;
    }
    return o;
}

static void setParam(RaspProcessor& p, const char* id, float v) {
    auto* prm = p.apvts.getParameter(id);
    prm->setValueNotifyingHost(prm->convertTo0to1(v));
}

static double rmsDb(const std::vector<float>& x) {
    double s = 0; for (float v : x) s += (double)v * v;
    return 10 * std::log10(s / x.size() + 1e-30);
}
static bool finite(const Out& o) {
    for (size_t i = 0; i < o.L.size(); i++) if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) return false;
    return true;
}
static double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0; for (size_t i = 0; i < a.size(); i++) m = std::max(m, (double)std::fabs(a[i] - b[i])); return m;
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = 48000;
    auto in = testSignal(sr, 8.0);

    // 1. Plugin == engine (preset "huge", tempo fixed, guard off as in the plugin default).
    rasp::Params ref = rasp::presetParams(0);
    ref.howl = false;
    auto eng = std::make_unique<rasp::Processor>();
    eng->init((float)sr); eng->setParams(ref);
    Out e; e.L.resize(in.size()); e.R.resize(in.size());
    for (size_t i = 0; i < in.size(); i++) eng->process(in[i], e.L[i], e.R[i]);

    RaspProcessor a;
    a.setCurrentProgram(0);
    setParam(a, "sync", 0.f);
    Out pa = runPlugin(a, in, sr, 1, 2, [] { return 512; });
    check(maxDiff(pa.L, e.L) < 1e-5 && maxDiff(pa.R, e.R) < 1e-5, "plugin == engine within -100 dB (huge, 512 blocks)", maxDiff(pa.L, e.L));

    // 2. Random block sizes 1..2048.
    std::mt19937 rng(1); std::uniform_int_distribution<int> bs(1, 2048);
    RaspProcessor b; b.setCurrentProgram(0); setParam(b, "sync", 0.f);
    Out pb = runPlugin(b, in, sr, 1, 2, [&] { return bs(rng); });
    check(maxDiff(pb.L, pa.L) == 0 && maxDiff(pb.R, pa.R) == 0, "random block sizes 1..2048 give identical output", maxDiff(pb.L, pa.L));

    // 3. Sample rates. Loudness within 1.5 dB of 48k, and the dotted-1/8 delay
    //    at 40 bpm (1.125 s, the longest echo) lands where it should.
    const double base = rmsDb(pa.L);
    for (double rate : {44100.0, 88200.0, 96000.0, 176400.0, 192000.0}) {
        auto x = testSignal(rate, 8.0);
        RaspProcessor c; c.setCurrentProgram(0); setParam(c, "sync", 0.f);
        Out o = runPlugin(c, x, rate, 1, 2, [] { return 256; });
        char msg[96];
        std::snprintf(msg, sizeof msg, "%.1f kHz: finite, loudness vs 48k (dB)", rate / 1000);
        double d = rmsDb(o.L) - base;
        check(finite(o) && std::fabs(d) < 1.5, msg, d);

        // Echo time: click in, delay only, find the first repeat.
        RaspProcessor q;
        q.setCurrentProgram(1);
        for (auto* id : {"controlOn", "thickenOn", "sync", "duck", "tailBlend", "bypassMatch"}) setParam(q, id, 0.f);
        setParam(q, "delayBlend", 1.f); setParam(q, "repeats", 0.f); setParam(q, "glue", 0.f);
        setParam(q, "bpm", 40.f); setParam(q, "delayTime", 1.f);
        std::vector<float> click((size_t)(rate * 2.0), 0.f); click[100] = 0.5f;
        Out qo = runPlugin(q, click, rate, 1, 2, [] { return 512; });
        size_t peak = 0; float pv = 0;
        for (size_t i = (size_t)(rate * 0.5); i < qo.L.size(); i++) {
            float v = std::fabs(qo.L[i]) + std::fabs(qo.R[i]);
            if (v > pv) { pv = v; peak = i; }
        }
        double echo = (peak - 100) / rate;
        std::snprintf(msg, sizeof msg, "%.1f kHz: dotted-1/8 echo @ 40 bpm (s, want 1.125)", rate / 1000);
        check(std::fabs(echo - 1.125) < 0.01, msg, echo);
    }

    // 4. Preset + state round trip.
    RaspProcessor s1; s1.setCurrentProgram(2); setParam(s1, "drive", 0.83f); setParam(s1, "sync", 0.f);
    juce::MemoryBlock blob; s1.getStateInformation(blob);
    RaspProcessor s2; s2.setStateInformation(blob.getData(), (int)blob.getSize());
    check(s2.getCurrentProgram() == 2, "state restores the preset number", s2.getCurrentProgram());
    Out o1 = runPlugin(s1, in, sr, 1, 2, [] { return 512; });
    Out o2 = runPlugin(s2, in, sr, 1, 2, [] { return 512; });
    check(maxDiff(o1.L, o2.L) == 0, "saved state reproduces the sound exactly", maxDiff(o1.L, o2.L));
    check(maxDiff(o1.L, pa.L) > 1e-3, "different preset sounds different", maxDiff(o1.L, pa.L));

    // 5. Mono out == mid of the engine in mono mode; stereo in == mono in of the mid.
    rasp::Params mref = rasp::presetParams(0); mref.howl = false; mref.mono = true;
    eng = std::make_unique<rasp::Processor>(); eng->init((float)sr); eng->setParams(mref);
    std::vector<float> mid(in.size());
    for (size_t i = 0; i < in.size(); i++) { float L, R; eng->process(in[i], L, R); mid[i] = 0.5f * (L + R); }
    RaspProcessor m; m.setCurrentProgram(0); setParam(m, "sync", 0.f);
    Out mo = runPlugin(m, in, sr, 1, 1, [] { return 512; });
    check(maxDiff(mo.L, mid) < 1e-5, "mono in/mono out == engine mono mode (-100 dB)", maxDiff(mo.L, mid));
    RaspProcessor st; st.setCurrentProgram(0); setParam(st, "sync", 0.f);
    Out so = runPlugin(st, in, sr, 2, 2, [] { return 512; });
    check(maxDiff(so.L, pa.L) == 0, "stereo in (dual mono) == mono in", maxDiff(so.L, pa.L));

    // 6. Bypass parameter is the host bypass, and it's level-matched (not silent).
    RaspProcessor by; by.setCurrentProgram(0); setParam(by, "sync", 0.f);
    check(by.getBypassParameter() == by.apvts.getParameter("bypass"), "host bypass button = level-matched bypass");
    setParam(by, "bypass", 1.f);
    Out bo = runPlugin(by, in, sr, 1, 2, [] { return 512; });
    check(std::fabs(rmsDb(bo.L) - base) < 3.0, "bypassed loudness within 3 dB of effect (dB)", rmsDb(bo.L) - base);

    // Editor.
    {
        RaspProcessor ep;
        std::unique_ptr<juce::AudioProcessorEditor> ed(ep.createEditor());
        check(ed != nullptr && ed->getWidth() > 0, "editor builds", ed ? ed->getWidth() : 0);
        if (ed && argc > 1) {
            auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f);
            juce::File f(juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]));
            f.deleteFile();
            juce::FileOutputStream os(f);
            juce::PNGImageFormat().writeImageToStream(img, os);
            std::printf("wrote %s\n", f.getFullPathName().toRawUTF8());
        }
    }

    std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
