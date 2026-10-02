// ESP2 host-port placeholder: register file + instruction memory with read-back,
// command bytes that complete instantly, and a run-handshake rule standing in for
// the microcode. No DSP execution.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

class Esp2Stub {
public:
    static constexpr int kRegs = 1024, kInstr = 300;

    uint8_t read8(uint32_t off, uint64_t cycle, double cpuHz);
    void write8(uint32_t off, uint8_t v);

    // Logging: decoded commands (register/instruction writes and reads, control).
    FILE* log = nullptr;
    double timeMs = 0; std::string source;   // set by the machine before each access

    bool running() const { return (control & 0x04) == 0 && (control & 0x01); }

    // Rule C5: while running, register 0x0F7 reads 0 (microcode acknowledged).
    bool mailboxAutoClear = true;

    // statistics
    uint64_t regWrites = 0, regReads = 0, instrWrites = 0, instrReads = 0;
    uint64_t mailboxReadsWhileRunning = 0, mailboxReadsNonZero = 0, controlWrites = 0;
    uint64_t readoutReads = 0, badInstrAddr = 0;
    int lastControl = -1;

    std::array<uint32_t, kRegs> regs{};
    std::array<std::array<uint8_t, 12>, kInstr> instr{};
    uint8_t port[0x20] = {0};
    uint8_t control = 0;
private:
    void regCommand(uint8_t cmd);
    void instrCommand(uint8_t cmd);
};
