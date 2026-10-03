// speed_bench: per-component cost of one second of 44.1 kHz audio, without ROMs (synthetic loads).
//   ESP2 core: a random 190-line program (every instruction slot used, as a busy effect program).
//   Voice chip: 32 voices, all filter modes, ramps.
//   68k (Musashi) + machine: a busy loop for one emulated second, ESP2 placeholder, no voices.
// Prints host milliseconds per emulated second (1000 ms = one full core in real time).
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include "esp2_core.h"
#include "machine.h"
#include "os_image.h"
#include "voice_core.h"

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
struct Line {
    int alu = 0x0B, A = 0x3FF, B = 0x3F1, C = 0x3F1;                     // ALU MOV REF > REF
    int mac = 2, D = 0x1D4, E = 0x1CC, F = 0x3FF, sh = 6;               // MAC NOP (MACRL x ONE >>1 > ZERO)
    int ag = 6, G = 0x200, rgn = 0, dl = 0;                             // AGEN NOP
    bool aluSkip = false, macSkip = false, agSkip = false;
};

std::vector<uint8_t> encode(const Line& l) {
    uint64_t hi = (uint64_t(l.A) << 22) | (uint64_t(l.B) << 12) | (uint64_t(l.C) << 2) | uint64_t(l.alu >> 3);
    uint64_t lo = (uint64_t(l.alu & 7) << 61) | (uint64_t(l.aluSkip) << 60) | (uint64_t(l.D) << 50) | (uint64_t(l.E) << 40) |
                  (uint64_t(l.F) << 30) | (uint64_t(l.mac) << 25) | (uint64_t(l.sh) << 21) | (uint64_t(l.macSkip) << 20) |
                  (uint64_t(l.G & 0x1FF) << 11) | (uint64_t(l.ag) << 8) | (uint64_t(l.rgn) << 5) | (uint64_t(l.dl) << 1) |
                  uint64_t(l.agSkip);
    std::vector<uint8_t> w(12);
    for (int i = 0; i < 4; ++i) w[i] = uint8_t(hi >> (24 - 8 * i));
    for (int i = 0; i < 8; ++i) w[4 + i] = uint8_t(lo >> (56 - 8 * i));
    return w;
}

// Host-port helpers: the byte protocol the OS uses.
void hostReg(Esp2Core& e, uint32_t a, uint32_t v) {
    e.write8(0x11, uint8_t(v >> 16)); e.write8(0x12, uint8_t(v >> 8)); e.write8(0x13, uint8_t(v));
    e.write8(0x14, uint8_t(a >> 8)); e.write8(0x15, uint8_t(a)); e.write8(0x17, 0x80);
}
void hostIns(Esp2Core& e, uint32_t a, const std::vector<uint8_t>& w) {
    for (int i = 0; i < 12; ++i) e.write8(uint32_t(i), w[i]);
    e.write8(0x0C, uint8_t(a >> 8)); e.write8(0x0D, uint8_t(a)); e.write8(0x0F, 0x80);
}

// Harness around a program body: ADC -> 0FF/0FE, body, 0FD/0FC -> DAC, BIOZ, jump to 0.
std::vector<Line> harness(const std::vector<Line>& body) {
    std::vector<Line> p;
    Line in; in.B = 0x3EF; in.C = 0x0FF; in.mac = 3; in.D = 0x3EE; in.E = 0x1CB; in.F = 0x0FE; in.sh = 7;   // MOV ADC_L, MAC MOV ADC_R
    p.push_back(in);
    p.push_back(Line());
    p.insert(p.end(), body.begin(), body.end());
    p.push_back(Line());
    Line out; out.B = 0x0FD; out.C = 0x3ED; out.mac = 3; out.D = 0x0FC; out.E = 0x1CB; out.F = 0x3EC; out.sh = 7;
    p.push_back(out);
    Line bioz; bioz.alu = 0x19; p.push_back(bioz);
    Line jmp; jmp.alu = 0x1C; jmp.A = 0; jmp.B = 0; jmp.C = 0x3FF; p.push_back(jmp);
    p.push_back(Line());                                                                               // delay slot
    return p;
}

