// filter_test: chip-level tests of the voice core's filter modes, without the OS CPU.
// Programs one voice (or 48 for --bench) directly: a looped zone from the user's native wave image, filter mode
// in CR bits 8-10, resonance codes in register 0x48 built from the user's OS image tables, K1 = K2 swept through
// the user's OS low-pass cutoff table. Nothing manufacturer-derived is compiled in; zone addresses are arguments.
//
//   filter_test --os <image> --wave <native image> --zone-start N --zone-len N --zone-pitch P [--key 36]
//               (--sweep <outdir> | --stress | --bench <cr filter bits hex> <res index>)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "os_image.h"
#include "os_profile.h"
#include "voice_core.h"

#ifdef FILTER_TEST_BASELINE   // build against an older core for before/after comparisons
#define CLAMPS(c) 0ull
#else
#define CLAMPS(c) (unsigned long long)(c).stateClamps
#endif
namespace {
OsImage g_os;
int g_limit = 0;   // 0 = core default
// End of note for ring-out: park the accumulator outside wave memory (reads 0) with FC = 0.
void silenceInput(VoiceCore &c, int voice) {
    c.write(0x7C, uint32_t(voice), 4); c.write(0x04, 0, 4);
    c.write(0x2C, 0x2FFFFFF0, 4); c.write(0x30, 0x2FFFFFF8, 4); c.write(0x38, 0x2FFFFFF0, 4);
}
uint16_t w16(uint32_t a) { return uint16_t(g_os.at(a) << 8 | g_os.at(a + 1)); }
uint32_t resCode(int i) { return (uint32_t(w16(profile::kResCodeA + 2 * i)) << 9) + w16(profile::kResCodeB + 2 * i); }
uint32_t cutoffK(int i) { return uint32_t(w16(profile::kCutoffLP + 2 * i)) << 4; }   // as the OS (0x1B7FE)
void writeWav(const std::string &path, const std::vector<float> &lr) {
    FILE *f = std::fopen(path.c_str(), "wb"); if (!f) return;
    auto p32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto p16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = uint32_t(lr.size() * 4);
    std::fwrite("RIFF", 1, 4, f); p32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    p32(16); p16(3); p16(2); p32(44100); p32(44100 * 8); p16(8); p16(32); std::fwrite("data", 1, 4, f); p32(bytes);
    std::fwrite(lr.data(), 4, lr.size(), f); std::fclose(f);
}
struct Zone { uint32_t start = 0, len = 0; int pitch = 0; };
void reg(VoiceCore &c, int voice, uint32_t r, uint32_t v) { c.write(0x7C, uint32_t(voice), 4); c.write(r, v, 4); }
void startVoice(VoiceCore &c, int voice, const Zone &z, int key, uint32_t fbits, uint32_t code, uint32_t k) {
    const uint32_t base = 0x20000000;
    double p = z.pitch + 256.0 * key - 1741.0;
    uint32_t fc = uint32_t(std::lround(2048.0 * std::pow(2.0, p / 3072.0)));
    reg(c, voice, 0x00, 0x0003);
    for (uint32_t r = 0x40; r <= 0x54; r += 4) reg(c, voice, r, 0);
    reg(c, voice, 0x48, code);
    reg(c, voice, 0x2C, base + z.start); reg(c, voice, 0x30, base + z.start + z.len); reg(c, voice, 0x34, 0);
    reg(c, voice, 0x38, base + z.start); reg(c, voice, 0x3C, 0);
    reg(c, voice, 0x04, fc); reg(c, voice, 0x08, 0xF000); reg(c, voice, 0x10, 0xF000);
    reg(c, voice, 0x24, k); reg(c, voice, 0x1C, k);
    reg(c, voice, 0x00, 0x80000 | 0x0008 | fbits);   // start strobe, forward loop, filter bits
}
const char *modeName(uint32_t f) {
    switch (f) { case 0x000: return "m0_2LP2HP"; case 0x100: return "m1_3LP1HP"; case 0x200: return "m2_2LP2LP";
    case 0x300: return "m3_3LP1LP"; case 0x600: return "m4_ResLP"; case 0x700: return "m5_ResBP"; case 0x500: return "m6_Bypass"; }
    return "m?";
}
}  // namespace

