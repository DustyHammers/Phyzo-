#include "voice_core.h"
#include <algorithm>

namespace {
// control register bits (ES5506 names)
constexpr uint32_t STOP0 = 0x0001, STOP1 = 0x0002, LEI = 0x0004, LPE = 0x0008, BLE = 0x0010, IRQE = 0x0020,
                   DIR = 0x0040, IRQ = 0x0080, LP3 = 0x0100, LP4 = 0x0200, RES_BIT10 = 0x0400;
constexpr uint32_t FMODE = LP3 | LP4 | RES_BIT10, F_RESLP = 0x0600, F_RESBP = 0x0700, F_BYPASS = 0x0500;
constexpr uint32_t STOPMASK = STOP0 | STOP1, LOOPMASK = LPE | BLE, GO = 0x80000;
constexpr int kFrac = 11;                         // address fraction bits
constexpr uint64_t kAddrMask = (1ull << (30 + kFrac)) - 1;   // 30-bit address (bank + 28-bit offset) + fraction
constexpr int kFilterBit = 12, kFilterShift = 4;  // as the ES5506: 16-bit K, top 12 bits used

inline int32_t sext18(uint32_t v) { return int32_t(v << 14) >> 14; }
inline int32_t lowpass(int32_t k, int32_t in, int32_t prev) { return ((k >> kFilterShift) * (in - prev)) / (1 << kFilterBit) + prev; }
inline int32_t highpass(int32_t k, int32_t in, int32_t prevOut, int32_t prevIn) {
    return in - prevIn + ((k >> kFilterShift) * prevOut) / (1 << (kFilterBit + 1)) + prevOut / 2;
}
// 9-bit shift-and-add coefficient: x * (1 +/- 2^-u) * 2^-s, s = bits 0-3, u = bits 4-7, bit 8 = subtract.
// (Decoding verified against all 51 entries of the OS resonance table; FILTER_REPORT.md.)
inline int32_t shiftAdd(uint32_t code, int32_t x) {
    int s = int(code & 15), u = int((code >> 4) & 15);
    int32_t a = x >> s, b = (s + u) > 31 ? (x < 0 ? -1 : 0) : (x >> (s + u));
    return (code & 0x100) ? a - b : a + b;
}
inline int32_t clampState(int64_t v, int32_t lim, uint64_t &n) {
    if (v > lim) { ++n; return lim; }
    if (v < -lim) { ++n; return -lim; }
    return int32_t(v);
}
}  // namespace

int16_t VoiceCore::sample(uint64_t addr) const {
    const auto &m = mem_[(addr >> 28) & 3];
    uint64_t off = addr & 0x0FFFFFFF;
    return off < m.size() ? m[off] : 0;
}

void VoiceCore::logResonance(uint32_t reg, uint32_t value, const Voice &vc) {
    if (resonanceLog)
        std::fprintf(resonanceLog, "%.3f,%06X,%u,%02X,%08X,%08X\n", timeMs, pc, pageSel & 63, reg, value, vc.cr);
}

