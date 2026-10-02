// C callbacks required by Musashi, forwarded to the active Machine.
#include "machine.h"

extern "C" {
unsigned int m68k_read_memory_8(unsigned int a)  { return g_machine->read(a, 1); }
unsigned int m68k_read_memory_16(unsigned int a) { return g_machine->read(a, 2); }
unsigned int m68k_read_memory_32(unsigned int a) { return g_machine->read(a, 4); }
void m68k_write_memory_8(unsigned int a, unsigned int v)  { g_machine->write(a, v & 0xff, 1); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { g_machine->write(a, v & 0xffff, 2); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { g_machine->write(a, v, 4); }
unsigned int m68k_read_disassembler_8(unsigned int a)  { return g_machine->peek(a, 1); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return g_machine->peek(a, 2); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return g_machine->peek(a, 4); }
int harness_int_ack(int level) { return g_machine->intAck(level); }
void harness_instr_hook(unsigned int pc) { g_machine->instrHook(pc); }
}
