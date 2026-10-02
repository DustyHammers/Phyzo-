#include "os_image.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {
// Compact MD5 (RFC 1321), used only to confirm the user's baseline file.
struct Md5 {
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
        static const int S[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                                  4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = p[i*4] | (p[i*4+1] << 8) | (p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);       g = (7*i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B; B = B + rotl(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
};
}

std::string md5Hex(const uint8_t* data, size_t len) {
    Md5 m;
    size_t full = len / 64;
    for (size_t i = 0; i < full; ++i) m.block(data + i * 64);
    uint8_t tail[128] = {0};
    size_t rem = len - full * 64;
    std::memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    size_t tlen = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) tail[tlen - 8 + i] = (uint8_t)(bits >> (8 * i));
    m.block(tail);
    if (tlen == 128) m.block(tail + 64);
    char out[33];
    uint32_t w[4] = {m.a0, m.b0, m.c0, m.d0};
    for (int i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", (w[i / 4] >> (8 * (i % 4))) & 0xff);
    return std::string(out, 32);
}

bool OsImage::load(const std::string& p, std::string& err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { err = "cannot open OS image: " + p; return false; }
    bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    path = p;
    if (bytes.size() < 16) { err = "OS image too small"; return false; }
    if (bytes.size() > 0x100000 - kLoadAddress) { err = "OS image larger than flash"; return false; }
    md5 = md5Hex(bytes.data(), bytes.size());
    auto be32 = [&](size_t o) {
        return (uint32_t)bytes[o] << 24 | (uint32_t)bytes[o+1] << 16 | (uint32_t)bytes[o+2] << 8 | bytes[o+3];
    };
    bodyEnd = be32(0);
    entry = be32(4);
    uint32_t endOff = bodyEnd - kLoadAddress;
    if (endOff + 4 <= bytes.size() && (endOff & 1) == 0) {
        storedSum = be32(endOff);
        uint32_t s = 0;
        for (uint32_t o = 0; o < endOff; o += 2) s += (uint32_t)(bytes[o] << 8 | bytes[o+1]);
        computedSum = s;
        sumOk = (s == storedSum);
    }
    return true;
}
