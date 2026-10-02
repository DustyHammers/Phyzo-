#include "mc68340.h"
#include <algorithm>

// ---------------------------------------------------------------- Timer
// CR bits: 15 SWR, 14 IE2 (TO irq enable), 13 IE1, 12 IE0, 11 TGE, 10 PSE,
//          9 CPE, 8 CLK, 7-5 POT, 4-2 MODE, 1-0 OC.
// SR bits: 15 IRQ, 14 TO, 13 TG, 12 TC (write 1 to clear), 10 ON.
// Counter: loaded from PREL1 on start, decrements, sets TO at zero and
// reloads from PREL1 on the next count (MAME ICOC behaviour).

namespace {
constexpr uint16_t CR_SWR = 0x8000, CR_IE2 = 0x4000, CR_PSE = 0x0400, CR_CPE = 0x0200;
constexpr uint16_t SR_TO = 0x4000, SR_FLAGS = 0x7000, SR_ON = 0x0400, SR_IRQ = 0x8000;

inline void setByte(uint16_t& reg, uint32_t off, uint8_t v) {
    if (off & 1) reg = (reg & 0xff00) | v; else reg = (reg & 0x00ff) | (uint16_t(v) << 8);
}
inline uint8_t getByte(uint16_t reg, uint32_t off) { return (off & 1) ? (reg & 0xff) : (reg >> 8); }
inline void setByte32(uint32_t& reg, uint32_t off, uint8_t v) {
    int sh = 8 * (3 - (off & 3));
    reg = (reg & ~(0xffu << sh)) | (uint32_t(v) << sh);
}
inline uint8_t getByte32(uint32_t reg, uint32_t off) { return (reg >> (8 * (3 - (off & 3)))) & 0xff; }
}

uint64_t Timer::divider() const {
    // PSE clear: counter clocked at fsys/2 (the OS's configuration).
    // PSE set: prescaler tap from POT (000 = /256). Derived; unused by the OS.
    if (!(cr & CR_PSE)) return 2;
    unsigned pot = (cr >> 5) & 7;
    return 2ull * (pot == 0 ? 256u : (1u << pot));
}

uint16_t Timer::counter(uint64_t now) const {
    if (!on || now < anchor) return 0;
    uint64_t k = (now - anchor) / divider();
    return k >= loaded ? 0 : uint16_t(loaded - k);
}

int Timer::irqLevel() const {
    if ((sr & SR_TO) && (cr & CR_IE2)) return (ir >> 8) & 7;
    return 0;
}

uint8_t Timer::read8(uint32_t off) {
    advance(host.now());
    switch (off & 0x3e) {
    case 0x00: return getByte(mcr, off);
    case 0x04: return getByte(ir, off);
    case 0x06: return getByte(cr, off);
    case 0x08: {
        uint16_t v = (sr & SR_FLAGS) | (on ? SR_ON : 0) | (irqLevel() ? SR_IRQ : 0);
        return getByte(v, off);
    }
    case 0x0a: return getByte(counter(host.now()), off);
    case 0x0c: return getByte(prel1, off);
    case 0x0e: return getByte(prel2, off);
    case 0x10: return getByte(com, off);
    default: return 0;
    }
}

void Timer::write8(uint32_t off, uint8_t v) {
    advance(host.now());
    switch (off & 0x3e) {
    case 0x00: setByte(mcr, off, v); break;
    case 0x04: setByte(ir, off, v); host.irqChanged(); break;
    case 0x06: setByte(cr, off, v); crChanged(); host.irqChanged(); break;
    case 0x08: {
        uint16_t w = (off & 1) ? v : uint16_t(v) << 8;
        sr &= ~(w & SR_FLAGS);
        host.irqChanged();
        break;
    }
    case 0x0c: setByte(prel1, off, v); break;   // takes effect at the next reload
    case 0x0e: setByte(prel2, off, v); break;
    case 0x10: setByte(com, off, v); break;
    default: break;
    }
}

void Timer::crChanged() {
    bool enable = (cr & (CR_SWR | CR_CPE)) == (CR_SWR | CR_CPE);
    if (enable && !on) {
        on = true;
        anchor = host.now() + divider();   // next counter clock edge loads PREL1
        loaded = prel1;
        nextZero = anchor + uint64_t(loaded) * divider();
        host.reschedule();
    } else if (!enable && on) {
        on = false;
        nextZero = kNever;
    }
}

void Timer::advance(uint64_t now) {
    bool fired = false;
    while (on && now >= nextZero) {
        sr |= SR_TO;
        ++timeouts;
        fired = true;
        anchor = nextZero + divider();
        loaded = prel1;
        nextZero = anchor + uint64_t(loaded) * divider();
    }
    if (fired) host.irqChanged();
}

