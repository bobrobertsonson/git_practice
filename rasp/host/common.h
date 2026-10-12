// Shared helpers for the desktop tools: WAV I/O, parameter parsing, and
// denormal flushing.
#pragma once
#include "../dsp/rasp.h"
#include "../dsp/presets.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

// Flush denormals to zero. Reverb and filter tails decay into denormal
// floats during the silences between phrases, which made real takes render
// ~10x slower than they should.
inline void flushDenormals() {
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

inline bool readWav(const char* path, std::vector<float>& out, int& sr) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char id[4]; uint32_t size;
    if (std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "RIFF", 4) ||
        std::fread(&size, 4, 1, f) != 1 ||
        std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "WAVE", 4)) { std::fclose(f); return false; }
    uint16_t fmt = 0, ch = 0, bits = 0; uint32_t rate = 0;
    while (std::fread(id, 1, 4, f) == 4 && std::fread(&size, 4, 1, f) == 1) {
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> b(size);
            if (size < 16 || std::fread(b.data(), 1, size, f) != size) break;
            std::memcpy(&fmt, &b[0], 2); std::memcpy(&ch, &b[2], 2);
            std::memcpy(&rate, &b[4], 4); std::memcpy(&bits, &b[14], 2);
            if (fmt == 0xFFFE && size >= 26) std::memcpy(&fmt, &b[24], 2); // extensible
        } else if (!std::memcmp(id, "data", 4)) {
            std::vector<uint8_t> b(size);
            if (!ch || !bits || std::fread(b.data(), 1, size, f) != size) break;
            size_t frames = size / (bits / 8) / ch;
            out.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                float s = 0;
                for (int c = 0; c < ch; ++c) {
                    size_t k = i * ch + c;
                    if (fmt == 1 && bits == 16) { int16_t v; std::memcpy(&v, &b[k * 2], 2); s += v / 32768.f; }
                    else if (fmt == 3 && bits == 32) { float v; std::memcpy(&v, &b[k * 4], 4); s += v; }
                    else { std::fclose(f); std::fprintf(stderr, "unsupported WAV format\n"); return false; }
                }
                out[i] = s / ch;
            }
            sr = (int)rate;
            std::fclose(f);
            return true;
        } else {
            std::fseek(f, size + (size & 1), SEEK_CUR);
        }
    }
    std::fclose(f);
    return false;
}

inline void writeWav(const char* path, const std::vector<float>& L, const std::vector<float>& R, int sr) {
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "could not write %s\n", path); std::exit(1); }
    uint32_t dataSize = (uint32_t)L.size() * 4, riff = 36 + dataSize, fmtSize = 16, rate = sr, byteRate = sr * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtSize, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&dataSize, 4, 1, f);
    for (size_t i = 0; i < L.size(); ++i) {
        int16_t v[2] = {(int16_t)(rasp::clampf(L[i], -1.f, 1.f) * 32767.f),
                        (int16_t)(rasp::clampf(R[i], -1.f, 1.f) * 32767.f)};
        std::fwrite(v, 2, 2, f);
    }
    std::fclose(f);
}

// Apply "name=value" overrides to the parameter set.
inline bool setParam(rasp::Params& p, const char* arg) {
    const char* eq = std::strchr(arg, '=');
    if (!eq) return false;
    std::string k(arg, eq - arg);
    if (k == "preset") {                              // replaces everything: put it first
        rasp::Params q;
        if (!rasp::presetByName(eq + 1, q)) return false;
        p = q;
        return true;
    }
    float v = (float)std::atof(eq + 1);
    struct F { const char* n; float* f; } fs[] = {
        {"locut", &p.loCut}, {"hicut", &p.hiCut}, {"drive", &p.drive}, {"mix", &p.mix},
        {"chop", &p.chop}, {"release", &p.release}, {"controlgain", &p.controlGainDb},
        {"spread", &p.spread}, {"diffuse", &p.diffuse}, {"darken", &p.darken}, {"width", &p.width},
        {"thickengain", &p.thickenGainDb}, {"tail", &p.tail}, {"tailblend", &p.tailBlend},
        {"repeats", &p.repeats}, {"delayblend", &p.delayBlend}, {"bpm", &p.bpm}, {"tilt", &p.tilt},
        {"duck", &p.duck}, {"flavourgain", &p.flavourGainDb}, {"out", &p.outDb}, {"in", &p.inDb}, {"air", &p.airDb}, {"body", &p.bodyDb}, {"octdown", &p.octDown}, {"octup", &p.octUp},
        {"deess", &p.deEss}, {"bleed", &p.bleed}, {"glue", &p.glue},
    };
    for (auto& e : fs) if (k == e.n) { *e.f = v; return true; }
    if (k == "bright")  { p.brightness = (int)v; return true; }
    if (k == "learn")   return true;                  // handled by rasp_wav
    if (k == "match") {                               // match=g0,g1,...,g9 (dB)
        const char* c = eq + 1;
        for (int b = 0; b < rasp::kMatchBands && *c; b++) { p.match[b] = (float)std::atof(c); while (*c && *c != ',') c++; if (*c) c++; }
        if (p.matchAmount <= 0.f) p.matchAmount = 1.f;
        return true;
    }
    if (k == "matchamt") { p.matchAmount = v; return true; }
    if (k == "pitch1")  { p.pitch1Semi = (int)std::lround(v); return true; }
    if (k == "pitch2")  { p.pitch2Semi = (int)std::lround(v); return true; }
    if (k == "time")    { p.delayTime = (int)v; return true; }
    if (k == "solo")    { p.solo = v != 0.f; return true; }
    if (k == "control") { p.controlOn = v != 0.f; return true; }
    if (k == "thicken") { p.thickenOn = v != 0.f; return true; }
    if (k == "flavour") { p.flavourOn = v != 0.f; return true; }
    if (k == "bypass")  { p.bypass = v != 0.f; return true; }
    if (k == "bypassmatch") { p.bypassMatch = v != 0.f; return true; }
    if (k == "mono")    { p.mono = v != 0.f; return true; }
    if (k == "howl")    { p.howl = v != 0.f; return true; }
    if (k == "mega")    { p.mega = v != 0.f; return true; }
    if (k == "throw")   { p.throwOn = v != 0.f; return true; }
    return false;
}

