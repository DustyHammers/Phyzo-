// Reader for single-instrument EPS-16 PLUS disk images (800 KB) as produced by the wave-set builds.
// Layout facts used here (our own analysis of the build disks; no manufacturer data):
//  - directory in blocks 3-4, 26-byte entries, type 3 = instrument, start block at +18 (BE32)
//  - FAT from block 5, 3-byte BE entries, 170 per block, 0/1 = end of chain
//  - wavesample 1 object pointer: packed at instrument byte 136 (word 61 + 2*n, after a 10-byte allocator header)
//  - wavesample fields relative to object+10 (build record section 1.9); 3-byte values stored x2 on alternate bytes
//  - PCM is BE16 and begins at the first word after the filler run that follows the header; the header's
//    20-sample buffer (object+10+0x118) must equal PCM[1..20], which validates the location.
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "io.hpp"

namespace eps {

struct Wavesample {
    std::string name;
    int rootKey = 0, fineTune = 0, loopMode = 0, keyLow = 0, keyHigh = 0, modSource = 0, modAmount = 0, modType = 0;
    uint32_t sampleStart = 0, sampleEnd = 0, loopStart = 0, loopEnd = 0;
    uint32_t pcmObjectOffset = 0;   // where PCM[0] sits relative to the wavesample object (diagnostic)
    std::vector<int16_t> pcm;       // PCM[0 .. sampleEnd)
};

inline Wavesample readImg(const std::string &path) {
    auto d = io::readFile(path);
    auto fail = [&](const std::string &m) -> void { throw std::runtime_error(path + ": " + m); };
    if (d.size() != 819200) fail("not an 800 KB EPS image");
    const uint8_t *dir = d.data() + 3 * 512;
    int found = 0; std::string name; uint32_t start = 0;
    for (int e = 0; e + 26 <= 1024; e += 26) if (dir[e + 1] == 3) {
        ++found; name.assign((const char *)dir + e + 2, 12); start = (dir[e + 18] << 24) | (dir[e + 19] << 16) | (dir[e + 20] << 8) | dir[e + 21]; }
    if (found != 1) fail("expected exactly one instrument file");
    std::vector<uint8_t> f; uint32_t b = start;
    for (int guard = 0; guard < 1600; ++guard) {
        if (b >= 1600) fail("FAT chain out of range");
        f.insert(f.end(), d.begin() + b * 512, d.begin() + (b + 1) * 512);
        size_t fo = 5 * 512 + (b / 170) * 512 + (b % 170) * 3;
        uint32_t nxt = (d[fo] << 16) | (d[fo + 1] << 8) | d[fo + 2];
        if (nxt == 0 || nxt == 1) break;
        b = nxt;
    }
    auto unpack = [&](size_t o) { return ((uint32_t)(f[o + 3] >> 4) << 20) | ((uint32_t)f[o] << 12) | ((uint32_t)f[o + 2] << 4); };
    auto u3 = [&](size_t o) { return (uint32_t)((f[o] << 16) | (f[o + 2] << 8) | f[o + 4]); };
    for (int n = 2; n < 128; ++n) if (unpack(10 + (61 + 2 * n) * 2)) fail("more than one wavesample");
    uint32_t obj = unpack(10 + (61 + 2) * 2);
    if (!obj || obj + 0x200 > f.size()) fail("no wavesample 1");
    size_t W = obj + 10;
    Wavesample ws;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    ws.name = name;
    ws.rootKey = f[W + 0xA0]; ws.fineTune = (int8_t)f[W + 0xAC]; ws.loopMode = f[W + 0xE4];
    ws.sampleStart = u3(W + 0xE8) / 2; ws.sampleEnd = u3(W + 0xEE) / 2; ws.loopStart = u3(W + 0xF6) / 2; ws.loopEnd = u3(W + 0xFE) / 2;
    ws.keyLow = f[W + 0x108]; ws.keyHigh = f[W + 0x10A]; ws.modSource = f[W + 0x10C]; ws.modAmount = f[W + 0x10E]; ws.modType = f[W + 0x112];
    uint8_t f0 = f[W + 0x116], f1 = f[W + 0x117];
    size_t p = W + 0x140;
    while (p + 1 < f.size() && f[p] == f0 && f[p + 1] == f1) p += 2;
    if (p + 42 > f.size() || std::memcmp(&f[p + 2], &f[W + 0x118], 40) != 0) fail("PCM location not confirmed by the 20-sample header buffer");
    if (p + 2ull * ws.sampleEnd > f.size()) fail("sample end beyond file");
    ws.pcmObjectOffset = (uint32_t)(p - obj);
    ws.pcm.resize(ws.sampleEnd);
    for (uint32_t i = 0; i < ws.sampleEnd; ++i) ws.pcm[i] = (int16_t)((f[p + 2 * i] << 8) | f[p + 2 * i + 1]);
    return ws;
}

}  // namespace eps