void VoiceCore::writeReg(Voice &vc, uint32_t reg, uint32_t x) {
    vc.raw[reg >> 2] = x;
    switch (reg) {
    case 0x00:
        if (((vc.cr ^ x) & RES_BIT10) && resonanceLog) logResonance(reg, x, vc);
        // Bit 19 = start strobe (Derived): the OS's note start writes CR = shadow + 0x80002 and never clears
        // STOP1 itself for one-shots, so the chip starts the voice and consumes the strobe.
        if (x & GO) { x &= ~(GO | STOPMASK); ++starts; }
        vc.cr = x; break;
    case 0x04: vc.fc = x & 0x3FFFF; break;
    case 0x08: vc.lvol = int32_t(x & 0xFFFF) << 8; break;
    case 0x0C: vc.lvramp = uint16_t(x); break;
    case 0x10: vc.rvol = int32_t(x & 0xFFFF) << 8; break;
    case 0x14: vc.rvramp = uint16_t(x); break;
    case 0x18: vc.ecount = x & 0x1FF; vc.filtcount = 0; break;
    case 0x1C: vc.k2 = int32_t(x & 0xFFFF) << 8; break;
    case 0x20: vc.k2ramp = uint16_t(x); break;
    case 0x24: vc.k1 = int32_t(x & 0xFFFF) << 8; break;
    case 0x28: vc.k1ramp = uint16_t(x); break;
    case 0x2C: vc.start = uint64_t(x & 0x3FFFFFFF) << kFrac; break;
    case 0x30: vc.end = (uint64_t(x & 0x3FFFFFFF) << kFrac) | (vc.end & 0x7FF); break;
    case 0x34: vc.end = (vc.end & ~uint64_t(0x7FF)) | (uint64_t(x & 0xF) << 7); break;
    case 0x38: vc.accum = (uint64_t(x & 0x3FFFFFFF) << kFrac) | (vc.accum & 0x7FF); break;
    case 0x3C: vc.accum = (vc.accum & ~uint64_t(0x7FF)) | (x & 0x7FF); break;
    // filter state: the ES5506 pole states, and in the resonant modes the state-variable sections (same slots)
    case 0x40: vc.o4n1 = sext18(x); vc.bp2 = vc.o4n1 << 4; break;
    case 0x44: vc.o3n1 = sext18(x); vc.lp2 = vc.o3n1 << 4; break;
    case 0x48: logResonance(reg, x, vc); break;      // resonance codes A<<9 | B (raw[0x12]); ES5506 slot O3(n-2)
    case 0x4C: vc.o2n1 = sext18(x); vc.bp1 = vc.o2n1 << 4; break;
    case 0x50: vc.o2n2 = sext18(x); break;
    case 0x54: vc.o1n1 = sext18(x); vc.lp1 = vc.o1n1 << 4; break;
    case 0x58: case 0x5C: case 0x60: case 0x64: case 0x68: case 0x74: logResonance(reg, x, vc); break;
    default: break;
    }
}

uint32_t VoiceCore::readReg(const Voice &vc, uint32_t reg) const {
    switch (reg) {
    case 0x00: return vc.cr;
    case 0x04: return vc.fc;
    case 0x08: return uint32_t(vc.lvol >> 8) & 0xFFFF;
    case 0x0C: return vc.lvramp;
    case 0x10: return uint32_t(vc.rvol >> 8) & 0xFFFF;
    case 0x14: return vc.rvramp;
    case 0x18: return vc.ecount;
    case 0x1C: return uint32_t(vc.k2 >> 8) & 0xFFFF;
    case 0x20: return vc.k2ramp;
    case 0x24: return uint32_t(vc.k1 >> 8) & 0xFFFF;
    case 0x28: return vc.k1ramp;
    case 0x2C: return uint32_t(vc.start >> kFrac);
    case 0x30: return uint32_t(vc.end >> kFrac);
    case 0x34: return uint32_t(vc.end >> 7) & 0xF;
    case 0x38: return uint32_t(vc.accum >> kFrac);
    case 0x3C: return uint32_t(vc.accum & 0x7FF);
    case 0x40: return uint32_t(vc.o4n1) & 0x3FFFF;
    case 0x44: return uint32_t(vc.o3n1) & 0x3FFFF;
    case 0x4C: return uint32_t(vc.o2n1) & 0x3FFFF;
    case 0x50: return uint32_t(vc.o2n2) & 0x3FFFF;
    case 0x54: return uint32_t(vc.o1n1) & 0x3FFFF;
    default: return vc.raw[reg >> 2];
    }
}

