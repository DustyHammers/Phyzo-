// os_disasm <image> <start> <count>: disassemble the user-supplied OS image (loaded at 0x4000).
#include <cstdio>
#include <cstdlib>
#include <string>
#include "os_image.h"
extern "C" {
#include "m68k.h"
}

static OsImage g_os;
static unsigned rd(unsigned a) { return g_os.at(a); }
extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return rd(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return rd(a) << 8 | rd(a + 1); }
unsigned int m68k_read_memory_32(unsigned int a) { return m68k_read_memory_16(a) << 16 | m68k_read_memory_16(a + 2); }
void m68k_write_memory_8(unsigned int, unsigned int) {}
void m68k_write_memory_16(unsigned int, unsigned int) {}
void m68k_write_memory_32(unsigned int, unsigned int) {}
unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
int harness_int_ack(int) { return -1; }
void harness_instr_hook(unsigned int) {}
}

int main(int argc, char** argv) {
    if (argc < 4) { std::puts("usage: os_disasm <image> <start> <count>"); return 2; }
    std::string err;
    if (!g_os.load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    unsigned a = std::strtoul(argv[2], nullptr, 0);
    int n = std::atoi(argv[3]);
    char buf[256];
    for (int i = 0; i < n; ++i) {
        unsigned len = m68k_disassemble(buf, a, M68K_CPU_TYPE_68020);
        std::printf("%06X: %s\n", a, buf);
        a += len;
    }
    return 0;
}
