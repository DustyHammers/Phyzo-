// esp2_test: chip-level tests of the ESP2 core without the OS CPU. Effect programs are located and parsed in the
// user's OS image at run time (ESP2 Object Format, public spec); nothing from the image is stored here.
//
//   esp2_test --os <image> --list
//   esp2_test --os <image> --testsine <out.wav> [--seconds 2]
//       loads program "testsine" as the OS would (microinstructions at 0, initial registers), starts the chip and
//       records the DAC lines (SER 0x3ED/0x3EC); reports frequency, level and residual after a fitted sine.
//   esp2_test --os <image> --bypass [--seconds 1]
//       runs program "bypass" between a 7-line test harness of our own (ADC -> bypass inputs, bypass outputs -> DAC,
//       BIOZ, loop) and feeds a deterministic test signal; the output must equal the input delayed by one sample.
//   esp2_test --replay <esp2_commands.csv> [--seconds 10] [--ref] [--wav out.wav]
//       replays a recorded host command stream (from phyzo_boot_trace / phyzo_play --trace) at its original times, then
//       runs the loaded image with a deterministic signal on all ten voice channels; prints the ESP2-alone
//       real-time factor and an FNV hash of the DAC output (compare --ref = reference interpreter).
//   esp2_test --os <image> --bench <seconds>
//       instruction-rate benchmark: testsine padded to a full 192-cycle period (no BIOZ waiting).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "esp2_core.h"
#include "os_image.h"

