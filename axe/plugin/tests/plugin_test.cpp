// Headless test of the plugin class (what Logic and an LV2 host see).
//   plugin_test [snapshot_dir]   (snapshot_dir: PNGs of the editor on every floor)
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void setParam(AxeProcessor& p, const char* id, float realValue) {
    auto* prm = p.apvts.getParameter(id);
    prm->setValueNotifyingHost(prm->convertTo0to1(realValue));
}

static std::vector<float> pluck(double sr, double seconds, float f0, float amp = 0.25f, int harmonics = 10) {
    std::vector<float> x((size_t)(sr * seconds), 0.f);
    for (int k = 0; k * 0.5 < seconds; k++)
        for (int h = 1; h <= harmonics && f0 * h < sr * 0.45; h++)
            for (size_t i = (size_t)(k * 0.5 * sr); i < x.size() && i < (size_t)((k * 0.5 + 1.2) * sr); i++) {
                double t = (double)i / sr - k * 0.5;
                x[i] += (float)(amp / (harmonics > 10 ? h : h * h) * std::exp(-(2.0 + 0.2 * h) * t) * std::sin(2.0 * M_PI * f0 * h * t + h));
            }
    return x;
}

// Runs x through the processor in host-sized blocks, mono in/out.
static std::vector<float> run(AxeProcessor& p, const std::vector<float>& x, int block,
                              std::function<void(size_t)> atBlock = nullptr) {
    std::vector<float> y(x.size());
    juce::AudioBuffer<float> buf(1, block);
    juce::MidiBuffer midi;
    for (size_t i = 0; i < x.size(); i += (size_t)block) {
        if (atBlock) atBlock(i);
        const int n = (int)std::min<size_t>((size_t)block, x.size() - i);
        buf.setSize(1, n, false, false, true);
        buf.copyFrom(0, 0, &x[i], n);
        p.processBlock(buf, midi);
        std::memcpy(&y[i], buf.getReadPointer(0), sizeof(float) * (size_t)n);
    }
    return y;
}

static void testLayout() {
    AxeProcessor p;
    const auto& specs = paramSpecs();
    std::set<std::string> ids;
    std::map<std::string, int> perPage;
    for (const auto& s : specs) { ids.insert(s.id); perPage[s.page]++; }
    CHECK(ids.size() == specs.size(), "duplicate parameter ids");
    // the Anagram pages strictly by 6 in port order: every page exactly 6, contiguous
    for (auto& [page, n] : perPage) CHECK(n == 6, "page %s has %d parameters (want exactly 6)", page.c_str(), n);
    std::string last; std::set<std::string> seen;
    for (const auto& s : specs) {
        if (s.page != last) { CHECK(!seen.count(s.page), "page %s is split", s.page); seen.insert(s.page); last = s.page; }
    }
    CHECK((int)perPage.size() == 2 + axe::kNumModes, "%d pages (want Global, Trigger and one per mode)", (int)perPage.size());
    for (int m = 0; m < axe::kNumModes; m++) {
        int n = 0; for (const auto& s : specs) n += s.mode == m;
        CHECK(n == 6, "mode %s has %d controls", axe::modeName(m), n);
    }
    CHECK(p.getBypassParameter() != nullptr, "no bypass parameter (required for lv2:enabled)");
    auto alt = p.getAlternateDisplayNames();
    CHECK(alt.size() > 0 && alt[0].length() >= 2 && alt[0].length() <= 3 && alt[0].toUpperCase() == alt[0], "Anagram abbreviation must be 2-3 uppercase characters");
    CHECK(p.getLatencySamples() == 0, "latency should be 0");
    CHECK(p.getTotalNumInputChannels() == 1 && p.getTotalNumOutputChannels() == 1, "default layout should be mono in / mono out (Anagram)");
    juce::AudioProcessor::BusesLayout st;
    st.inputBuses.add(juce::AudioChannelSet::stereo()); st.outputBuses.add(juce::AudioChannelSet::stereo());
    CHECK(p.checkBusesLayoutSupported(st), "stereo in/out should be supported (dual mono)");
    for (const auto& s : specs)
        if (s.kind == ParamSpec::Float)
            CHECK(std::string(s.label).size() > 0, "parameter %s has no unit (the Anagram can't ask for value text)", s.id);
}

static void testRatesAndModes() {
    for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
        AxeProcessor p;
        p.prepareToPlay(sr, 512);
        for (int m = 0; m < axe::kNumModes; m++) {
            for (int inst = 0; inst < 2; inst++) {
                setParam(p, "mode", (float)m);
                setParam(p, "instrument", (float)inst);
                setParam(p, "trigger", m == axe::kGlitch ? 0.f : 1.f);
                auto x = pluck(sr, 0.6, inst ? 55.f : 110.f);
                auto y = run(p, x, 512);
                bool ok = true; double e = 0;
                for (float v : y) { ok = ok && std::isfinite(v); e += (double)v * v; }
                CHECK(ok && e > 0.0, "mode %d inst %d at %.0f Hz: bad output", m, inst, sr);
            }
        }
    }
}

