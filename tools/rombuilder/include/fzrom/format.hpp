// Phyzo wave ROM container, format v1.0 - see docs/ROM_FORMAT_SPEC.md
// This header defines layout and engine rules only. It contains no wave data.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fzrom {

constexpr char kMagic[8] = {'F', 'Z', 'W', 'A', 'V', 'R', 'O', 'M'};
constexpr uint16_t kFormatMajor = 1, kFormatMinor = 3;  // 1.1: SWEEP_TABLE_NOTE_START; 1.2: ZONE_LOW_CONFIDENCE; 1.3: rom_patch
constexpr uint32_t kHeaderSize = 96, kDirEntrySize = 48, kZoneSize = 64;
constexpr uint32_t kCrcOffset = 80;  // byte offset of crc32 inside the header

enum LoopType : uint8_t { LOOP_ONESHOT = 0, LOOP_FORWARD = 1, LOOP_BIDIRECTIONAL = 2, LOOP_PLAIN = 3 };
enum SweepType : uint8_t { SWEEP_NONE = 0, SWEEP_TABLE = 1, SWEEP_STRETCH = 2, SWEEP_LOOPSTARTX = 3, SWEEP_TABLE_NOTE_START = 4 };
enum SourceKind : uint8_t { SRC_NONE = 0, SRC_EPS_IMG = 1, SRC_MR_ZONE = 2, SRC_WAV = 3, SRC_GENERATED = 4 };
enum WaveClass : uint16_t { CLASS_UNKNOWN = 0, CLASS_INTERNAL = 1, CLASS_EXPANSION = 2, CLASS_NATIVE_ONLY = 3 };
enum DirFlags : uint16_t { DIR_HAS_ZONES = 1, DIR_SOURCE_PENDING = 2, DIR_IN_FACTORY_PRESETS = 4 };
enum ZoneFlags : uint32_t { ZONE_KEY_OVERRIDE = 1, ZONE_VIRTUAL_ZEROS_BEFORE = 2, ZONE_PCM_SHARED = 4, ZONE_LOW_CONFIDENCE = 8 };

// ---- Pitch law (native): p = zone pitch + 256*note - 1741; increment = 2^(p/3072) at 44.1 kHz
constexpr int kPitchOffset = 1741, kUnitsPerSemitone = 256, kUnitsPerOctave = 3072;
constexpr double kOutputRate = 44100.0;
inline double increment(int32_t zonePitch, int note, double extraUnits = 0.0) {
    return std::pow(2.0, (zonePitch + kUnitsPerSemitone * note - kPitchOffset + extraUnits) / kUnitsPerOctave);
}

// ---- Start Index position (OS routine 0x1AC28), 8.8 fixed point:
// p = StartIndex*256 + (modValue*modAmount)/128 + (offset-64)*256, clamped to 0 .. 127.99 (0x7FFF).
// offset is the centred-at-64 byte (most likely the I knob). Integer division truncates toward zero.
inline int32_t indexPosition(int startIndex, int modValue, int modAmount, int offset) {
    int32_t p = startIndex * 256 + (modValue * modAmount) / 128 + (offset - 64) * 256;
    return p < 0 ? 0 : p > 0x7FFF ? 0x7FFF : p;
}
// ---- Table sweep (native loop mode 6/7 and EXP-3 transwaves): frame = index * N / 128 (integer), index = p >> 8
inline uint32_t tableFrame(uint32_t index, uint32_t frameCount) { return index * frameCount / 128u; }

// ---- Stretch sweep (native loop mode 5): shift s = index * smax(key) / 128 (integer);
// loop window = last L*2^(s/3072) samples ending at the cycle end (virtual zeros before the cycle),
// pitch raised by s units. smax = 0x1800 for key <= 66, else 0x5A00 - 256*key, min 0.
inline int stretchMax(int key) { if (key <= 66) return 0x1800; int v = 0x5A00 - 256 * key; return v < 0 ? 0 : v; }
inline int stretchShift(int index, int key) { return index * stretchMax(key) / 128; }
inline int stretchShiftP(int32_t p, int key) { return (int)((int64_t)p * stretchMax(key) / 32768); }  // from the 8.8 position