uint32_t VoiceCore::read(uint32_t off, int size) {
    off &= 0x7f;
    uint32_t reg = off & 0x7c, full;
    ++reads; ++readCounts[reg];
    switch (reg) {
    case 0x6c: full = actv; break;
    case 0x70: full = mode; break;
    case 0x7c: full = pageSel; break;
    case 0x78:                                   // IRQV: reading acknowledges the latched voice
        full = irqv_;
        if (!(irqv_ & 0x80)) { ++irqAcks; irqv_ = 0x80; latchIrq(); }
        break;
    default: full = readReg(v[pageSel & 63], reg); break;
    }
    int shift = 8 * (4 - int(off & 3) - size);
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (8 * size)) - 1);
    return (full >> shift) & mask;
}

void VoiceCore::write(uint32_t off, uint32_t value, int size) {
    off &= 0x7f;
    uint32_t reg = off & 0x7c;
    bool global = reg == 0x6c || reg == 0x70 || reg == 0x78 || reg == 0x7c;
    Voice &vc = v[pageSel & 63];
    uint32_t cur = global ? (reg == 0x6c ? actv : reg == 0x70 ? mode : reg == 0x7c ? pageSel : 0) : readReg(vc, reg);
    if (!global && (reg == 0x48 || (reg >= 0x58 && reg <= 0x68) || reg == 0x74)) cur = vc.raw[reg >> 2];
    int shift = 8 * (4 - int(off & 3) - size);
    uint32_t mask = size == 4 ? 0xffffffffu : (((1u << (8 * size)) - 1) << shift);
    uint32_t x = (cur & ~mask) | ((value << shift) & mask);
    ++writes; ++writeCounts[reg];
    if (log) {
        if (global) std::fprintf(log, "%.3f,%06X,G,%02X,%d,%0*X\n", timeMs, pc, off, size, size * 2, value);
        else std::fprintf(log, "%.3f,%06X,%u,%02X,%d,%0*X\n", timeMs, pc, pageSel & 63, off, size, size * 2, value);
    }
    switch (reg) {
    case 0x6c: actv = x; return;
    case 0x70: mode = x; return;
    case 0x78: return;                           // IRQV is read-only
    case 0x7c: pageSel = x; if ((pageSel & 63) >= kVoices) ++badPage; return;
    default: writeReg(vc, reg, x); return;
    }
}

void VoiceCore::latchIrq() {
    if (!(irqv_ & 0x80)) return;
    for (int i = 0; i < kVoices; ++i)
        if (v[i].cr & IRQ) { irqv_ = uint32_t(i); v[i].cr &= ~IRQ; return; }
}

// Resonant modes (extension of the original chip, FILTER_REPORT.md): input gain A, then two Chamberlin state-variable
// sections in series, section 1 on K1 and section 2 on K2, both damped by B. 0x600 takes the low-pass outputs,
// 0x700 the band-pass outputs. Frequency coefficient k = K/65536 at the same 12-bit precision as the pole-pair
// modes (fitted: same k, damping scale 1.0). Internal precision: sample << 4 in 32-bit states.
void VoiceCore::resonant(Voice &vc, int32_t &s) {
    const uint32_t code = vc.raw[0x48 >> 2];
    const uint32_t a = (code >> 9) & 0x1FF, b = code & 0x1FF;
    const int64_t k1 = ((vc.k1 >> 8) & 0xFFFF) >> kFilterShift, k2 = ((vc.k2 >> 8) & 0xFFFF) >> kFilterShift;
    const bool bp = (vc.cr & FMODE) == F_RESBP;
    int32_t x = shiftAdd(a, s * 16);
    // section 1
    vc.lp1 = clampState(vc.lp1 + ((k1 * vc.bp1) >> kFilterBit), stateLimit, stateClamps);
    int64_t hp = int64_t(x) - vc.lp1 - shiftAdd(b, vc.bp1);
    vc.bp1 = clampState(vc.bp1 + ((k1 * hp) >> kFilterBit), stateLimit, stateClamps);
    x = bp ? vc.bp1 : vc.lp1;
    // section 2
    vc.lp2 = clampState(vc.lp2 + ((k2 * vc.bp2) >> kFilterBit), stateLimit, stateClamps);
    hp = int64_t(x) - vc.lp2 - shiftAdd(b, vc.bp2);
    vc.bp2 = clampState(vc.bp2 + ((k2 * hp) >> kFilterBit), stateLimit, stateClamps);
    s = (bp ? vc.bp2 : vc.lp2) >> 4;
    ++resonantSamples;
}

