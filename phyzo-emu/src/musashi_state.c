/* Save and restore Musashi's CPU state (one global struct). Used to run several machines in one process
   (several plugin instances) and to store the CPU in the plugin's project state. Host-side pointers in the struct
   (cycle tables and callbacks) are never taken from a saved copy: the live values of this process are kept. */
#include <string.h>
#include "m68kcpu.h"

unsigned long phyzo_cpu_state_size(void) { return (unsigned long)sizeof(m68ki_cpu_core); }

void phyzo_cpu_save(void* dst) { memcpy(dst, &m68ki_cpu, sizeof(m68ki_cpu_core)); }

void phyzo_cpu_load(const void* src) {
    m68ki_cpu_core live = m68ki_cpu;
    memcpy(&m68ki_cpu, src, sizeof(m68ki_cpu_core));
    m68ki_cpu.cyc_instruction = live.cyc_instruction;
    m68ki_cpu.cyc_exception = live.cyc_exception;
    m68ki_cpu.int_ack_callback = live.int_ack_callback;
    m68ki_cpu.bkpt_ack_callback = live.bkpt_ack_callback;
    m68ki_cpu.reset_instr_callback = live.reset_instr_callback;
    m68ki_cpu.cmpild_instr_callback = live.cmpild_instr_callback;
    m68ki_cpu.rte_instr_callback = live.rte_instr_callback;
    m68ki_cpu.tas_instr_callback = live.tas_instr_callback;
    m68ki_cpu.illg_instr_callback = live.illg_instr_callback;
    m68ki_cpu.trap_instr_callback = live.trap_instr_callback;
    m68ki_cpu.pc_changed_callback = live.pc_changed_callback;
    m68ki_cpu.set_fc_callback = live.set_fc_callback;
    m68ki_cpu.instr_hook_callback = live.instr_hook_callback;
}

/* As phyzo_cpu_save, with the host pointers cleared (for state written to project files). */
void phyzo_cpu_save_portable(void* dst) {
    m68ki_cpu_core c = m68ki_cpu;
    c.cyc_instruction = 0; c.cyc_exception = 0;
    c.int_ack_callback = 0; c.bkpt_ack_callback = 0; c.reset_instr_callback = 0; c.cmpild_instr_callback = 0;
    c.rte_instr_callback = 0; c.tas_instr_callback = 0; c.illg_instr_callback = 0; c.trap_instr_callback = 0;
    c.pc_changed_callback = 0; c.set_fc_callback = 0; c.instr_hook_callback = 0;
    memcpy(dst, &c, sizeof c);
}
