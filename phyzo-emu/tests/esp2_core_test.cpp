// ESP2 core test: needs no ROM data. All microcode here is synthetic, built with the instruction field layout of the
// public Object Format appendix.
//  1. Pass-through: ADC -> DAC through registers, output = input delayed by one period (both execution paths).
//  2. Equivalence: seeded random programs (ALU, MAC, AGEN, skips, indirection, delay RAM and voice-port traffic) run on
//     the translated core and the reference interpreter in lockstep; every register, the DAC/aux lines, RAM and the
//     statistics must match after every sample period.
//  3. Golden hash: the translated core's DAC output over all random programs must equal the recorded value, so
//     speed work cannot change the output by accident (update it only for an intended behaviour change).
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "esp2_core.h"

namespace {

int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

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

std::string diff(const Esp2Core& f, const Esp2Core& r) {
    char b[160];
    if (f.pc() != r.pc()) { std::snprintf(b, sizeof b, "pc %03X vs %03X", f.pc(), r.pc()); return b; }
    if (f.cycle() != r.cycle()) return "cycle";
    for (int i = 0; i < 2; ++i) if (f.dacOut[i] != r.dacOut[i]) { std::snprintf(b, sizeof b, "dac %d: %d vs %d", i, f.dacOut[i], r.dacOut[i]); return b; }
    for (int i = 0; i < 8; ++i) if (f.auxOut[i] != r.auxOut[i]) return "aux";
    for (int a = 0; a < Esp2Core::kRegs; ++a)
        if (f.reg(a) != r.reg(a)) { std::snprintf(b, sizeof b, "reg %03X: %06X vs %06X", a, f.reg(a), r.reg(a)); return b; }
    for (int i = 0; i < 32; ++i) if (f.voicePort[i] != r.voicePort[i]) return "voice port";
    for (uint32_t i = 0; i < 0x100; ++i) if (f.ramWord(i) != r.ramWord(i)) { std::snprintf(b, sizeof b, "ram %02X", i); return b; }
    if (f.executed != r.executed || f.suspendedCycles != r.suspendedCycles) return "executed/suspended counts";
    if (f.macSat != r.macSat || f.aluSat != r.aluSat || f.overruns != r.overruns) return "saturation/overrun counts";
    if (f.memReads != r.memReads || f.memWrites != r.memWrites || f.regionViolations != r.regionViolations) return "memory counts";
    return "";
}

}  // namespace

int main() {
    const uint64_t ips = 192;

    // 1. Pass-through on both execution paths.
    for (bool fast : {true, false}) {
        Line body; body.B = 0x0FF; body.C = 0x0FD; body.mac = 3; body.D = 0x0FE; body.E = 0x1CB; body.F = 0x0FC; body.sh = 7;
        Esp2Core e; e.fastPath = fast;
        auto prog = harness({body});
        for (size_t i = 0; i < prog.size(); ++i) hostIns(e, uint32_t(i), encode(prog[i]));
        e.write8(0x19, 0x31);
        std::vector<int32_t> inL, inR; size_t mism = 0;
        for (uint64_t s = 0; s < 4000; ++s) {
            feed(e, s);
            inL.push_back(e.serialIn[0]); inR.push_back(e.serialIn[1]);
            e.runTo((s + 1) * ips); e.sampleTick();
            if (s >= 1 && (e.dacOut[0] != inL[s - 1] || e.dacOut[1] != inR[s - 1])) ++mism;
        }
        CHECK(mism == 0);
        CHECK(e.overruns == 0 && e.biozSuspends > 3900);
    }

    // 2 + 3. Random programs: translated core vs reference interpreter, and the golden hash.
    uint64_t hash = 1469598103934665603ull;
    int programs = 0, divergent = 0;
    uint64_t totals[6] = {0, 0, 0, 0, 0, 0};
    for (uint64_t seed = 1; seed <= 300; ++seed) {
        Rng r{seed};
        std::vector<Line> body;
        const int n = 20 + r.below(100);
        for (int i = 0; i < n; ++i) body.push_back(randomLine(r));
        const auto prog = harness(body);
        const bool mag = seed % 3 == 0;
        Esp2Core fast, ref;
        fast.fastPath = true; ref.fastPath = false;
        Rng r1{seed * 977}, r2{seed * 977};
        setup(fast, prog, r1, mag); setup(ref, prog, r2, mag);
        ++programs;
        for (uint64_t s = 0; s < 300; ++s) {
            feed(fast, s); feed(ref, s);
            fast.runTo((s + 1) * ips); ref.runTo((s + 1) * ips);
            fast.sampleTick(); ref.sampleTick();
            const std::string d = diff(fast, ref);
            if (!d.empty()) {
                if (divergent < 5) std::printf("seed %" PRIu64 " sample %" PRIu64 ": translated and reference differ (%s)\n", seed, s, d.c_str());
                ++divergent; break;
            }
            for (int i = 0; i < 2; ++i) { hash ^= uint32_t(fast.dacOut[i]); hash *= 1099511628211ull; }
        }
        totals[0] += fast.macSat; totals[1] += fast.aluSat; totals[2] += fast.memReads + fast.memWrites; totals[3] += fast.voicePortReads;
        totals[4] += fast.executed; totals[5] += fast.biozSuspends;
    }
    CHECK(divergent == 0);
    // The random programs must actually reach the paths they are meant to cover.
    CHECK(totals[0] > 0 && totals[1] > 0 && totals[2] > 0 && totals[3] > 0);

    const uint64_t kGolden = 0xde4d05b4661b235aull;   // recorded from the v0.6 core (2026-10-02)
    std::printf("esp2_core_test: %d programs, %" PRIu64 " instructions, %" PRIu64 " BIOZ waits, %" PRIu64 " MAC / %" PRIu64
                " ALU saturations, %" PRIu64 " memory and %" PRIu64 " voice-port accesses; translated == reference: %s; DAC hash %016" PRIx64 "\n",
                programs, totals[4], totals[5], totals[0], totals[1], totals[2], totals[3], divergent ? "NO" : "yes", hash);
    if (hash != kGolden) { std::printf("FAIL: DAC hash differs from the recorded %016" PRIx64 "\n", kGolden); ++failures; }

    std::printf("esp2_core_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
