// OS image loader. The image is user-supplied at run time and never embedded.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct OsImage {
    std::vector<uint8_t> bytes;   // file contents, loaded at kLoadAddress
    std::string path;
    std::string md5;              // hex
    uint32_t bodyEnd = 0;         // header long at +0 (end of image body)
    uint32_t entry = 0;           // header long at +4
    uint32_t storedSum = 0;       // trailer long at bodyEnd
    uint32_t computedSum = 0;     // 32-bit sum of big-endian words below bodyEnd
    bool sumOk = false;

    static constexpr uint32_t kLoadAddress = 0x4000;

    bool load(const std::string& p, std::string& err);
    uint8_t at(uint32_t addr) const {
        uint32_t o = addr - kLoadAddress;
        return o < bytes.size() ? bytes[o] : 0;
    }
};

std::string md5Hex(const uint8_t* data, size_t len);