// ---------------------------------------------------------------- Serial
// DUART-compatible serial module. Offsets relative to module base + 0x700.
// SR: 0 RxRDY, 1 FFULL, 2 TxRDY, 3 TxEMP, 4-7 errors (always 0 here).
// ISR/IER: 0 TxRDYA, 1 RxRDYA, 3 (crystal not ready; reads 0), 4 TxRDYB, 5 RxRDYB.

uint8_t Serial::status(int c) const {
    const SerialChannel& s = ch_[c];
    uint8_t v = 0;
    if (!s.fifo.empty()) v |= 0x01;
    if (s.fifo.size() >= 3) v |= 0x02;
    if (s.txEn && !s.holdFull) v |= 0x04;
    if (s.txEn && !s.holdFull && !s.shiftBusy) v |= 0x08;
    return v;
}

uint8_t Serial::isr() const {
    uint8_t v = 0;
    for (int c = 0; c < 2; ++c) {
        uint8_t st = status(c);
        bool rxSel = (ch_[c].mr1 & 0x40) ? (st & 0x02) : (st & 0x01);
        if (st & 0x04) v |= c ? 0x10 : 0x01;
        if (rxSel) v |= c ? 0x20 : 0x02;
    }
    return v;   // bit 3 (crystal unstable) always 0
}

int Serial::irqLevel() const { return (isr() & ier) ? (ilr & 7) : 0; }

uint8_t Serial::read8(uint32_t off, bool& handled) {
    advance(host.now());
    handled = true;
    switch (off & 0x3f) {
    case 0x00: return mcr >> 8;
    case 0x01: return mcr & 0xff;
    case 0x04: return ilr;
    case 0x05: return ivr;
    case 0x10: return ch_[0].mr1;
    case 0x11: return status(0);
    case 0x13: case 0x1b: {
        int c = (off & 0x08) ? 1 : 0;
        SerialChannel& s = ch_[c];
        if (!s.fifo.empty()) {
            s.lastRead = s.fifo.front();
            s.fifo.pop_front();
            ++s.rxBytes;
            host.irqChanged();
            host.reschedule();
        }
        return s.lastRead;
    }
    case 0x14: return 0;            // IPCR
    case 0x15: ++isrBit3Reads; return isr();
    case 0x18: return ch_[1].mr1;
    case 0x19: return status(1);
    case 0x1d: return 0;            // IP pins
    case 0x20: return ch_[0].mr2;
    case 0x21: return ch_[1].mr2;
    default: handled = false; return 0;
    }
}

void Serial::write8(uint32_t off, uint8_t v, bool& handled) {
    advance(host.now());
    handled = true;
    switch (off & 0x3f) {
    case 0x00: mcr = (mcr & 0x00ff) | uint16_t(v) << 8; break;
    case 0x01: mcr = (mcr & 0xff00) | v; break;
    case 0x04: ilr = v; host.irqChanged(); break;
    case 0x05: ivr = v; break;
    case 0x10: ch_[0].mr1 = v; break;
    case 0x11: ch_[0].csr = v; break;
    case 0x12: command(0, v); break;
    case 0x13: writeTx(0, v); break;
    case 0x14: acr = v; break;
    case 0x15: ier = v; host.irqChanged(); break;
    case 0x18: ch_[1].mr1 = v; break;
    case 0x19: ch_[1].csr = v; break;
    case 0x1a: command(1, v); break;
    case 0x1b: writeTx(1, v); break;
    case 0x1d: opcr = v; break;
    case 0x1e: op |= v; host.serialOutputPort(op, host.now()); break;
    case 0x1f: op &= ~v; host.serialOutputPort(op, host.now()); break;
    case 0x20: ch_[0].mr2 = v; break;
    case 0x21: ch_[1].mr2 = v; break;
    default: handled = false; break;
    }
}

void Serial::command(int c, uint8_t v) {
    SerialChannel& s = ch_[c];
    if ((v & 3) == 1) s.rxEn = true; else if ((v & 3) == 2) s.rxEn = false;
    if (((v >> 2) & 3) == 1) s.txEn = true; else if (((v >> 2) & 3) == 2) s.txEn = false;
    switch ((v >> 4) & 7) {
    case 2: s.rxEn = false; s.fifo.clear(); break;                       // reset receiver
    case 3: s.txEn = false; s.holdFull = false; s.shiftBusy = false;      // reset transmitter
            s.shiftDone = kNever; break;
    default: break;   // MR pointer, error status, break: nothing to model
    }
    host.irqChanged();
    host.reschedule();
}