void VoiceCore::filter(Voice &vc, int32_t &s) {
    const uint32_t fm = vc.cr & FMODE;
    if (fm == F_RESLP || fm == F_RESBP) { resonant(vc, s); return; }
    if (fm & RES_BIT10) { ++bypassSamples; return; }   // 0x500 bypass (0x400 alone is never written by the OS)
    int32_t k1 = (vc.k1 >> 8) & 0xFFFF, k2 = (vc.k2 >> 8) & 0xFFFF;
    s = lowpass(k1, s, vc.o1n1); vc.o1n1 = s;                          // pole 1: LP, K1
    s = lowpass(k1, s, vc.o2n1); vc.o2n2 = vc.o2n1; vc.o2n1 = s;       // pole 2: LP, K1
    switch (vc.cr & (LP3 | LP4)) {
    case 0:            // 2LP/2HP
        s = highpass(k2, s, vc.o3n1, vc.o2n2); vc.o3n2 = vc.o3n1; vc.o3n1 = s;
        s = highpass(k2, s, vc.o4n1, vc.o3n2); vc.o4n1 = s; break;
    case LP3:          // 3LP/1HP
        s = lowpass(k1, s, vc.o3n1); vc.o3n2 = vc.o3n1; vc.o3n1 = s;
        s = highpass(k2, s, vc.o4n1, vc.o3n2); vc.o4n1 = s; break;
    case LP4:          // 2LP/2LP
        s = lowpass(k2, s, vc.o3n1); vc.o3n2 = vc.o3n1; vc.o3n1 = s;
        s = lowpass(k2, s, vc.o4n1); vc.o4n1 = s; break;
    default:           // 3LP/1LP
        s = lowpass(k1, s, vc.o3n1); vc.o3n2 = vc.o3n1; vc.o3n1 = s;
        s = lowpass(k2, s, vc.o4n1); vc.o4n1 = s; break;
    }
}

void VoiceCore::envelopes(Voice &vc) {
    --vc.ecount;
    bool slotTick = (vc.filtcount & 7) == 0;
    auto ramp = [&](int32_t &val, uint16_t r) {
        if (!r) return;
        if ((r & 1) && !slotTick) return;          // slow: every 8th sample (value pre-multiplied by 8)
        val += int32_t(int16_t(r & 0xFFFE));
        val = std::clamp(val, 0, 0xFFFFFF);
    };
    ramp(vc.lvol, vc.lvramp); ramp(vc.rvol, vc.rvramp); ramp(vc.k1, vc.k1ramp); ramp(vc.k2, vc.k2ramp);
    ++vc.filtcount;
}

void VoiceCore::checkEndForward(Voice &vc) {
    if (vc.accum > vc.end && !(vc.cr & LEI)) {
        if (vc.cr & IRQE) { vc.cr |= IRQ; ++irqsRaised; }
        switch (vc.cr & LOOPMASK) {
        case 0: vc.cr |= STOP0; ++voiceStops; break;
        case LPE: vc.accum = (vc.start + (vc.accum - vc.end)) & kAddrMask; break;
        case BLE: vc.accum = (vc.start + (vc.accum - vc.end)) & kAddrMask; vc.cr = (vc.cr & ~LOOPMASK) | LEI; break;
        default: vc.accum = (vc.end - (vc.accum - vc.end)) & kAddrMask; vc.cr ^= DIR; break;
        }
    }
}