namespace {
struct Prog {
    uint32_t addr = 0; std::string name; int family = 0, member = 0;
    std::vector<std::pair<uint32_t, uint32_t>> inits;      // register, value
    std::vector<std::vector<uint8_t>> uinst;
};
struct Reader {
    const std::vector<uint8_t>& d; size_t p;
    uint8_t b() { return p < d.size() ? d[p++] : 0; }
    uint32_t num() { uint8_t c = b(); if (!(c & 0x80)) return c; uint32_t v = 0; for (int i = 0; i < (c & 0x7F); ++i) v = (v << 8) | b(); return v; }
    std::string name() { int c = b(); std::string s; for (int i = 0; i < c; ++i) s += char(b()); return s; }
};
bool parse(const std::vector<uint8_t>& d, size_t s, Prog& o) {
    if (s + 5 > d.size() || d[s] != 'E' || d[s + 1] != '2') return false;
    Reader r{d, s + 2};
    uint32_t hsize = r.num(); r.num();
    size_t hend = s + 3 + hsize; uint32_t offs[8] = {0};
    while (r.p < hend && r.p < d.size()) { uint8_t t = r.b(); uint32_t v = r.num(); if (t < 8) offs[t] = v; }
    auto part = [&](int tag, size_t& start, size_t& size) {
        if (!offs[tag]) return false;
        r.p = s + offs[tag]; if (r.b() != tag) return false; size = r.num(); start = r.p; return true;
    };
    size_t q, sz;
    if (!part(0, q, sz)) return false;
    r.p = q; r.b(); o.family = int(r.num()); o.member = int(r.num()); o.name = r.name();
    if (part(3, q, sz)) {
        r.p = q;
        while (r.p < q + sz) {
            uint8_t t = r.b();
            if (t == 0) { uint32_t n = r.num(), a = r.num(); for (uint32_t i = 0; i < n; ++i) o.inits.push_back({a + i, r.num()}); }
            else if (t == 1) { uint32_t n = r.num(); for (uint32_t i = 0; i < n; ++i) { uint32_t a = r.num(); o.inits.push_back({a, r.num()}); } }
            else if (t == 2) { uint32_t n = r.num(); for (uint32_t i = 0; i < n; ++i) { uint32_t a = r.num(), c = r.num(), v = r.num(); for (uint32_t k = 0; k < c; ++k) o.inits.push_back({a + k, v}); } }
            else if (t == 3 || t == 5) { uint32_t n = r.num(); for (uint32_t i = 0; i < 3 * n; ++i) r.num(); }
            else if (t == 4) { uint32_t n = r.num(); for (uint32_t i = 0; i < n; ++i) { r.num(); r.num(); r.name(); } }
            else break;
        }
    }
    if (!part(5, q, sz)) return false;
    r.p = q; if (r.b() != 0) return false;
    uint32_t n = r.num();
    for (uint32_t i = 0; i < n; ++i) { o.uinst.emplace_back(d.begin() + long(r.p), d.begin() + long(r.p + 12)); r.p += 12; }
    o.addr = uint32_t(s);
    return n > 0;
}
std::vector<Prog> findPrograms(const std::vector<uint8_t>& d) {
    std::vector<Prog> v;
    for (size_t i = 0; i + 5 < d.size(); ++i)
        if (d[i] == 'E' && d[i + 1] == '2' && d[i + 2] < 0x80 && d[i + 3] < 0x80 && d[i + 4] == 0) { Prog p; if (parse(d, i, p)) v.push_back(p); }
    return v;
}
// host-port helpers (exactly the byte protocol the OS uses)
void hostReg(Esp2Core& e, uint32_t a, uint32_t v) {
    e.write8(0x11, uint8_t(v >> 16)); e.write8(0x12, uint8_t(v >> 8)); e.write8(0x13, uint8_t(v));
    e.write8(0x14, uint8_t(a >> 8)); e.write8(0x15, uint8_t(a)); e.write8(0x17, 0x80);
}
void hostIns(Esp2Core& e, uint32_t a, const uint8_t* w) {
    for (int i = 0; i < 12; ++i) e.write8(uint32_t(i), w[i]);
    e.write8(0x0C, uint8_t(a >> 8)); e.write8(0x0D, uint8_t(a)); e.write8(0x0F, 0x80);
}
// instruction encoder for the test harness lines (field layout from the Object Format appendix)
std::vector<uint8_t> enc(int alu, int A, int B, int C, int mac, int D, int E, int F, int sh, int ag = 6, int G = 0x200) {
    uint64_t hi = (uint64_t(A) << 22) | (uint64_t(B) << 12) | (uint64_t(C) << 2) | uint64_t(alu >> 3);
    uint64_t lo = (uint64_t(alu & 7) << 61) | (uint64_t(D) << 50) | (uint64_t(E) << 40) | (uint64_t(F) << 30) | (uint64_t(mac) << 25) |
                  (uint64_t(sh + 7) << 21) | (uint64_t(G & 0x1FF) << 11) | (uint64_t(ag) << 8);
    std::vector<uint8_t> w(12);
    for (int i = 0; i < 4; ++i) w[i] = uint8_t(hi >> (24 - 8 * i));
    for (int i = 0; i < 8; ++i) w[4 + i] = uint8_t(lo >> (56 - 8 * i));
    return w;
}
const auto NOPW = []() { return enc(0x0B, 0x3FF, 0x3F1, 0x3F1, 2, 0x1D4, 0x1CC, 0x3FF, -1); };
void writeWav(const std::string& path, const std::vector<float>& lr) {
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    auto p32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto p16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = uint32_t(lr.size() * 4);
    std::fwrite("RIFF", 1, 4, f); p32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    p32(16); p16(3); p16(2); p32(44100); p32(44100 * 8); p16(8); p16(32); std::fwrite("data", 1, 4, f); p32(bytes);
    std::fwrite(lr.data(), 4, lr.size(), f); std::fclose(f);
}
void loadProgram(Esp2Core& e, const Prog& p) {
    for (size_t i = 0; i < p.uinst.size(); ++i) hostIns(e, uint32_t(i), p.uinst[i].data());
    for (auto& kv : p.inits) hostReg(e, kv.first, kv.second);
}
}  // namespace

