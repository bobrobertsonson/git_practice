// A minimal headless LV2 host (lilv), giving a plugin what mod-host gives it
// on the Anagram / MOD Dwarf: urid:map, options (sample rate, min/max/
// nominal block length), bounded block length. Used two ways:
//
//   lv2_host test <plugin-uri>                       LV2 checks (exit code = failures)
//   lv2_host run <plugin-uri> in.wav out.wav [symbol=value ...]
//
// The checks: the plugin instantiates at several rates; a 0-sample run is
// harmless; every mode gives finite output; lv2:enabled = 0 passes the input
// through bit-exact after its crossfade; kx:Reset makes two instances fed
// different histories bit-identical (Anagram dual mono).
#include "../host/common.h"
#include <lilv/lilv.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <lv2/parameters/parameters.h>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace {

struct UridMap {
    std::map<std::string, LV2_URID> ids;
    std::vector<std::string> names;
    static LV2_URID map(LV2_URID_Map_Handle h, const char* uri) {
        auto* self = static_cast<UridMap*>(h);
        auto it = self->ids.find(uri);
        if (it != self->ids.end()) return it->second;
        self->names.push_back(uri);
        return self->ids[uri] = (LV2_URID)self->names.size();
    }
};

struct Host {
    LilvWorld* world = nullptr;
    const LilvPlugin* plugin = nullptr;
    UridMap umap;
    LV2_URID_Map mapFeature{&umap, &UridMap::map};
    Host(const char* uri) {
        world = lilv_world_new();
        lilv_world_load_all(world);
        LilvNode* u = lilv_new_uri(world, uri);
        plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), u);
        lilv_node_free(u);
    }
    ~Host() { lilv_world_free(world); }
};

// One running instance with its ports connected.
struct Instance {
    Host& host;
    LilvInstance* inst = nullptr;
    float sr;
    int maxBlock;
    int32_t minBlockV = 1, maxBlockV, nominalV;
    float srV;
    std::vector<LV2_Options_Option> opts;
    LV2_Feature mapF, optF, boundedF;
    std::vector<const LV2_Feature*> features;
    std::vector<float> controls;
    int audioIn = -1, audioOut = -1;
    std::map<std::string, int> bySymbol;
    std::vector<float> inBuf, outBuf;