void VoiceCore::checkEndReverse(Voice &vc) {
    if (vc.accum < vc.start && !(vc.cr & LEI)) {
        if (vc.cr & IRQE) { vc.cr |= IRQ; ++irqsRaised; }
        switch (vc.cr & LOOPMASK) {
        case 0: vc.cr |= STOP0; ++voiceStops; break;
        case LPE: vc.accum = (vc.end - (vc.start - vc.accum)) & kAddrMask; break;
        case BLE: vc.accum = (vc.end - (vc.start - vc.accum)) & kAddrMask; vc.cr = (vc.cr & ~LOOPMASK) | LEI; break;
        default: vc.accum = (vc.start + (vc.start - vc.accum)) & kAddrMask; vc.cr ^= DIR; break;
        }
    }
}

void VoiceCore::tick(int32_t &left, int32_t &right) {
    if (!tablesReady_) {   // 4-bit exponent, 8-bit mantissa log volume (ES5506)
        for (int i = 0; i < 4096; ++i) volTable_[i] = (((i & 0xFF) | 0x100) << 7) >> (16 - (i >> 8));
        tablesReady_ = true;
    }
    int32_t l = 0, r = 0;
    for (auto& c : chan) c[0] = c[1] = 0;
    int n = std::min<int>(kVoices, int(actv & 0x3F) + 1);
    for (int i = 0; i < n; ++i) {
        Voice &vc = v[i];
        if (!(vc.cr & STOPMASK)) {
            int32_t s = 0;
            if (processAudio) {
                uint64_t a = vc.accum >> kFrac; uint32_t f = uint32_t(vc.accum & 0x7FF);
                int32_t s1 = sample(a), s2 = sample(a + 1);
                s = (s1 * int32_t(2048 - f) + s2 * int32_t(f)) >> kFrac;
            }
            if (vc.cr & DIR) vc.accum = (vc.accum - vc.fc) & kAddrMask; else vc.accum = (vc.accum + vc.fc) & kAddrMask;
            if (processAudio) filter(vc, s);
            if (vc.ecount) envelopes(vc);
            if (processAudio) {
                int32_t vl = int32_t((int64_t(s) * volTable_[(vc.lvol >> 12) & 0xFFF]) >> 11);
                int32_t vr = int32_t((int64_t(s) * volTable_[(vc.rvol >> 12) & 0xFFF]) >> 11);
                l += vl; r += vr;
                uint32_t ch = (vc.cr >> 12) & 0xF;          // output channel (native: CR bits 12-15)
                if (ch < uint32_t(kChannels)) { chan[ch][0] += vl; chan[ch][1] += vr; } else ++badChannel;
            }
            if (vc.cr & DIR) checkEndReverse(vc); else checkEndForward(vc);
        } else if (vc.ecount) envelopes(vc);
    }
    latchIrq();
    ++samples;
    // Unclamped sum. The ES5506 serial output is 20 bits (MAME clamps there); whether the original hardware saturates at
    // that point before the ESP2 is Open, so the clamp is left to the consumer and over-range is counted.
    const int32_t lim = 1 << 19;
    if (l >= lim || l < -lim || r >= lim || r < -lim) ++overRange;
    left = l; right = r;
}

// ---------------------------------------------------------------- state (plugin projects)
#include <type_traits>
#include "state_io.h"

void VoiceCore::save(StateWriter& w) const {
    static_assert(std::is_trivially_copyable<Voice>::value, "voice state is copied as plain data");
    w.put(uint32_t(sizeof(Voice))); w.put(uint32_t(v.size()));
    w.raw(v.data(), sizeof(Voice) * v.size());
    w.put(chan); w.put(irqv_); w.put(actv); w.put(mode); w.put(pageSel); w.put(stateLimit); w.put(processAudio);
}
void VoiceCore::load(StateReader& r) {
    if (r.get<uint32_t>() != sizeof(Voice) || r.get<uint32_t>() != v.size()) { r.fail(); return; }
    r.raw(v.data(), sizeof(Voice) * v.size());
    r.get(chan); r.get(irqv_); r.get(actv); r.get(mode); r.get(pageSel); r.get(stateLimit); r.get(processAudio);
}
