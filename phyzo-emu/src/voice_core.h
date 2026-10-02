// Voice-chip core: an ES5506-derived chip with 48 voices and one flat page of 32 longword
// registers per voice (page select 0x7C). Written for this project; MAME's es5506 device (BSD-3-Clause,
// Aaron Giles) is the behavioural reference for interpolation, the 4-pole filter, loop handling and IRQs.
// Differences from the ES5506 that the OS image shows (HARDWARE_MAP.md, VOICE_CORE_REPORT.md):
//  - START/END/ACCUM hold integer sample addresses with the bank in bits 28-29; END and ACCUM fractions sit in
//    separate registers (0x34: 4 bits, 0x3C: 11 bits).
//  - Volume and filter ramps are signed 8.8 per-sample increments; bit 0 = slow (applied every 8th sample).
//  - CR bits 8-10 select the filter: 0x000-0x300 the four ES5506 pole-pair modes, 0x600 resonant 2LP/2LP,
//    0x700 resonant 2BP/2BP, 0x500 bypass (FILTER_REPORT.md). Register 0x48 holds two 9-bit shift-and-add codes
//    for the resonant modes: A (bits 9-17) = input gain, B (bits 0-8) = damping of both 2-pole sections.
//  - CR bits 12-15 carry the output (effect bus) channel code; ignored in the dry mix.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

class StateWriter;
class StateReader;

class VoiceCore {
public:
    static constexpr int kVoices = 48;
    static constexpr double kOutputRate = 44100.0;
    static constexpr int kChannels = 10;   // output channels selected by CR bits 12-15 (bus codes, table 0x1E25A)
    int64_t chan[kChannels][2] = {};       // per-channel L/R sums of the last sample (20-bit domain, unclamped)
    uint64_t badChannel = 0;               // voices with a channel code >= 10

    // bus interface (offset 0x00-0x7F inside the chip window)
    uint32_t read(uint32_t off, int size);
    void write(uint32_t off, uint32_t v, int size);

    // wave memory: four banks selected by address bits 28-29
    void setBank(int bank, std::vector<int16_t> samples) { if (bank >= 0 && bank < 4) mem_[bank] = std::move(samples); }

    // one output sample (dry stereo; 2^19 = full scale of the ES5506's 20-bit output, not clamped)
    void tick(int32_t &left, int32_t &right);
    bool irqLine() const { return !(irqv_ & 0x80); }

    // Chip state (registers, voices, filter states, IRQ latch). Wave memory is not included: it comes from the ROM.
    void save(StateWriter& w) const;
    void load(StateReader& r);

    // logs and statistics
    FILE *log = nullptr;         // every register write (t_ms,pc,page,reg,size,value)
    FILE *resonanceLog = nullptr;// writes to 0x48, 0x58-0x68, 0x74 and CR bit 10 changes
    double timeMs = 0; uint32_t pc = 0;
    // Saturation of the resonant sections' states (internal units: sample << 4). Default: the ES5506's 18-bit
    // filter-state register width, i.e. 4x a full-scale sample (Assumed; the original hardware's real limit is Open). It bounds
    // index 50 (damping 0), which otherwise grows without limit.
    int32_t stateLimit = 1 << 21;
    uint64_t resonantSamples = 0, bypassSamples = 0, stateClamps = 0;
    uint64_t writes = 0, reads = 0, badPage = 0, samples = 0, irqsRaised = 0, irqAcks = 0, voiceStops = 0, starts = 0, overRange = 0;
    std::map<uint32_t, uint64_t> readCounts, writeCounts;
    uint32_t actv = 0, mode = 0, pageSel = 0;
    bool processAudio = true;

    struct Voice {
        uint32_t cr = 0x0003, fc = 0;               // control (STOP1|STOP0 at reset), frequency count (11-bit fraction)
        int32_t lvol = 0, rvol = 0, k1 = 0, k2 = 0;  // 16.8 internal (register value << 8)
        uint16_t lvramp = 0, rvramp = 0, k1ramp = 0, k2ramp = 0;
        uint32_t ecount = 0, filtcount = 0;
        uint64_t start = 0, end = 0, accum = 0;      // address << 11 (bank in address bits 28-29)
        int32_t o1n1 = 0, o2n1 = 0, o2n2 = 0, o3n1 = 0, o3n2 = 0, o4n1 = 0;
        int32_t lp1 = 0, bp1 = 0, lp2 = 0, bp2 = 0;   // resonant modes: two state-variable sections (sample << 4)
        std::array<uint32_t, 32> raw{};              // last written value per register (unmodelled registers)
    };
    std::array<Voice, 64> v;

private:
    void writeReg(Voice &vc, uint32_t reg, uint32_t value);
    uint32_t readReg(const Voice &vc, uint32_t reg) const;
    int16_t sample(uint64_t addr) const;
    void filter(Voice &vc, int32_t &s);
    void resonant(Voice &vc, int32_t &s);
    void envelopes(Voice &vc);
    void checkEndForward(Voice &vc);
    void checkEndReverse(Voice &vc);
    void latchIrq();
    void logResonance(uint32_t reg, uint32_t value, const Voice &vc);
    uint32_t irqv_ = 0x80;
    std::array<std::vector<int16_t>, 4> mem_;
    std::array<int32_t, 4096> volTable_{};
    bool tablesReady_ = false;
};