int main(int argc, char** argv) {
    std::string osPath, mode, out, replay, wavOut; double seconds = 2; bool ref = false, checkpoint = false; uint64_t ipsArg = 192;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--os") osPath = next(); else if (a == "--list") mode = "list";
        else if (a == "--testsine") { mode = "testsine"; out = next(); } else if (a == "--bypass") mode = "bypass";
        else if (a == "--replay") { mode = "replay"; replay = next(); } else if (a == "--ref") ref = true;
        else if (a == "--checkpoint") checkpoint = true; else if (a == "--ips") ipsArg = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--wav") wavOut = next();
        else if (a == "--bench") { mode = "bench"; seconds = std::atof(next().c_str()); } else if (a == "--seconds") seconds = std::atof(next().c_str());
    }
    if (mode == "replay") {
        Esp2Core e; e.fastPath = !ref; e.specFixes = !checkpoint; e.instrPerSample = int(ipsArg);
        const uint64_t ips = ipsArg;
        FILE* f = std::fopen(replay.c_str(), "r"); if (!f) { std::puts("cannot open replay"); return 2; }
        char line[256]; uint64_t sample = 0, cmds = 0;
        auto tick = [&](uint64_t upto) { while (sample < upto) { e.runTo((sample + 1) * ips); e.sampleTick(); ++sample; } };
        while (std::fgets(line, sizeof line, f)) {
            double t; char src[32], op[32], addr[16], val[64];
            std::string L(line); for (auto& c : L) if (c == ',') c = ' ';
            int n = std::sscanf(L.c_str(), "%lf %31s %31s %15s %63s", &t, src, op, addr, val);
            if (n < 3) continue;
            uint64_t at = uint64_t(t * 44.1);                       // sample index of the command
            tick(at);
            e.runTo(sample * ips + uint64_t((t * 44.1 - double(at)) * double(ips)));
            std::string o = op;
            if (o == "CTRL" && n >= 4) { e.write8(0x19, uint8_t(std::strtoul(addr, nullptr, 16))); ++cmds; }
            else if (o == "REGW" && n >= 5) { hostReg(e, uint32_t(std::strtoul(addr, nullptr, 16)), uint32_t(std::strtoul(val, nullptr, 16))); ++cmds; }
            else if (o == "INSW" && n >= 5) { uint8_t w[12]; for (int i = 0; i < 12; ++i) w[i] = uint8_t(std::strtoul(std::string(val + 2 * i, 2).c_str(), nullptr, 16)); hostIns(e, uint32_t(std::strtoul(addr, nullptr, 16)), w); ++cmds; }
            else if (o == "REGR" && n >= 4) { uint32_t a = uint32_t(std::strtoul(addr, nullptr, 16)); e.write8(0x14, uint8_t(a >> 8)); e.write8(0x15, uint8_t(a)); e.write8(0x17, 0x81); ++cmds; }
        }
        std::fclose(f);
        uint64_t trace0 = sample, ex0 = e.executed;
        uint64_t hash = 1469598103934665603ull; uint32_t lcg = 1; uint32_t phase[20] = {0};
        std::vector<float> lr;
        auto t0 = std::chrono::steady_clock::now();
        for (uint64_t s = 0; s < uint64_t(seconds * 44100); ++s) {
            for (int c = 0; c < 10; ++c) for (int side = 0; side < 2; ++side) {   // sawtooth per channel + noise (cheap)
                lcg = lcg * 1664525u + 1013904223u;
                phase[2 * c + side] += uint32_t((110.0 * (c + 1) + 3 * side) * 97391.5);   // 2^32 / 44100
                int32_t saw = int32_t(phase[2 * c + side]) >> 10;                         // +/- 2^21 (0.25 FS)
                e.voicePort[2 * c + side] = (saw + (int32_t(lcg) >> 13)) & ~0xF;
            }
            e.runTo((sample + 1) * ips); e.sampleTick(); ++sample;
            for (int i = 0; i < 2; ++i) { hash ^= uint32_t(e.dacOut[i]); hash *= 1099511628211ull; }
            if (!wavOut.empty()) { lr.push_back(float(e.dacOut[0] / 8388608.0)); lr.push_back(float(e.dacOut[1] / 8388608.0)); }
        }
        double host = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!wavOut.empty()) writeWav(wavOut, lr);
        std::printf("replay: %llu commands over %.2f s; then %.1f s with the loaded image: executed %.1f instr/period, host %.3f s -> ESP2-alone real-time factor %.1fx (%.1f ns/instruction) [%s, %s]\n",
                    (unsigned long long)cmds, double(trace0) / 44100.0, seconds, double(e.executed - ex0) / (seconds * 44100), host, seconds / host,
                    host * 1e9 / double(e.executed - ex0), ref ? "reference interpreter" : "translated", checkpoint ? "checkpoint semantics" : "spec semantics");
        std::printf("replay: DAC hash %016llx  mac_sat %llu alu_sat %llu unmapped %llu overruns %llu\n", (unsigned long long)hash,
                    (unsigned long long)e.macSat, (unsigned long long)e.aluSat, (unsigned long long)e.memUnmapped, (unsigned long long)e.overruns);
        return 0;
    }
    OsImage os; std::string err;
    if (osPath.empty() || !os.load(osPath, err)) { std::fprintf(stderr, "usage: esp2_test --os <image> (--list | --testsine out.wav | --bypass | --bench s)\n%s\n", err.c_str()); return 2; }
    std::vector<Prog> progs = findPrograms(os.bytes);
    auto get = [&](const char* n) -> const Prog* { for (auto& p : progs) if (p.name == n) return &p; return nullptr; };
    std::printf("os_md5 %s, %zu ESP2 programs found\n", os.md5.c_str(), progs.size());
    if (mode == "list") { for (auto& p : progs) std::printf("%06X %-10s family %3d member %3d instructions %3zu inits %3zu\n", p.addr + 0x4000, p.name.c_str(), p.family, p.member, p.uinst.size(), p.inits.size()); return 0; }

    Esp2Core e; e.specFixes = !checkpoint;
    const uint64_t ips = 192;
    if (mode == "testsine" || mode == "bench") {
        const Prog* p = get("testsine"); if (!p) { std::puts("testsine not found"); return 1; }
        loadProgram(e, *p);
        e.write8(0x19, 0x31);                                   // run, IOZ enabled (the OS's run value)
        size_t n = size_t(seconds * 44100);
        std::vector<float> lr; std::vector<double> L;
        auto t0 = std::chrono::steady_clock::now();
        for (size_t s = 0; s < n; ++s) {
            e.runTo((s + 1) * ips); e.sampleTick();
            if (mode == "testsine") { lr.push_back(float(e.dacOut[0] / 8388608.0)); lr.push_back(float(e.dacOut[1] / 8388608.0)); L.push_back(e.dacOut[0] / 8388608.0); }
        }
        double host = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (mode == "bench") {
            std::printf("bench: %zu periods, executed %llu instructions (%.1f per period), host %.3f s -> %.1f ns/instruction; real-time factor %.1fx at 192/period if every cycle executed\n",
                        n, (unsigned long long)e.executed, double(e.executed) / n, host, host * 1e9 / double(e.executed), double(n) / 44100.0 / (host * 192.0 * n / double(e.executed)));
            return 0;
        }
        writeWav(out, lr);
        // analysis: zero-crossing frequency, fitted sine at that frequency, residual
        std::vector<double> zc; size_t skip = 4410;
        for (size_t i = skip + 1; i < L.size(); ++i) if (L[i - 1] < 0 && L[i] >= 0) zc.push_back(double(i - 1) + L[i - 1] / (L[i - 1] - L[i]));
        double f = zc.size() > 2 ? 44100.0 * double(zc.size() - 1) / (zc.back() - zc.front()) : 0;
        double w = 2 * M_PI * f / 44100.0, sc = 0, ss = 0, peak = 0;
        for (size_t i = skip; i < L.size(); ++i) { sc += L[i] * std::cos(w * double(i)); ss += L[i] * std::sin(w * double(i)); peak = std::max(peak, std::fabs(L[i])); }
        double m = double(L.size() - skip), a = 2 * std::hypot(sc, ss) / m, ph = std::atan2(-ss, sc), res = 0, sig = 0;
        for (size_t i = skip; i < L.size(); ++i) { double y = a * std::cos(w * double(i) + ph); res += (L[i] - y) * (L[i] - y); sig += y * y; }
        double rdiff = 0; for (size_t i = 0; i < lr.size(); i += 2) rdiff = std::max(rdiff, double(std::fabs(lr[i] - lr[i + 1])));
        std::printf("testsine: %zu samples, frequency %.4f Hz, amplitude %.6f (%.2f dBFS), peak %.6f, residual after fitted sine %.1f dB, max |L-R| %.3g\n",
                    L.size(), f, a, 20 * std::log10(a), peak, 10 * std::log10(res / sig), rdiff);
        std::printf("aux lines SER 0x3EB..0x3E4 at end:"); for (int i = 0; i < 8; ++i) std::printf(" %d", e.auxOut[i]); std::printf("\n");
        std::printf("core: executed %llu bioz_suspends %llu overruns %llu mac_sat %llu alu_sat %llu\n", (unsigned long long)e.executed,
                    (unsigned long long)e.biozSuspends, (unsigned long long)e.overruns, (unsigned long long)e.macSat, (unsigned long long)e.aluSat);
        return 0;
    }
    if (mode == "bypass") {
        const Prog* p = get("bypass"); if (!p || p->uinst.size() != 1) { std::puts("bypass not found"); return 1; }
        std::vector<std::vector<uint8_t>> img = {
            enc(0x0B, 0x3FF, 0x3EF, 0x0FF, 3, 0x3EE, 0x1CB, 0x0FE, 0),   // ALU MOV ADC_L > 0FF | MAC MOV ADC_R > 0FE
            NOPW(),
            p->uinst[0],                                                  // the OS's bypass program (0FF,0FE -> 0FD,0FC)
            NOPW(),
            enc(0x0B, 0x3FF, 0x0FD, 0x3ED, 3, 0x0FC, 0x1CB, 0x3EC, 0),   // ALU MOV 0FD > DAC_L | MAC MOV 0FC > DAC_R
            enc(0x19, 0x3FF, 0x3F1, 0x3F1, 2, 0x1D4, 0x1CC, 0x3FF, -1),  // BIOZ
            enc(0x1C, 0x000, 0x000, 0x3FF, 2, 0x1D4, 0x1CC, 0x3FF, -1),  // Jcc 0 (always)
            NOPW()};                                                      // delay slot
        for (size_t i = 0; i < img.size(); ++i) hostIns(e, uint32_t(i), img[i].data());
        for (auto& kv : p->inits) hostReg(e, kv.first, kv.second);
        e.write8(0x19, 0x31);
        size_t n = size_t(seconds * 44100), mism = 0; uint32_t lcg = 12345;
        std::vector<int32_t> inL, inR;
        for (size_t s = 0; s < n; ++s) {
            lcg = lcg * 1103515245u + 12345u;
            int32_t xl = int32_t(std::lround(0.7 * 8388607.0 * std::sin(2 * M_PI * 997.0 * double(s) / 44100.0))), xr = int32_t((lcg >> 8) & 0xFFFFFF) - 0x800000;
            inL.push_back(xl); inR.push_back(xr);
            e.serialIn[0] = xl; e.serialIn[1] = xr;
            e.runTo((s + 1) * ips); e.sampleTick();
            // the tick latches the DAC from the previous period and loads the ADC for the next one: output(s) = input(s-1)
            if (s >= 1 && (e.dacOut[0] != inL[s - 1] || e.dacOut[1] != inR[s - 1])) ++mism;
        }
        std::printf("bypass: %zu samples (997 Hz sine left, full-scale noise right), mismatches vs input delayed 1 period: %zu; executed %llu, suspends %llu\n",
                    n, mism, (unsigned long long)e.executed, (unsigned long long)e.biozSuspends);
        return mism ? 1 : 0;
    }
    std::puts("no mode"); return 2;
}
