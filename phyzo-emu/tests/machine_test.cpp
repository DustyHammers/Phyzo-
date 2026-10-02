// Machine smoke test: needs no ROM data. A small 68k program of our own stands in for the OS image and exercises
// the CPU core (Musashi), RAM, serial channel B in both directions (the MIDI port) and the hard-stall detector.
#include <cstdio>
#include <string>
#include <vector>
#include "machine.h"
#include "os_image.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// Loaded at 0x4000; main() at 0x4080 (os_profile.h). Assembled by hand, checked with os_disasm:
const uint16_t kProgram[] = {
    0x13FC, 0x0005, 0x00F0, 0x071A,     // 4080 move.b #$05,$F0071A    channel B: receiver and transmitter on
    0x7000,                             // 4088 moveq  #0,d0
    0x5280,                             // 408A addq.l #1,d0
    0x23C0, 0x0BE0, 0x0100,             // 408C move.l d0,$0BE00100
    0x0C80, 0x0000, 0x03E8,             // 4092 cmpi.l #1000,d0
    0x66F0,                             // 4098 bne    $408A
    0x13FC, 0x0090, 0x00F0, 0x071B,     // 409A move.b #$90,$F0071B    send one MIDI byte
    0x0839, 0x0000, 0x00F0, 0x0719,     // 40A2 btst   #0,$F00719      wait for a received byte
    0x67F6,                             // 40AA beq    $40A2
    0x1239, 0x00F0, 0x071B,             // 40AC move.b $F0071B,d1      read it
    0x13C1, 0x0BE0, 0x0200,             // 40B2 move.b d1,$0BE00200    store it
    0x60FE};                            // 40B8 bra    *
}  // namespace

OsImage syntheticOs() {
    OsImage os;
    os.bytes.assign(0x1000, 0);
    for (size_t i = 0; i < sizeof kProgram / 2; ++i) {
        os.bytes[0x80 + 2 * i] = uint8_t(kProgram[i] >> 8);
        os.bytes[0x81 + 2 * i] = uint8_t(kProgram[i]);
    }
    return os;
}

// Runs the program to its end: counting loop, MIDI byte out, one byte in, stall. Returns the final state.
std::vector<uint8_t> runToEnd(Machine& m) {
    m.runUntil(m.cyclesFromMs(50), [&] { return m.peek(0x0BE00100, 4) == 1000; });
    m.runUntil(m.cycles() + m.cyclesFromMs(2));
    m.serial.queueRx(1, {0x3C}, m.cycles());
    m.runUntil(m.cycles() + m.cyclesFromMs(5), [&] { return m.peek(0x0BE00200, 1) == 0x3C; });
    m.runUntil(m.cycles() + m.cyclesFromMs(300));
    return m.saveState();
}

int main() {
    const OsImage os = syntheticOs();
    Machine m; Machine::Config cfg; std::string err;
    CHECK(m.init(os, cfg, err));

    // The counting loop finishes and its result is in RAM.
    auto counted = [&] { return m.peek(0x0BE00100, 4) == 1000; };
    CHECK(m.runUntil(m.cyclesFromMs(50), counted) == Machine::Stop::Predicate);

    // The transmitted byte leaves channel B one character time later.
    m.runUntil(m.cycles() + m.cyclesFromMs(2));
    CHECK(m.midiOut.size() == 1 && m.midiOut[0].second == 0x90);

    // A byte arriving on channel B is read by the program and stored.
    const uint64_t sent = m.cycles();
    m.serial.queueRx(1, {0x3C}, sent);
    auto stored = [&] { return m.peek(0x0BE00200, 1) == 0x3C; };
    CHECK(m.runUntil(sent + m.cyclesFromMs(5), stored) == Machine::Stop::Predicate);

    // The final `bra *` is reported as a hard stall at its address.
    CHECK(m.runUntil(m.cycles() + m.cyclesFromMs(1000)) == Machine::Stop::HardStall);
    CHECK(m.stallPc == 0x40B8);
    CHECK(m.unmapped.empty());

    // State round trip: save in the middle of the counting loop, restore into a new machine, run both to the end.
    {
        Machine a; CHECK(a.init(os, cfg, err));
        a.runUntil(a.cycles() + 7777);
        const uint32_t mid = a.peek(0x0BE00100, 4);
        CHECK(mid > 0 && mid < 1000);
        const std::vector<uint8_t> saved = a.saveState();
        Machine b; CHECK(b.init(os, cfg, err));
        CHECK(b.loadState(saved, err));
        CHECK(b.peek(0x0BE00100, 4) == mid && b.cycles() == a.cycles());
        const auto endA = runToEnd(a), endB = runToEnd(b);
        CHECK(endA == endB);
        CHECK(a.midiOut.size() == 1 && b.midiOut.size() == 1);
        // A damaged state is refused.
        std::vector<uint8_t> bad = saved; bad.resize(bad.size() / 2);
        Machine c; CHECK(c.init(os, cfg, err));
        CHECK(!c.loadState(bad, err));
    }

    // Several machines in one process (plugin instances) take turns on the single CPU core: interleaved runs must
    // end exactly as a machine that ran alone.
    {
        // (Same run steps for all three: the step boundaries themselves decide where the CPU pauses.)
        Machine solo; CHECK(solo.init(os, cfg, err));
        for (int i = 0; i < 40; ++i) solo.runUntil(solo.cycles() + 997 + 37 * i);
        const auto endSolo = runToEnd(solo);
        Machine x, y; CHECK(x.init(os, cfg, err)); CHECK(y.init(os, cfg, err));
        for (int i = 0; i < 40; ++i) {
            x.runUntil(x.cycles() + 997 + 37 * i);
            y.runUntil(y.cycles() + 997 + 37 * i);
        }
        const auto endY = runToEnd(y), endX = runToEnd(x);
        CHECK(endX == endSolo && endY == endSolo);
    }

    std::printf("machine_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