struct Header {
    uint16_t formatMajor = kFormatMajor, formatMinor = kFormatMinor, romMajor = 0, romMinor = 0, romPatch = 0;
    char buildDate[24] = {};
    uint32_t fileSize = 0, headerSize = kHeaderSize, waveCount = 0, dirOffset = 0, dirEntrySize = kDirEntrySize;
    uint32_t zoneCount = 0, zoneOffset = 0, zoneSize = kZoneSize, pcmOffset = 0, pcmSamples = 0, crc32 = 0, flags = 0;
};
struct DirEntry {
    uint16_t waveSelect = 0, number = 0, checksumSpec = 0, waveClass = 0, flags = 0, zoneCount = 0;
    uint32_t presetId = 0, firstZone = 0;
    char name[24] = {};
};
struct Zone {
    uint16_t waveSelect = 0;
    uint8_t zoneIndex = 0, lowKey = 0, highKey = 127, loopType = 0, sweepType = 0, sourceKind = 0;
    int32_t zonePitch = 0;
    uint32_t sourceRateMilliHz = 0, pcmOffset = 0, pcmLength = 0, playStart = 0, loopStart = 0, loopEnd = 0;
    uint32_t frameSize = 0, frameCount = 0, pcmCrc32 = 0, flags = 0;
    int16_t srcRoot = 0, srcTune = 0;
};

// ---- CRC-32 (IEEE 802.3, reflected, init/xorout 0xFFFFFFFF)
inline uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; } init = true; }
    crc = ~crc; for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8); return ~crc;
}
inline uint32_t crc32Samples(const int16_t *s, size_t n) {  // over little-endian bytes
    std::vector<uint8_t> b(n * 2);
    for (size_t i = 0; i < n; ++i) { uint16_t v = (uint16_t)s[i]; b[2 * i] = v & 0xFF; b[2 * i + 1] = v >> 8; }
    return crc32(b.data(), b.size());
}

// ---- Little-endian serialisation (explicit, host-endianness independent)
struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
    void s16(int16_t v) { u16((uint16_t)v); }
    void s32(int32_t v) { u32((uint32_t)v); }
    void bytes(const char *p, size_t n) { for (size_t i = 0; i < n; ++i) u8((uint8_t)p[i]); }
    void pad(size_t align) { while (b.size() % align) u8(0); }
};
struct Reader {
    const uint8_t *p; size_t n, o = 0; bool ok = true;
    Reader(const uint8_t *d, size_t len, size_t off = 0) : p(d), n(len), o(off) {}
    uint8_t u8() { if (o + 1 > n) { ok = false; return 0; } return p[o++]; }
    uint16_t u16() { uint16_t a = u8(); return a | (uint16_t)(u8() << 8); }
    uint32_t u32() { uint32_t a = u16(); return a | ((uint32_t)u16() << 16); }
    int16_t s16() { return (int16_t)u16(); }
    int32_t s32() { return (int32_t)u32(); }
    void bytes(char *d, size_t k) { for (size_t i = 0; i < k; ++i) d[i] = (char)u8(); }
};

inline void writeHeader(Writer &w, const Header &h) {
    w.bytes(kMagic, 8); w.u16(h.formatMajor); w.u16(h.formatMinor); w.u16(h.romMajor); w.u16(h.romMinor);
    w.bytes(h.buildDate, 24);
    for (uint32_t v : {h.fileSize, h.headerSize, h.waveCount, h.dirOffset, h.dirEntrySize, h.zoneCount, h.zoneOffset,
                       h.zoneSize, h.pcmOffset, h.pcmSamples, h.crc32, h.flags}) w.u32(v);
    w.u16(h.romPatch); w.u16(0); w.u32(0);  // 1.3: rom_patch at offset 88 (was reserved, 0 in older files)
}
inline void writeDir(Writer &w, const DirEntry &d) {
    w.u16(d.waveSelect); w.u16(d.number); w.u16(d.checksumSpec); w.u16(d.waveClass); w.u16(d.flags); w.u16(d.zoneCount);
    w.u32(d.presetId); w.u32(d.firstZone); w.bytes(d.name, 24); w.u32(0);
}
inline void writeZone(Writer &w, const Zone &z) {
    w.u16(z.waveSelect); w.u8(z.zoneIndex); w.u8(z.lowKey); w.u8(z.highKey); w.u8(z.loopType); w.u8(z.sweepType); w.u8(z.sourceKind);
    w.s32(z.zonePitch);
    for (uint32_t v : {z.sourceRateMilliHz, z.pcmOffset, z.pcmLength, z.playStart, z.loopStart, z.loopEnd, z.frameSize,
                       z.frameCount, z.pcmCrc32, z.flags}) w.u32(v);
    w.s16(z.srcRoot); w.s16(z.srcTune); w.u32(0); w.u32(0);
}

