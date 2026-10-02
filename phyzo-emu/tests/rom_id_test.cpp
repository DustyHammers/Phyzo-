// ROM identification test: needs no ROM data. Synthetic files stand in for the two ROMs; the scanner is
// given their checksums, so the test checks the matching rules, not the real files.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "os_image.h"
#include "rom_id.h"

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void writeFile(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

static std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> b(n);
    for (size_t i = 0; i < n; ++i) b[i] = uint8_t(i * 31 + seed + (i >> 9));
    return b;
}

int main() {
    // MD5 against the RFC 1321 test suite.
    const char* rfc[][2] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
         "57edf4a22be3c955ac49da2e2107b67a"}};
    for (auto& v : rfc) CHECK(md5Hex(reinterpret_cast<const uint8_t*>(v[0]), std::strlen(v[0])) == v[1]);

    const fs::path dir = fs::temp_directory_path() / "phyzo_rom_id_test";
    fs::remove_all(dir);

    // Folder missing.
    romid::ScanResult none = romid::scanFolder((dir / "nope").u8string());
    CHECK(!none.folderExists && !none.complete());
    CHECK(none.problem().find("does not exist") != std::string::npos);

    fs::create_directories(dir);
    const auto os = pattern(300000, 1), wave = pattern(romid::kWaveImageSize, 2);
    romid::Expected ex;
    ex.osMd5 = md5Hex(os.data(), os.size());
    ex.waveMd5 = md5Hex(wave.data(), wave.size());

    // Empty folder: both missing, the message names both and the folder.
    romid::ScanResult r = romid::scanFolder(dir.u8string(), ex);
    CHECK(r.folderExists && !r.hasOs() && !r.hasWave());
    CHECK(r.problem().find("OS image") != std::string::npos);
    CHECK(r.problem().find("native wave image") != std::string::npos);
    CHECK(r.problem().find(dir.u8string()) != std::string::npos);

    // Arbitrary names are found by checksum; unrelated and hidden files are ignored.
    writeFile(dir / "my renamed os", os);
    writeFile(dir / "waves.dat", wave);
    writeFile(dir / "notes.txt", pattern(100, 3));
    writeFile(dir / ".DS_Store", os);
    r = romid::scanFolder(dir.u8string(), ex);
    CHECK(r.complete() && r.problem().empty());
    CHECK(fs::u8path(r.osPath).filename() == "my renamed os");
    CHECK(fs::u8path(r.wavePath).filename() == "waves.dat");

    // A file of the right size with the wrong checksum is reported by name.
    fs::remove(dir / "waves.dat");
    auto damaged = wave; damaged[12345] ^= 1;
    writeFile(dir / "damaged wave", damaged);
    r = romid::scanFolder(dir.u8string(), ex);
    CHECK(r.hasOs() && !r.hasWave());
    const std::string msg = r.problem();
    CHECK(msg.find("native wave image") != std::string::npos);
    CHECK(msg.find("the OS image") == std::string::npos);
    CHECK(msg.find("damaged wave") != std::string::npos);

    // The OS image's checksum on a 4 MB-sized file does not count as the wave image (and vice versa).
    romid::Expected swapped{ex.waveMd5, ex.osMd5};
    writeFile(dir / "waves.dat", wave);
    r = romid::scanFolder(dir.u8string(), swapped);
    CHECK(!r.hasOs() && !r.hasWave());

    // The real checksums are the ones the user gave.
    CHECK(std::string(romid::kOsImageMd5) == "8de06f48bfb0d847cacab05b763e22c5");
    CHECK(std::string(romid::kWaveImageMd5) == "42a974e31e48b05815d07554d0063171");

    fs::remove_all(dir);
    std::printf("rom_id_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
