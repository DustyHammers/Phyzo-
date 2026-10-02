#include "esp2_stub.h"

// Port map (byte offsets from the ESP2 base):
//  0x00-0x0B instruction data (96 bits), 0x0C-0x0D instruction address, 0x0F instruction command
//  0x11-0x13 register data (24 bits, MSB first), 0x14-0x15 register address, 0x17 register command
//  0x19 host control, 0x1B-0x1D 24-bit readout
// Commands: 0x80 write, 0x81 read; bit 7 of the command byte reads back 0 (done).

uint8_t Esp2Stub::read8(uint32_t off, uint64_t cycle, double cpuHz) {
    off &= 0x1f;
    if (off >= 0x1b && off <= 0x1d) {
        // Stable readout: a counter advancing once per 44.1 kHz frame.
        ++readoutReads;
        uint32_t frame = uint32_t(double(cycle) * 44100.0 / cpuHz) & 0xffffff;
        return uint8_t(frame >> (8 * (0x1d - off)));
    }
    if (off == 0x19) return control;
    return port[off];
}

void Esp2Stub::write8(uint32_t off, uint8_t v) {
    off &= 0x1f;
    if (off == 0x0f) { instrCommand(v); return; }
    if (off == 0x17) { regCommand(v); return; }
    if (off == 0x19) {
        control = v;
        ++controlWrites;
        if (log) std::fprintf(log, "%.3f,%s,CTRL,,%02X%s\n", timeMs, source.c_str(), v,
                              running() ? " (run)" : "");
        lastControl = v;
        return;
    }
    port[off] = v;
}

void Esp2Stub::regCommand(uint8_t cmd) {
    uint32_t addr = ((uint32_t(port[0x14]) << 8) | port[0x15]) & 0x3ff;
    if (cmd == 0x80) {
        uint32_t val = (uint32_t(port[0x11]) << 16) | (uint32_t(port[0x12]) << 8) | port[0x13];
        regs[addr] = val;
        ++regWrites;
        if (log) std::fprintf(log, "%.3f,%s,REGW,%03X,%06X\n", timeMs, source.c_str(), addr, val);
    } else if (cmd == 0x81) {
        uint32_t val = regs[addr];
        if (addr == 0x0f7 && running()) {
            ++mailboxReadsWhileRunning;
            if (mailboxAutoClear) val = 0;
        }
        if (addr == 0x0f7 && val) ++mailboxReadsNonZero;
        port[0x11] = val >> 16; port[0x12] = val >> 8; port[0x13] = val;
        ++regReads;
        if (log) std::fprintf(log, "%.3f,%s,REGR,%03X,%06X\n", timeMs, source.c_str(), addr, val);
    } else if (log) {
        std::fprintf(log, "%.3f,%s,REGCMD?,%03X,%02X\n", timeMs, source.c_str(), addr, cmd);
    }
    port[0x17] = cmd & 0x7f;   // done
}

void Esp2Stub::instrCommand(uint8_t cmd) {
    uint32_t addr = (uint32_t(port[0x0c]) << 8) | port[0x0d];
    char hex[25];
    if (addr >= kInstr) {
        ++badInstrAddr;
        if (log) std::fprintf(log, "%.3f,%s,INSCMD-BADADDR,%03X,%02X\n", timeMs, source.c_str(), addr, cmd);
        port[0x0f] = cmd & 0x7f;
        return;
    }
    if (cmd == 0x80) {
        for (int i = 0; i < 12; ++i) instr[addr][i] = port[i];
        ++instrWrites;
        if (log) {
            for (int i = 0; i < 12; ++i) std::snprintf(hex + 2 * i, 3, "%02X", port[i]);
            std::fprintf(log, "%.3f,%s,INSW,%03X,%s\n", timeMs, source.c_str(), addr, hex);
        }
    } else if (cmd == 0x81) {
        for (int i = 0; i < 12; ++i) port[i] = instr[addr][i];
        ++instrReads;
        if (log) std::fprintf(log, "%.3f,%s,INSR,%03X,\n", timeMs, source.c_str(), addr);
    } else if (log) {
        std::fprintf(log, "%.3f,%s,INSCMD?,%03X,%02X\n", timeMs, source.c_str(), addr, cmd);
    }
    port[0x0f] = cmd & 0x7f;
}