struct Rng {
    uint64_t s;
    uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    int below(int n) { return int(next() % uint32_t(n)); }
    template <size_t N> int pick(const int (&a)[N]) { return a[below(int(N))]; }
};

// Operand pools: plain storage plus the SPRs whose semantics the two execution paths implement separately.
const int kReads[] = {0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x010, 0x011, 0x01F, 0x0FF, 0x0FE, 0x0FD,
                      0x0FC, 0x200, 0x201, 0x1CB, 0x1CC, 0x1CD, 0x1D4, 0x1D3, 0x1D1, 0x1D5, 0x1DE, 0x1DF, 0x1FF, 0x1FE,
                      0x1F0, 0x3F1, 0x3FF, 0x3EF, 0x3EE, 0x3F9};
const int kWrites[] = {0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x010, 0x011, 0x01F, 0x0FD, 0x0FC, 0x200,
                       0x201, 0x1D0, 0x1D1, 0x1D2, 0x1D3, 0x1D5, 0x1DE, 0x1EF, 0x1EE, 0x1E0, 0x3FF, 0x3F9, 0x1CC, 0x3F1};
const int kAluOps[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                       0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1A, 0x1B, 0x0B, 0x0B};
const int kMacOps[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x14, 0x15, 0x16, 0x17,
                       0x18, 0x19, 0x1A, 0x1B, 0x0C, 0x1C};

Line randomLine(Rng& r) {
    Line l;
    if (r.below(8)) {                                    // ALU
        l.alu = r.pick(kAluOps);
        l.A = l.alu == 0x1B ? r.below(0x400) : r.pick(kReads);   // MOVcc: A is the condition mask (CMR preload)
        l.B = r.pick(kReads); l.C = r.pick(kWrites);
    }
    if (r.below(8)) {                                    // MAC
        l.mac = r.pick(kMacOps); l.D = r.pick(kReads); l.E = r.pick(kReads); l.F = r.pick(kWrites); l.sh = r.below(16);
    } else if (r.below(2)) {                             // the MAC MOV pseudo-instruction
        l.mac = 3; l.D = r.pick(kReads); l.E = 0x1CB; l.F = r.pick(kWrites); l.sh = 7;
    }
    if (r.below(3) == 0) {                               // AGEN: region 0 = delay RAM, region 1 = voice port
        const int ops[] = {0, 1, 2, 3, 4, 5, 7};
        l.ag = r.pick(ops); l.rgn = r.below(2); l.G = 0x200 + r.below(4); l.dl = r.below(16);
    }
    if (r.below(10) == 0) { l.aluSkip = r.below(2); l.macSkip = r.below(2); l.agSkip = r.below(2); }
    return l;
}

void setup(Esp2Core& e, const std::vector<Line>& prog, Rng& r, bool magTrunc) {
    for (size_t i = 0; i < prog.size(); ++i) hostIns(e, uint32_t(i), encode(prog[i]));
    for (int a = 0; a < 0x20; ++a) hostReg(e, uint32_t(a), r.next() & 0xFFFFFF);
    for (int a = 0x200; a < 0x204; ++a) hostReg(e, uint32_t(a), uint32_t(r.below(0x40)));            // AORs
    hostReg(e, 0x3DD, Esp2Core::kRamBase + 0xFF); hostReg(e, 0x3DE, 0xFF); hostReg(e, 0x3DF, Esp2Core::kRamBase);   // region 0
    hostReg(e, 0x3DA, Esp2Core::kVoicePort + 0x1F); hostReg(e, 0x3DB, 0x1F); hostReg(e, 0x3DC, Esp2Core::kVoicePort);   // region 1
    for (uint32_t p : {0x3FCu, 0x3FBu, 0x3FAu, 0x3C7u, 0x3C6u, 0x3C5u, 0x3C4u}) hostReg(e, p, uint32_t(r.below(0x20)));   // pointers
    hostReg(e, 0x1D5, uint32_t(r.below(17) - 8) & 0xFFFFFF);                                          // ALU_SHIFT
    if (magTrunc) hostReg(e, 0x3F3, 0x800);
    e.write8(0x19, 0x31);                                // run, IOZ enabled (the OS's run value)
}

