// Shared helpers for the desktop tools: WAV I/O, parameter parsing, denormal flushing.
#pragma once
#include "../dsp/axe.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

// The desktop tools are the host here, so they set flush-to-zero (the
// engine never does; on the Anagram the host owns the CPU flags).
inline void flushDenormals() {
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    asm volatile("msr fpcr, %0" ::"r"(fpcr | (1ull << 24)));
#endif
}

// Reads 16/24/32-bit PCM or 32-bit float WAV, mixed to mono.
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
            if (fmt == 0xFFFE && size >= 26) std::memcpy(&fmt, &b[24], 2);
            if (size & 1) std::fseek(f, 1, SEEK_CUR);
        } else if (!std::memcmp(id, "data", 4)) {
            std::vector<uint8_t> b(size);
            if (!ch || !bits || std::fread(b.data(), 1, size, f) != size) break;
            const int bps = bits / 8;
            size_t frames = size / bps / ch;
            out.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                float s = 0;
                for (int c = 0; c < ch; ++c) {
                    const uint8_t* q = &b[(i * ch + c) * bps];
                    if (fmt == 1 && bits == 16) { int16_t v; std::memcpy(&v, q, 2); s += v / 32768.f; }
                    else if (fmt == 1 && bits == 24) { int32_t v = (q[0] << 8) | (q[1] << 16) | (q[2] << 24); s += (v >> 8) / 8388608.f; }
                    else if (fmt == 1 && bits == 32) { int32_t v; std::memcpy(&v, q, 4); s += v / 2147483648.f; }
                    else if (fmt == 3 && bits == 32) { float v; std::memcpy(&v, q, 4); s += v; }
                    else { std::fclose(f); std::fprintf(stderr, "unsupported WAV format (%d-bit, fmt %d)\n", bits, fmt); return false; }
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

// Mono 32-bit float WAV (no clipping, so measurements see the real output).
inline void writeWav(const char* path, const std::vector<float>& x, int sr) {
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "could not write %s\n", path); std::exit(1); }
    uint32_t dataSize = (uint32_t)x.size() * 4, riff = 36 + dataSize, fmtSize = 16, rate = sr, byteRate = sr * 4;
    uint16_t fmt = 3, ch = 1, align = 4, bits = 32;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtSize, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&dataSize, 4, 1, f);
    std::fwrite(x.data(), 4, x.size(), f);
    std::fclose(f);
}

inline int modeByName(const std::string& s) {
    static const char* names[axe::kNumModes] = {"tight", "sub", "buzzsaw", "glitch", "laser", "dive", "chip", "stutter", "ring", "freeze", "feedback", "swarm", "radio"};
    for (int m = 0; m < axe::kNumModes; m++) if (s == names[m] || s == std::to_string(m)) return m;
    if (s == "buzz") return axe::kBuzzsaw;
    if (s == "chiptune") return axe::kChip;
    if (s == "gate") return axe::kStutter;
    return -1;
}