static void testBypassAndAutomation() {
    const double sr = 48000.0;
    AxeProcessor p;
    p.prepareToPlay(sr, 256);
    setParam(p, "mode", (float)axe::kBuzzsaw);
    auto x = pluck(sr, 2.0, 110.f);
    // host bypass: after the 20 ms crossfade the output is the input, bit for bit
    auto y = run(p, x, 256, [&](size_t i) { if (i == 24064) p.getBypassParameter()->setValueNotifyingHost(1.f); });
    double err = 0; for (size_t i = 28000; i < x.size(); i++) err = std::max(err, (double)std::fabs(y[i] - x[i]));
    CHECK(err == 0.0, "host bypass is not bit-exact (%g)", err);
    p.getBypassParameter()->setValueNotifyingHost(0.f);

    // automation of every float parameter, full range in one block: no clicks on a steady tone
    std::vector<float> tone((size_t)sr * 2);
    for (size_t i = 0; i < tone.size(); i++) tone[i] = 0.2f * (float)std::sin(2.0 * M_PI * 220.0 * (double)i / sr);
    // (the fx pages are rhythmic or random by design; the engine test covers their smoothed controls)
    for (const auto& s : paramSpecs()) {
        if (s.kind != ParamSpec::Float || s.mode >= axe::kGlitch) continue;
        if (!std::strcmp(s.id, "tempo") || !std::strcmp(s.id, "swing")) continue;
        AxeProcessor q;
        q.prepareToPlay(sr, 128);
        setParam(q, "mode", (float)(s.mode < 0 ? axe::kTight : s.mode));
        setParam(q, s.id, s.lo);
        auto out = run(q, tone, 128, [&](size_t i) { if (i == 48000) setParam(q, s.id, s.hi); });
        auto d2 = [&](size_t a, size_t b) { float m = 0; for (size_t i = a; i < b; i++) m = std::max(m, std::fabs(out[i] - 2 * out[i - 1] + out[i - 2])); return m; };
        float base = std::max(d2(36000, 48000), d2(72000, 84000)), jump = d2(48000, 52800);
        CHECK(jump <= 4.f * base + 1e-4f, "automating %s clicks (%.1fx)", s.id, jump / (base + 1e-9f));
    }
}

static void testState() {
    AxeProcessor a;
    a.prepareToPlay(48000.0, 256);
    setParam(a, "mode", (float)axe::kLaser); setParam(a, "instrument", 1.f); setParam(a, "lSweep", 300.f);
    setParam(a, "rate", 7.f); setParam(a, "tempo", 141.f); setParam(a, "engage", 1.f); setParam(a, "release", 120.f);
    juce::MemoryBlock mb;
    a.getStateInformation(mb);
    AxeProcessor b;
    b.setStateInformation(mb.getData(), (int)mb.getSize());
    for (const auto& s : paramSpecs()) {
        float va = a.apvts.getRawParameterValue(s.id)->load(), vb = b.apvts.getRawParameterValue(s.id)->load();
        CHECK(std::fabs(va - vb) < 1e-4f, "state round trip changed %s (%g -> %g)", s.id, va, vb);
    }
}

// The host's tempo drives the grid when Sync = Host; the Tempo knob otherwise.
struct FakePlayHead : juce::AudioPlayHead {
    double bpm = 150.0, ppq = 0.0; bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo p; p.setBpm(bpm); p.setPpqPosition(ppq); p.setIsPlaying(playing); return p;
    }
};

static void testTempoAndTrigger() {
    const double sr = 48000.0;
    AxeProcessor p;
    FakePlayHead ph;
    p.setPlayHead(&ph);
    p.prepareToPlay(sr, 256);
    setParam(p, "mode", (float)axe::kStutter);
    setParam(p, "tempo", 100.f);
    auto x = pluck(sr, 0.5, 110.f);
    run(p, x, 256, [&](size_t i) { ph.ppq = (double)i / sr * ph.bpm / 60.0; });
    CHECK(std::fabs(p.meters.bpm.load() - 150.f) < 0.01f, "host tempo not followed (%.2f BPM)", p.meters.bpm.load());
    setParam(p, "sync", 0.f);
    run(p, x, 256);
    CHECK(std::fabs(p.meters.bpm.load() - 100.f) < 0.01f, "Tempo knob not used with Sync = Tempo Knob (%.2f BPM)", p.meters.bpm.load());
    // Trigger reaches the engine
    setParam(p, "trigger", 1.f);
    run(p, std::vector<float>(512, 0.f), 256);
    CHECK(p.meters.triggered.load(), "Trigger parameter does not reach the engine");
    setParam(p, "trigger", 0.f);
    run(p, std::vector<float>(512, 0.f), 256);
    CHECK(!p.meters.triggered.load(), "Trigger does not release");
    p.setPlayHead(nullptr);
}

static void testEditor(const char* snapDir) {
    AxeProcessor p;
    p.prepareToPlay(48000.0, 256);
    std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
    CHECK(ed != nullptr && ed->getWidth() > 0, "no editor");
    for (int m = 0; m < axe::kNumModes; m++) {
        setParam(p, "mode", (float)m);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(120);   // editor timer picks up the mode
        auto img = ed->createComponentSnapshot(ed->getLocalBounds());
        CHECK(img.isValid(), "editor snapshot failed");
        if (snapDir) {
            juce::File f = juce::File(snapDir).getChildFile("editor-" + juce::String(m + 1) + ".png");
            f.getParentDirectory().createDirectory();
            juce::FileOutputStream os(f);
            if (os.openedOk()) { os.setPosition(0); os.truncate(); juce::PNGImageFormat().writeImageToStream(img, os); }
        }
    }
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf("layout\n");               testLayout();
    std::printf("rates and modes\n");      testRatesAndModes();
    std::printf("bypass and automation\n"); testBypassAndAutomation();
    std::printf("state\n");                testState();
    std::printf("tempo and trigger\n");    testTempoAndTrigger();
    std::printf("editor\n");               testEditor(argc > 1 ? argv[1] : nullptr);
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