// ---- Loaded ROM (read-only view)
struct Rom {
    std::vector<uint8_t> file;
    Header h;
    std::vector<DirEntry> dir;
    std::vector<Zone> zones;
    std::vector<int16_t> pcm;
    std::string error;
    const DirEntry *find(uint16_t number, uint16_t presetChecksum) const {
        uint32_t id = ((uint32_t)number << 16) | presetChecksum;
        for (auto &d : dir) if (d.presetId == id) return &d;
        return nullptr;
    }
    const Zone *zoneFor(const DirEntry &d, int key) const {
        for (uint32_t i = 0; i < d.zoneCount; ++i) { const Zone &z = zones[d.firstZone + i]; if (key >= z.lowKey && key <= z.highKey) return &z; }
        return nullptr;
    }
};

inline bool loadRom(const std::vector<uint8_t> &bytes, Rom &rom) {
    rom.file = bytes; auto &f = rom.file;
    if (f.size() < kHeaderSize || std::memcmp(f.data(), kMagic, 8) != 0) { rom.error = "bad magic"; return false; }
    Reader r(f.data(), f.size(), 8);
    Header &h = rom.h;
    h.formatMajor = r.u16(); h.formatMinor = r.u16(); h.romMajor = r.u16(); h.romMinor = r.u16(); r.bytes(h.buildDate, 24);
    h.fileSize = r.u32(); h.headerSize = r.u32(); h.waveCount = r.u32(); h.dirOffset = r.u32(); h.dirEntrySize = r.u32();
    h.zoneCount = r.u32(); h.zoneOffset = r.u32(); h.zoneSize = r.u32(); h.pcmOffset = r.u32(); h.pcmSamples = r.u32();
    h.crc32 = r.u32(); h.flags = r.u32(); h.romPatch = r.u16();
    if (h.formatMajor != kFormatMajor) { rom.error = "unsupported format version"; return false; }
    if (h.fileSize != f.size()) { rom.error = "file size mismatch"; return false; }
    if ((uint64_t)h.pcmOffset + 2ull * h.pcmSamples > f.size()) { rom.error = "PCM block out of range"; return false; }
    std::vector<uint8_t> tmp(f); std::memset(tmp.data() + kCrcOffset, 0, 4);
    if (crc32(tmp.data(), tmp.size()) != h.crc32) { rom.error = "CRC32 mismatch"; return false; }
    for (uint32_t i = 0; i < h.waveCount; ++i) {
        Reader d(f.data(), f.size(), h.dirOffset + i * h.dirEntrySize); DirEntry e;
        e.waveSelect = d.u16(); e.number = d.u16(); e.checksumSpec = d.u16(); e.waveClass = d.u16(); e.flags = d.u16(); e.zoneCount = d.u16();
        e.presetId = d.u32(); e.firstZone = d.u32(); d.bytes(e.name, 24);
        if (!d.ok || e.firstZone + (uint64_t)e.zoneCount > h.zoneCount) { rom.error = "directory out of range"; return false; }
        rom.dir.push_back(e);
    }
    for (uint32_t i = 0; i < h.zoneCount; ++i) {
        Reader d(f.data(), f.size(), h.zoneOffset + i * h.zoneSize); Zone z;
        z.waveSelect = d.u16(); z.zoneIndex = d.u8(); z.lowKey = d.u8(); z.highKey = d.u8(); z.loopType = d.u8(); z.sweepType = d.u8(); z.sourceKind = d.u8();
        z.zonePitch = d.s32(); z.sourceRateMilliHz = d.u32(); z.pcmOffset = d.u32(); z.pcmLength = d.u32(); z.playStart = d.u32();
        z.loopStart = d.u32(); z.loopEnd = d.u32(); z.frameSize = d.u32(); z.frameCount = d.u32(); z.pcmCrc32 = d.u32(); z.flags = d.u32();
        z.srcRoot = d.s16(); z.srcTune = d.s16();
        if (!d.ok || z.pcmOffset + (uint64_t)z.pcmLength > h.pcmSamples) { rom.error = "zone PCM out of range"; return false; }
        rom.zones.push_back(z);
    }
    rom.pcm.resize(h.pcmSamples);
    for (uint32_t i = 0; i < h.pcmSamples; ++i) { size_t o = h.pcmOffset + 2 * (size_t)i; rom.pcm[i] = (int16_t)(f[o] | (f[o + 1] << 8)); }
    return true;
}

}  // namespace fzrom