// Apply "name=value" overrides to the parameter set.
inline bool setParam(axe::Params& p, const char* arg) {
    const char* eq = std::strchr(arg, '=');
    if (!eq) return false;
    std::string k(arg, eq - arg), sv(eq + 1);
    float v = (float)std::atof(eq + 1);
    if (k == "mode") { int m = modeByName(sv); if (m < 0) return false; p.mode = m; return true; }
    if (k == "inst" || k == "instrument") { p.instrument = (sv == "bass" || sv == "1") ? axe::kBass : axe::kGuitar; return true; }
    if (k == "subtype") { p.subType = (sv == "mono" || sv == "1") ? 1 : 0; return true; }
    if (k == "bypass") { p.bypass = v != 0.f; return true; }
    if (k == "trigger") { p.trigger = v != 0.f; return true; }
    if (k == "engage") { p.engage = (sv == "trigger" || sv == "1") ? axe::kWhileTriggered : axe::kAlways; return true; }
    if (k == "sync") { p.sync = v != 0.f ? 1 : 0; return true; }
    if (k == "rate") { for (int r = 0; r < axe::kNumRates; r++) if (sv == axe::rateName(r)) { p.rate = r; return true; } p.rate = (int)v; return true; }
    if (k == "ltrack") { p.lTrack = v != 0.f; return true; }
    if (k == "rtrack") { p.rTrack = v != 0.f; return true; }
    if (k == "carp") { p.cArp = (int)v; return true; }
    if (k == "coct") { p.cOctave = (int)v; return true; }
    if (k == "pattern") { p.sPattern = (int)v; return true; }
    if (k == "rtype") { p.rType = (int)v; return true; }
    if (k == "harmonic") { p.fbHarmonic = (int)v; return true; }
    struct F { const char* n; float* f; } fs[] = {
        {"in", &p.inDb}, {"gate", &p.gate}, {"mix", &p.mix}, {"out", &p.outDb}, {"tempo", &p.tempo}, {"swing", &p.swing},
        {"tight", &p.tight}, {"punch", &p.punch}, {"release", &p.release}, {"low", &p.lowDb}, {"mid", &p.midDb}, {"high", &p.highDb},
        {"sub", &p.sub}, {"dry", &p.dry}, {"subtone", &p.subTone}, {"range", &p.range}, {"grit", &p.grit},
        {"gain", &p.gain}, {"lows", &p.lows}, {"highs", &p.highs}, {"scoop", &p.scoop}, {"level", &p.level}, {"blend", &p.blend},
        {"chaos", &p.gChaos}, {"crush", &p.gCrush}, {"jumps", &p.gJumps}, {"tapestop", &p.gTapeStop}, {"reverse", &p.gReverse}, {"smooth", &p.gSmooth},
        {"lpitch", &p.lPitch}, {"drop", &p.lDrop}, {"sweep", &p.lSweep}, {"tone", &p.lTone}, {"llevel", &p.lLevel},
        {"depth", &p.dDepth}, {"dtime", &p.dTime}, {"dreturn", &p.dReturn}, {"curve", &p.dCurve}, {"wobble", &p.dWobble}, {"ddry", &p.dDry},
        {"pulse", &p.cPulse}, {"ccrush", &p.cCrush}, {"glide", &p.cGlide}, {"cdry", &p.cDry},
        {"duty", &p.sDuty}, {"sdepth", &p.sDepth}, {"shape", &p.sShape}, {"random", &p.sRandom}, {"stone", &p.sTone},
        {"rfreq", &p.rFreq}, {"jump", &p.rJump}, {"rglide", &p.rGlide}, {"drive", &p.rDrive},
        {"size", &p.fzSize}, {"attack", &p.fzAttack}, {"frelease", &p.fzRelease}, {"drift", &p.fzDrift}, {"shimmer", &p.fzShimmer}, {"flevel", &p.fzLevel},
        {"swell", &p.fbSwell}, {"fbrelease", &p.fbRelease}, {"fblevel", &p.fbLevel}, {"bloom", &p.fbBloom}, {"fbtone", &p.fbTone},
        {"density", &p.swDensity}, {"grain", &p.swSize}, {"spray", &p.swSpray}, {"spitch", &p.swPitch}, {"sreverse", &p.swReverse}, {"swlevel", &p.swLevel},
        {"band", &p.rdBand}, {"width", &p.rdWidth}, {"static", &p.rdStatic}, {"warble", &p.rdWarble}, {"spin", &p.rdSpin}, {"fade", &p.rdFade},
    };
    for (auto& e : fs) if (k == e.n) { *e.f = v; return true; }
    return false;
}

inline void printParams() {
    std::fprintf(stderr,
        "global  : mode=tight|sub|buzzsaw|glitch|laser|dive|chip|stutter|ring|freeze|feedback|swarm|radio\n"
        "          inst=guitar|bass in=dB gate=0..1 mix=0..1 out=dB bypass=0|1\n"
        "trigger : trigger=0|1 engage=always|trigger sync=0|1 tempo=BPM rate=1/16 (1/1..1/32, 1/8T, 1/8D...) swing=0..1\n"
        "          trigat=SECONDS[,SECONDS...]  toggles Trigger at those times (axe_wav only)\n"
        "tight   : tight punch release (0..1)  low mid high (dB)\n"
        "sub     : subtype=poly|mono sub dry subtone range grit\n"
        "buzzsaw : gain lows highs scoop blend (0..1) level (dB)\n"
        "glitch  : chaos crush jumps tapestop reverse smooth\n"
        "laser   : lpitch ltrack=0|1 drop sweep tone llevel\n"
        "dive    : depth (semitones) dtime dreturn curve wobble ddry\n"
        "chip    : pulse ccrush carp=0..5 coct=0|1|2 glide cdry\n"
        "stutter : pattern=0..5 duty sdepth shape random stone\n"
        "ring    : rtype=0|1 rfreq rtrack=0|1 jump rglide drive\n"
        "freeze  : size attack frelease drift shimmer flevel\n"
        "feedback: harmonic=0..3 swell fbrelease fblevel bloom fbtone\n"
        "swarm   : density grain spray spitch sreverse swlevel\n"
        "radio   : band width static warble spin fade\n");
}