void feed(Esp2Core& e, uint64_t s) {
    uint32_t x = uint32_t(s * 2654435761u);
    e.serialIn[0] = int32_t(x << 8) >> 8; e.serialIn[1] = int32_t((x * 7) << 8) >> 8;
    for (int i = 0; i < 32; ++i) e.voicePort[i] = int32_t(((x ^ uint32_t(i * 40503)) & 0xFFFFF0) << 8) >> 8;
}


namespace v {
constexpr uint32_t kBank2 = 0x20000000;                  // chip address of wave memory (bank 2)
constexpr uint32_t STOP0 = 0x0001, STOP1 = 0x0002, LPE = 0x0008, BLE = 0x0010, IRQE = 0x0020, IRQ = 0x0080;
constexpr uint32_t BYPASS = 0x0500;

std::vector<int16_t> syntheticBank() {
    std::vector<int16_t> m(1 << 16);
    uint32_t x = 1;
    for (size_t i = 0; i < m.size(); ++i) {
        x = x * 1664525u + 1013904223u;
        double v = 12000 * std::sin(double(i) * 0.05) + 6000 * std::sin(double(i) * 0.31) + double(int32_t(x) >> 20);
        m[i] = int16_t(std::lround(v));
    }
    return m;
}

struct Chip {
    VoiceCore c;
    void reg(int voice, uint32_t r, uint32_t v) { c.write(0x7C, uint32_t(voice), 4); c.write(r, v, 4); }
    uint32_t get(int voice, uint32_t r) { c.write(0x7C, uint32_t(voice), 4); return c.read(r, 4); }
    void voices(int n) { c.write(0x6C, uint32_t(n - 1), 4); }
    // full volume both sides, filter coefficients wide open, no ramps
    void basic(int v, uint32_t start, uint32_t end, uint32_t fc, uint32_t cr) {
        reg(v, 0x08, 0xFFFF); reg(v, 0x10, 0xFFFF); reg(v, 0x24, 0xFFFF); reg(v, 0x1C, 0xFFFF);
        reg(v, 0x2C, kBank2 + start); reg(v, 0x30, kBank2 + end); reg(v, 0x38, kBank2 + start); reg(v, 0x3C, 0);
        reg(v, 0x04, fc); reg(v, 0x00, cr);
    }
};

// Level law at full volume register 0xFFFF: table entry 0xFFF = (0x1FF << 7) >> 1 = 32704, output = s * 32704 >> 11.

}

// A reverb-like line: MAC on plain registers, delay-RAM traffic on every other line, plain ALU moves/adds.
Line effectLine(Rng& r, int i) {
    Line l;
    const int alus[] = {0x0B, 0x0B, 0x00, 0x03, 0x0D};
    l.alu = alus[r.below(5)]; l.A = r.below(0x100); l.B = r.below(0x100); l.C = 0x100 + r.below(0x40);
    l.mac = r.below(2) ? 0x08 : 0x00; l.D = r.below(0x100); l.E = 0x140 + r.below(0x40); l.F = 0x180 + r.below(0x40); l.sh = 6 + r.below(2);
    if (i % 2 == 0) { l.ag = r.below(2) ? 0 : 1; l.rgn = 0; l.G = 0x200 + r.below(4); l.dl = r.below(16); }
    return l;
}

uint64_t esp2Hash(bool fast, double& ms, bool effect = false) {
    Rng r{12345};
    std::vector<Line> body;
    for (int i = 0; i < 180; ++i) body.push_back(effect ? effectLine(r, i) : randomLine(r));
    Esp2Core e; e.fastPath = fast;
    setup(e, harness(body), r, false);
    uint64_t h = 1469598103934665603ull;
    const auto t0 = Clock::now();
    for (uint64_t s = 0; s < uint64_t(getenv("BENCH_SAMPLES") ? atoi(getenv("BENCH_SAMPLES")) : 44100); ++s) {
        feed(e, s);
        e.runTo((s + 1) * 192); e.sampleTick();
        h ^= uint32_t(e.dacOut[0]); h *= 1099511628211ull; h ^= uint32_t(e.dacOut[1]); h *= 1099511628211ull;
    }
    ms = msSince(t0);
    return h;
}
}  // namespace