int main(int argc, char **argv) {
    std::string osPath, wavePath, sweepDir, mode; Zone z; int key = 36; uint32_t benchBits = 0x600; int benchRes = 49;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) std::exit(2); return argv[++i]; };
        if (a == "--os") osPath = next(); else if (a == "--wave") wavePath = next();
        else if (a == "--zone-start") z.start = std::strtoul(next().c_str(), nullptr, 0);
        else if (a == "--zone-len") z.len = std::strtoul(next().c_str(), nullptr, 0);
        else if (a == "--zone-pitch") z.pitch = std::atoi(next().c_str());
        else if (a == "--key") key = std::atoi(next().c_str());
        else if (a == "--sweep") { mode = "sweep"; sweepDir = next(); }
        else if (a == "--stress") mode = "stress";
        else if (a == "--state-limit") g_limit = std::atoi(next().c_str());
        else if (a == "--bench") { mode = "bench"; benchBits = std::strtoul(next().c_str(), nullptr, 16); benchRes = std::atoi(next().c_str()); }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    std::string err;
    if (osPath.empty() || wavePath.empty() || !z.len || !g_os.load(osPath, err)) { std::fprintf(stderr, "usage / %s\n", err.c_str()); return 2; }
    std::vector<int16_t> mem; { std::ifstream f(wavePath, std::ios::binary); f.seekg(0, std::ios::end); size_t n = size_t(f.tellg()) / 2;
        mem.resize(n); f.seekg(0); f.read(reinterpret_cast<char *>(mem.data()), std::streamsize(n * 2)); }
    auto fresh = [&](VoiceCore &c, int nv) {
#ifndef FILTER_TEST_BASELINE
        if (g_limit) c.stateLimit = int32_t(1) << g_limit;
#endif
        c.setBank(2, mem); c.write(0x6C, uint32_t(nv - 1), 4); };

    if (mode == "sweep") {
        // 0.25 s at cutoff 0, sweep table index 0 -> 1023 over 4 s, 1023 -> 0 over 4 s, 0.5 s hold, note off (STOP) + 0.5 s
        mkdir(sweepDir.c_str(), 0755);
        struct Case { uint32_t bits; int res; };
        std::vector<Case> cases = {{0x000, 0}, {0x100, 0}, {0x200, 0}, {0x300, 0}, {0x500, 0}};
        for (uint32_t b : {0x600u, 0x700u}) for (int r : {0, 16, 32, 49, 50}) cases.push_back({b, r});
        std::printf("case,peak_dbfs,rms_dbfs,state_clamps,final_abs\n");
        for (auto &cs : cases) {
            VoiceCore c; fresh(c, 1);
            startVoice(c, 0, z, key, cs.bits, resCode(cs.res), cutoffK(0));
            const int sr = 44100, nPre = sr / 4, nUp = 4 * sr, nHold = sr / 2, nTail = sr / 2;
            std::vector<float> lr; double pk = 0, e = 0; long n = 0;
            for (int i = 0; i < nPre + 2 * nUp + nHold + nTail; ++i) {
                int idx = 0;
                if (i >= nPre && i < nPre + nUp) idx = int(1023.0 * (i - nPre) / nUp);
                else if (i >= nPre + nUp && i < nPre + 2 * nUp) idx = 1023 - int(1023.0 * (i - nPre - nUp) / nUp);
                reg(c, 0, 0x24, cutoffK(idx)); reg(c, 0, 0x1C, cutoffK(idx));
                if (i == nPre + 2 * nUp + nHold) silenceInput(c, 0);   // input to zero: the filter rings out
                int32_t l, r; c.tick(l, r);
                float fl = float(l) / float(1 << 19); lr.push_back(fl); lr.push_back(float(r) / float(1 << 19));
                pk = std::max(pk, double(std::fabs(fl))); e += double(fl) * fl; ++n;
            }
            double fin = std::fabs(lr[lr.size() - 2]);
            char name[96]; std::snprintf(name, sizeof name, "%s_res%02d", modeName(cs.bits), cs.res);
            if (cs.bits < 0x600) std::snprintf(name, sizeof name, "%s", modeName(cs.bits));
            writeWav(sweepDir + "/sweep_" + name + ".wav", lr);
            std::printf("%s,%.2f,%.2f,%llu,%.3g\n", name, 20 * std::log10(pk + 1e-12), 10 * std::log10(e / n + 1e-24),
                        CLAMPS(c), fin);
        }
        return 0;
    }
    if (mode == "stress") {
        // Extreme cutoffs, per-sample K jumps, maximum ramps, highest and lowest pitch, all resonance codes 0..50.
        std::printf("test,bits,res,peak_dbfs,clamps,silent_after_stop\n");
        int fails = 0;
        for (uint32_t bits : {0x600u, 0x700u}) for (int res = 0; res <= 50; ++res) for (int test = 0; test < 5; ++test) {
            VoiceCore c; fresh(c, 1);
            int k = test == 2 ? 127 : test == 3 ? 0 : key;
            uint32_t K0 = test == 0 ? 0xFFFF : test == 1 ? cutoffK(0) : 0xFFFF;
            startVoice(c, 0, z, k, bits, resCode(res), K0);
            double pk = 0, tail = 0;
            for (int i = 0; i < 44100; ++i) {
                if (test == 2 || test == 3) { uint32_t K = (i & 1) ? 0xFFFF : 0; reg(c, 0, 0x24, K); reg(c, 0, 0x1C, K); }
                if (test == 4) {   // maximum-rate ramps up and down (signed 8.8 step 0x7FFE each sample)
                    if (i % 8820 == 0) { uint16_t rmp = (i / 8820) % 2 ? 0x8002 : 0x7FFE; reg(c, 0, 0x28, rmp); reg(c, 0, 0x20, rmp); reg(c, 0, 0x18, 0x1FF); }
                    if (i % 511 == 0) reg(c, 0, 0x18, 0x1FF);
                }
                if (i == 30000) silenceInput(c, 0);
                int32_t l, r; c.tick(l, r);
                double v = std::fabs(double(l) / double(1 << 19)); pk = std::max(pk, v); if (i > 43000) tail = std::max(tail, v);
            }
            bool silent = tail < 1e-3 || res == 50;   // index 50 has zero damping: it may keep ringing (by design)
            if (!silent) ++fails;
            if (res % 7 == 0 || res >= 48 || !silent)
                std::printf("%d,%03X,%d,%.2f,%llu,%s\n", test, bits, res, 20 * std::log10(pk + 1e-12), CLAMPS(c),
                            silent ? "yes" : (tail < 1 ? "ringing" : "NO"));
        }
        std::printf("non-silent tails (excluding index 50): %d\n", fails);
        return 0;
    }
    if (mode == "bench") {
        VoiceCore c; fresh(c, 48);
        for (int v = 0; v < 48; ++v) startVoice(c, v, z, key + (v % 24), benchBits, resCode(benchRes), cutoffK(600));
        const int n = 44100 * 20;
        auto t0 = std::chrono::steady_clock::now(); int64_t acc = 0;
        for (int i = 0; i < n; ++i) { int32_t l, r; c.tick(l, r); acc += l; }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("bench bits %03X res %d: 48 voices, 20 s audio in %.3f s host, real-time factor %.1fx (%lld)\n",
                    benchBits, benchRes, s, 20.0 / s, (long long)(acc & 1));
        return 0;
    }
    std::fputs("choose --sweep, --stress or --bench\n", stderr); return 2;
}
