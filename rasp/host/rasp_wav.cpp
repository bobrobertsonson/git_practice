// Desktop test harness: run a WAV file through the RASP DSP core.
//   rasp_wav in.wav out.wav [mode 0|1|2] [drive] [fry] [throat] [vowel] [air]
// Reads 16-bit PCM or 32-bit float WAV (stereo is downmixed), writes 16-bit mono.
#include "../dsp/rasp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static bool readWav(const char* path, std::vector<float>& out, int& sr) {
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

static void writeWav(const char* path, const std::vector<float>& in, int sr) {
    FILE* f = std::fopen(path, "wb");
    uint32_t dataSize = (uint32_t)in.size() * 2, riff = 36 + dataSize, fmtSize = 16, rate = sr, byteRate = sr * 2;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtSize, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&dataSize, 4, 1, f);
    for (float s : in) { int16_t v = (int16_t)(rasp::clampf(s, -1.f, 1.f) * 32767.f); std::fwrite(&v, 2, 1, f); }
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s in.wav out.wav [mode] [drive] [fry] [throat] [vowel] [air]\n", argv[0]);
        return 1;
    }
    std::vector<float> buf; int sr = 0;
    if (!readWav(argv[1], buf, sr)) { std::fprintf(stderr, "could not read %s\n", argv[1]); return 1; }

    rasp::Params p;
    if (argc > 3) p.mode   = std::atoi(argv[3]);
    if (argc > 4) p.drive  = std::atof(argv[4]);
    if (argc > 5) p.fry    = std::atof(argv[5]);
    if (argc > 6) p.throat = std::atof(argv[6]);
    if (argc > 7) p.vowel  = std::atof(argv[7]);
    if (argc > 8) p.air    = std::atof(argv[8]);

    rasp::Processor proc;
    proc.init((float)sr);
    proc.setParams(p);
    for (float& s : buf) s = proc.process(s);
    writeWav(argv[2], buf, sr);
    std::printf("processed %zu samples @ %d Hz -> %s\n", buf.size(), sr, argv[2]);
    return 0;
}