int main(int argc, char** argv) {
    double ms = 0;
    const bool effOnly = argc > 1 && std::strcmp(argv[1], "effect") == 0;
    if (effOnly || argc == 1) {
        const uint64_t he = esp2Hash(true, ms, true);
        std::printf("ESP2 core (fast path), effect-like program: %6.1f ms per second   [hash %016" PRIx64 "]\n", ms, he);
        if (effOnly) return 0;
    }
    const uint64_t hf = esp2Hash(true, ms);
    if (argc > 1 && std::strcmp(argv[1], "esp2") == 0) { std::printf("ESP2 fast: %.1f ms [%016" PRIx64 "]\n", ms, hf); return 0; }
    std::printf("ESP2 core (fast path), 190-line program: %8.1f ms per second   [hash %016" PRIx64 ", executed/sample 192]\n", ms, hf);
    const uint64_t hr = esp2Hash(false, ms);
    std::printf("ESP2 core (reference),  190-line program: %8.1f ms per second   [hash %016" PRIx64 "]\n", ms, hr);

    {   // voice chip: 32 voices
        using namespace v;
        const auto mem = syntheticBank();
        Chip k; k.c.setBank(2, mem); k.voices(32);
        const uint32_t modes[] = {0x000, 0x100, 0x200, 0x300, 0x500, 0x600, 0x700};
        for (int i = 0; i < 32; ++i) {
            k.reg(i, 0x08, 0xC000); k.reg(i, 0x10, 0xC000); k.reg(i, 0x24, 0x4000); k.reg(i, 0x1C, 0x3000);
            k.reg(i, 0x48, (0x020u << 9) | 0x013); k.reg(i, 0x18, 0x1FF);
            k.reg(i, 0x2C, kBank2 + 1000u * uint32_t(i)); k.reg(i, 0x30, kBank2 + 1000u * uint32_t(i) + 900); k.reg(i, 0x38, kBank2 + 1000u * uint32_t(i));
            k.reg(i, 0x04, 1500 + 37 * uint32_t(i)); k.reg(i, 0x00, modes[i % 7] | 0x0008 | uint32_t(i % 10) << 12);
        }
        int64_t acc = 0;
        const auto t0 = Clock::now();
        for (int s = 0; s < 44100; ++s) { int32_t l, r; k.c.tick(l, r); acc += l + 3 * int64_t(r); }
        std::printf("Voice chip, 32 voices:                    %8.1f ms per second   [%lld]\n", msSince(t0), (long long)acc);
    }
    {   // 68k: busy loop
        OsImage os; os.bytes.assign(0x1000, 0);
        const uint16_t prog[] = {0x2200, 0x5281, 0x0C81, 0x0000, 0xFFFF, 0x66F6, 0x60F2};   // count in d1 forever
        for (size_t i = 0; i < sizeof prog / 2; ++i) { os.bytes[0x80 + 2 * i] = uint8_t(prog[i] >> 8); os.bytes[0x81 + 2 * i] = uint8_t(prog[i]); }
        Machine m; Machine::Config cfg; cfg.esp2Stub = true; std::string err;
        if (!m.init(os, cfg, err)) { std::printf("init: %s\n", err.c_str()); return 1; }
        const auto t0 = Clock::now();
        m.runUntil(m.cyclesFromMs(1000));
        std::printf("68k busy loop + machine (no ESP2):        %8.1f ms per second\n", msSince(t0));
        Machine m2; Machine::Config c2; std::string e2;
        m2.init(os, c2, e2);
        const auto t1 = Clock::now();
        m2.runUntil(m2.cyclesFromMs(1000));
        std::printf("68k busy loop + machine + ESP2 core (idle program): %8.1f ms per second\n", msSince(t1));
    }
    return 0;
}
