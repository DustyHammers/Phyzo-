// Voice-chip core test: needs no ROM data. Wave memory is a synthetic bank.
//  1. Exact behaviour against a small independent model: forward loop at rate 1.0, linear interpolation at rate 0.5,
//     one-shot stop (STOP0), loop-end IRQ and its acknowledge through IRQV.
//  2. Golden hash: 8 voices covering every filter mode, loop type, ramps and both stereo sides; the output must equal
//     the recorded value, so speed work cannot change it by accident (update only for an intended behaviour change).
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <vector>
#include "state_io.h"
#include "voice_core.h"

namespace {

int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

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

// Level law at full volume register 0xFFFF: table entry 0xFFF = (0x1FF << 7) >> 1 = 32704, output = s * 32704 >> 12
// (A-17 headroom).
int32_t full(int32_t s) { return int32_t((int64_t(s) * 32704) >> 12); }

}  // namespace

int main() {
    const auto mem = syntheticBank();

    {   // forward loop, rate 1.0, bypass filter
        Chip k; k.c.setBank(2, mem); k.voices(1);
        const uint32_t start = 1000, end = 1100;
        k.basic(0, start, end, 2048, BYPASS | LPE);
        uint32_t addr = start; int bad = 0;
        for (int i = 0; i < 1000; ++i) {
            int32_t l, r; k.c.tick(l, r);
            const int32_t e = full(mem[addr]);
            if (l != e || r != e) ++bad;
            if (++addr > end) addr = start + (addr - end);
        }
        CHECK(bad == 0);
    }
    {   // rate 0.5: every other sample is the average of two neighbours (11-bit fraction)
        Chip k; k.c.setBank(2, mem); k.voices(1);
        k.basic(0, 2000, 3000, 1024, BYPASS | LPE);
        uint64_t acc = uint64_t(2000) << 11; int bad = 0;
        for (int i = 0; i < 1500; ++i) {
            int32_t l, r; k.c.tick(l, r);
            const uint64_t a = acc >> 11; const int32_t f = int32_t(acc & 0x7FF);
            const int32_t s = (mem[a] * (2048 - f) + mem[a + 1] * f) >> 11;
            if (l != full(s)) ++bad;
            acc += 1024;
        }
        CHECK(bad == 0);
    }
    {   // one-shot: plays to END, then STOP0 is set and the voice is silent
        Chip k; k.c.setBank(2, mem); k.voices(1);
        k.basic(0, 5000, 5050, 2048, BYPASS);
        int sounding = 0;
        for (int i = 0; i < 200; ++i) { int32_t l, r; k.c.tick(l, r); if (l) ++sounding; }
        CHECK(sounding >= 49 && sounding <= 51);
        CHECK(k.get(0, 0x00) & STOP0);
        CHECK(k.c.voiceStops == 1);
    }
    {   // START = END (table-sweep frame chaining): the voice keeps running and is not stopped
        Chip k; k.c.setBank(2, mem); k.voices(1);
        k.basic(0, 6000, 6000, 2048, BYPASS | LPE);
        int sounding = 0;
        for (int i = 0; i < 100; ++i) { int32_t l, r; k.c.tick(l, r); if (l) ++sounding; }
        CHECK(sounding > 0);
        CHECK(!(k.get(0, 0x00) & (STOP0 | STOP1)));
        CHECK(k.c.voiceStops == 0);
    }
    {   // loop-end IRQ on voice 3, acknowledged by reading IRQV
        Chip k; k.c.setBank(2, mem); k.voices(4);
        for (int v = 0; v < 4; ++v) k.basic(v, 100, 200, 2048, BYPASS | STOP1);   // stopped
        k.basic(3, 100, 160, 2048, BYPASS | LPE | IRQE);
        CHECK(!k.c.irqLine());
        int raisedAt = -1;
        for (int i = 0; i < 100 && raisedAt < 0; ++i) { int32_t l, r; k.c.tick(l, r); if (k.c.irqLine()) raisedAt = i; }
        CHECK(raisedAt == 60);
        CHECK((k.c.read(0x78, 4) & 0xFF) == 3);           // IRQV names the voice and acknowledges it
        CHECK(!k.c.irqLine());
        CHECK(!(k.get(3, 0x00) & IRQ));
    }

    {   // golden hash: every filter mode, loop types, ramps, channels
        Chip k; k.c.setBank(2, mem); k.voices(8);
        struct V { uint32_t mode, loop, fc, start, end, k1, k2, lv, rv, res; uint16_t lramp, k1ramp; };
        const V vs[8] = {
            {0x000, LPE, 2048, 100, 900, 0x8000, 0x1000, 0xC000, 0x9000, 0, 0, 0},
            {0x100, LPE | BLE, 3000, 1000, 1500, 0x4000, 0x2000, 0xF000, 0xF000, 0, 0xFF00, 0x0010},
            {0x200, LPE, 1500, 2000, 2600, 0x2000, 0x6000, 0xE000, 0xA000, 0, 0x0101, 0},
            {0x300, BLE, 2500, 3000, 3900, 0xF000, 0x0800, 0xD000, 0xD000, 0, 0, 0xFFF1},
            {0x500, LPE, 777, 4000, 4300, 0, 0, 0xFFFF, 0x8000, 0, 0, 0},
            {0x600, LPE, 2048, 5000, 5400, 0x3000, 0x3000, 0xE800, 0xE800, (0x020u << 9) | 0x013, 0, 0x0020},
            {0x700, LPE, 4096, 6000, 7000, 0x1800, 0x2400, 0xF800, 0xC000, (0x010u << 9) | 0x000, 0, 0},
            {0x600, 0, 2048, 8000, 30000, 0x7000, 0x7000, 0xFFFF, 0xFFFF, (0x030u << 9) | 0x10F, 0xFF01, 0}};   // damping 0: saturates
        for (int v = 0; v < 8; ++v) {
            const V& x = vs[v];
            k.reg(v, 0x08, x.lv); k.reg(v, 0x10, x.rv); k.reg(v, 0x24, x.k1); k.reg(v, 0x1C, x.k2); k.reg(v, 0x48, x.res);
            k.reg(v, 0x0C, x.lramp); k.reg(v, 0x28, x.k1ramp); k.reg(v, 0x18, 0x1FF);
            k.reg(v, 0x2C, kBank2 + x.start); k.reg(v, 0x30, kBank2 + x.end); k.reg(v, 0x38, kBank2 + x.start);
            k.reg(v, 0x04, x.fc); k.reg(v, 0x00, x.mode | x.loop | uint32_t(v % 10) << 12);
        }
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](int64_t x) { h ^= uint64_t(x); h *= 1099511628211ull; };
        Chip copy; copy.c.setBank(2, mem);
        int copyMismatch = 0;
        for (int i = 0; i < 30000; ++i) {
            int32_t l, r; k.c.tick(l, r); mix(l); mix(r);
            for (int c = 0; c < VoiceCore::kChannels; ++c) { mix(k.c.chan[c][0]); mix(k.c.chan[c][1]); }
            if (i == 15000) k.reg(1, 0x18, 0x1FF);         // re-arm a ramp mid-way
            if (i == 9000) {                                 // state round trip: a restored copy must continue identically
                StateWriter w; k.c.save(w);
                StateReader rd(w.bytes.data(), w.bytes.size());
                copy.c.load(rd);
                CHECK(rd.ok() && rd.atEnd());
            }
            if (i > 9000) {
                int32_t cl, cr; copy.c.tick(cl, cr);
                if (cl != l || cr != r) ++copyMismatch;
                if (i == 15000) copy.reg(1, 0x18, 0x1FF);
            }
        }
        CHECK(copyMismatch == 0);
        CHECK(k.c.resonantSamples > 0 && k.c.bypassSamples > 0 && k.c.stateClamps > 0);
        const uint64_t kGolden = 0x5c0654db6b80512bull;   // A-17 headroom (volume >> 12), recorded 2026-10-03
        std::printf("voice_core_test: resonant samples %llu, bypass samples %llu, state saturations %llu; render hash %016" PRIx64 "\n",
                    (unsigned long long)k.c.resonantSamples, (unsigned long long)k.c.bypassSamples, (unsigned long long)k.c.stateClamps, h);
        if (h != kGolden) { std::printf("FAIL: render hash differs from the recorded %016" PRIx64 "\n", kGolden); ++failures; }
    }

    std::printf("voice_core_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