void Serial::writeTx(int c, uint8_t v) {
    SerialChannel& s = ch_[c];
    if (!s.txEn) { ++s.txWhileDisabled; return; }
    if (!s.shiftBusy) {
        s.shiftByte = v; s.shiftBusy = true; s.shiftDone = host.now() + s.byteCycles;
        host.reschedule();
    } else {
        if (s.holdFull) ++s.holdOverwrites;
        s.holdFull = true; s.hold = v;
    }
    host.irqChanged();
}

uint64_t Serial::rxNext(int c) const {
    const SerialChannel& s = ch_[c];
    if (s.pending.empty() || s.fifo.size() >= 3 || !s.rxEn) return kNever;
    return std::max(s.pending.front().first, s.lastArrival + s.byteCycles);
}

uint64_t Serial::nextEvent() const {
    uint64_t t = kNever;
    for (int c = 0; c < 2; ++c) {
        if (ch_[c].shiftBusy) t = std::min(t, ch_[c].shiftDone);
        t = std::min(t, rxNext(c));
    }
    return t;
}

void Serial::advance(uint64_t now) {
    bool changed = false;
    for (int c = 0; c < 2; ++c) {
        SerialChannel& s = ch_[c];
        while (s.shiftBusy && now >= s.shiftDone) {
            uint64_t when = s.shiftDone;
            uint8_t b = s.shiftByte;
            if (s.holdFull) { s.shiftByte = s.hold; s.holdFull = false; s.shiftDone += s.byteCycles; }
            else { s.shiftBusy = false; s.shiftDone = kNever; }
            ++s.txBytes;
            changed = true;
            host.serialTx(c, b, when);
        }
        for (;;) {
            uint64_t t = rxNext(c);
            if (t == kNever || t > now) break;
            s.fifo.push_back(s.pending.front().second);
            s.pending.pop_front();
            s.lastArrival = t;
            changed = true;
        }
    }
    if (changed) host.irqChanged();
}

void Serial::queueRx(int c, const std::vector<uint8_t>& bytes, uint64_t earliest) {
    for (uint8_t b : bytes) ch_[c].pending.emplace_back(earliest, b);
    host.reschedule();
}

// ---------------------------------------------------------------- DMA channel
// Offsets relative to 0x780: MCR 0, INTR 4, CCR 8, CSR A, FCR B, SAR C, DAR 10, BTC 14.
// CCR 0x0D4D = SAPI | DAPI | byte source | byte destination | internal request | STR.

uint8_t Dma::read8(uint32_t off) {
    switch (off & 0x3f) {
    case 0x00: case 0x01: return getByte(mcr, off);
    case 0x04: case 0x05: return getByte(intr, off);
    case 0x08: case 0x09: return getByte(ccr, off);
    case 0x0a: return csr;
    case 0x0b: return fcr;
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: return getByte32(sar, off);
    case 0x10: case 0x11: case 0x12: case 0x13: return getByte32(dar, off);
    case 0x14: case 0x15: case 0x16: case 0x17: return getByte32(btc, off);
    default: return 0;
    }
}

void Dma::write8(uint32_t off, uint8_t v) {
    switch (off & 0x3f) {
    case 0x00: case 0x01: setByte(mcr, off, v); break;
    case 0x04: case 0x05: setByte(intr, off, v); break;
    case 0x08: setByte(ccr, off, v); break;
    case 0x09: setByte(ccr, off, v); if (ccr & 1) start(); break;
    case 0x0a: csr &= ~(v & 0x7c); if (!(csr & 0x7c)) csr &= ~0x80; break;
    case 0x0b: fcr = v; break;
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: setByte32(sar, off, v); break;
    case 0x10: case 0x11: case 0x12: case 0x13: setByte32(dar, off, v); break;
    case 0x14: case 0x15: case 0x16: case 0x17: setByte32(btc, off, v); break;
    default: break;
    }
}

void Dma::start() {
    // Internal request at maximum rate: the block completes immediately.
    // Only the byte/byte configuration the OS uses is modelled exactly; other
    // sizes are still copied bytewise (logged by the machine as unexpected).
    const bool sapi = ccr & 0x0800, dapi = ccr & 0x0400;
    uint32_t n = btc;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t b = host.dmaRead8(sar);
        host.dmaWrite8(dar, b);
        if (sapi) ++sar;
        if (dapi) ++dar;
    }
    bytes += n;
    ++blocks;
    btc = 0;
    csr |= 0xc0;          // IRQ status + DONE (no interrupt: INTR = 0, INTN clear)
    ccr &= ~1;
}