    Instance(Host& h, float sampleRate, int block) : host(h), sr(sampleRate), maxBlock(block) {
        maxBlockV = block; nominalV = block; srV = sampleRate;
        auto m = [&](const char* uri) { return UridMap::map(&host.umap, uri); };
        const LV2_URID tInt = m(LV2_ATOM__Int), tFloat = m(LV2_ATOM__Float);
        opts = {
            {LV2_OPTIONS_INSTANCE, 0, m(LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), tInt, &minBlockV},
            {LV2_OPTIONS_INSTANCE, 0, m(LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), tInt, &maxBlockV},
            {LV2_OPTIONS_INSTANCE, 0, m(LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t), tInt, &nominalV},
            {LV2_OPTIONS_INSTANCE, 0, m(LV2_PARAMETERS__sampleRate), sizeof(float), tFloat, &srV},
            {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
        };
        mapF = {LV2_URID__map, &host.mapFeature};
        optF = {LV2_OPTIONS__options, opts.data()};
        boundedF = {LV2_BUF_SIZE__boundedBlockLength, nullptr};
        features = {&mapF, &optF, &boundedF, nullptr};
        inst = lilv_plugin_instantiate(host.plugin, sr, features.data());
        if (!inst) return;
        const uint32_t n = lilv_plugin_get_num_ports(host.plugin);
        controls.assign(n, 0.f);
        inBuf.assign((size_t)block, 0.f); outBuf.assign((size_t)block, 0.f);
        LilvNode* audio = lilv_new_uri(host.world, LV2_CORE__AudioPort);
        LilvNode* control = lilv_new_uri(host.world, LV2_CORE__ControlPort);
        LilvNode* input = lilv_new_uri(host.world, LV2_CORE__InputPort);
        std::vector<float> mins(n), maxs(n), defs(n);
        lilv_plugin_get_port_ranges_float(host.plugin, mins.data(), maxs.data(), defs.data());
        for (uint32_t i = 0; i < n; i++) {
            const LilvPort* port = lilv_plugin_get_port_by_index(host.plugin, i);
            bySymbol[lilv_node_as_string(lilv_port_get_symbol(host.plugin, port))] = (int)i;
            if (lilv_port_is_a(host.plugin, port, audio)) {
                if (lilv_port_is_a(host.plugin, port, input)) { audioIn = (int)i; lilv_instance_connect_port(inst, i, inBuf.data()); }
                else { audioOut = (int)i; lilv_instance_connect_port(inst, i, outBuf.data()); }
            } else if (lilv_port_is_a(host.plugin, port, control)) {
                controls[i] = std::isnan(defs[i]) ? 0.f : defs[i];
                lilv_instance_connect_port(inst, i, &controls[i]);
            } else {
                lilv_instance_connect_port(inst, i, nullptr);
            }
        }
        lilv_node_free(audio); lilv_node_free(control); lilv_node_free(input);
        lilv_instance_activate(inst);
    }
    ~Instance() { if (inst) { lilv_instance_deactivate(inst); lilv_instance_free(inst); } }
    bool ok() const { return inst && audioIn >= 0 && audioOut >= 0; }
    bool set(const std::string& sym, float v) {
        auto it = bySymbol.find(sym);
        if (it == bySymbol.end()) return false;
        controls[(size_t)it->second] = v;
        return true;
    }
    std::vector<float> run(const std::vector<float>& x, int block, const std::function<void(size_t)>& at = nullptr) {
        std::vector<float> y(x.size());
        for (size_t i = 0; i < x.size(); i += (size_t)block) {
            if (at) at(i);
            const int n = (int)std::min<size_t>((size_t)block, x.size() - i);
            std::copy(x.begin() + (long)i, x.begin() + (long)i + n, inBuf.begin());
            lilv_instance_run(inst, (uint32_t)n);
            std::copy(outBuf.begin(), outBuf.begin() + n, y.begin() + (long)i);
            set("lv2_reset", 0.f);   // a trigger lasts one run
        }
        return y;
    }
};

std::vector<float> pluck(float sr, float seconds, float f0) {
    std::vector<float> x((size_t)(sr * seconds), 0.f);
    for (int k = 0; k * 0.5f < seconds; k++)
        for (int h = 1; h <= 12; h++)
            for (size_t i = (size_t)(k * 0.5f * sr); i < x.size() && i < (size_t)((k * 0.5f + 1.2f) * sr); i++) {
                float t = (float)i / sr - k * 0.5f;
                x[i] += 0.25f / (float)(h * h) * std::exp(-(2.f + h) * t) * std::sin(2.f * axe::kPi * f0 * h * t);
            }
    return x;
}

int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int test(Host& host) {
    for (float sr : {44100.f, 48000.f, 96000.f}) {
        Instance in(host, sr, 256);
        CHECK(in.ok(), "instantiate at %.0f Hz", sr);
        if (!in.ok()) continue;
        lilv_instance_run(in.inst, 0);                      // pre-run with 0 samples (Anagram does this)
        for (int mode = 0; mode < 4; mode++) {
            in.set("mode", (float)mode);
            auto y = in.run(pluck(sr, 0.5f, 110.f), 128);
            bool finite = true; double e = 0;
            for (float v : y) { finite = finite && std::isfinite(v); e += (double)v * v; }
            CHECK(finite && e > 0.0, "mode %d at %.0f Hz: no / bad output", mode, sr);
        }
    }
    // lv2:enabled = 0: bit-exact pass-through after the 20 ms crossfade
    {
        Instance in(host, 48000.f, 128);
        in.set("mode", 3.f); in.set("gain", 90.f);
        auto x = pluck(48000.f, 1.f, 98.f);
        auto y = in.run(x, 128, [&](size_t i) { if (i == 12032) in.set("lv2_enabled", 0.f); });
        double err = 0; for (size_t i = 14400; i < x.size(); i++) err = std::max(err, (double)std::fabs(y[i] - x[i]));
        CHECK(err == 0.0, "lv2:enabled=0 is not a bit-exact bypass (err %g)", err);
    }
    // kx:Reset: two instances with different histories become identical
    {
        Instance a(host, 48000.f, 128), b(host, 48000.f, 128);
        for (auto* in : {&a, &b}) { in->set("mode", 1.f); in->set("gate", 30.f); }
        a.run(pluck(48000.f, 0.7f, 123.f), 128);
        b.run(pluck(48000.f, 0.4f, 87.f), 64);
        a.set("lv2_reset", 1.f); b.set("lv2_reset", 1.f);
        auto x = pluck(48000.f, 1.f, 82.4f);
        auto ya = a.run(x, 128), yb = b.run(x, 128);
        bool same = true; for (size_t i = 0; i < x.size(); i++) if (ya[i] != yb[i]) { same = false; break; }
        CHECK(same, "after kx:Reset two instances differ (dual-mono sync)");
    }
    std::printf("lv2 host checks: %d failed\n", failures);
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: lv2_host test <uri>\n       lv2_host run <uri> in.wav out.wav [symbol=value ...]\n");
        return 2;
    }
    Host host(argv[2]);
    if (!host.plugin) { std::fprintf(stderr, "plugin %s not found (LV2_PATH?)\n", argv[2]); return 2; }
    if (!std::strcmp(argv[1], "test")) return test(host);
    if (argc < 5) return 2;
    std::vector<float> in; int sr = 0;
    if (!readWav(argv[3], in, sr)) { std::fprintf(stderr, "could not read %s\n", argv[3]); return 2; }
    Instance inst(host, (float)sr, 512);
    if (!inst.ok()) { std::fprintf(stderr, "instantiate failed\n"); return 2; }
    for (int i = 5; i < argc; i++) {
        const char* eq = std::strchr(argv[i], '=');
        if (!eq || !inst.set(std::string(argv[i], eq - argv[i]), (float)std::atof(eq + 1))) { std::fprintf(stderr, "unknown port: %s\n", argv[i]); return 2; }
    }
    writeWav(argv[4], inst.run(in, 512), sr);
    return 0;
}
