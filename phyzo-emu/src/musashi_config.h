/* Forced-include configuration for the Musashi core (C, included before m68kconf.h). */
#ifndef HARNESS_MUSASHI_CONFIG_H
#define HARNESS_MUSASHI_CONFIG_H

/* The 68340 supplies vectors itself (IVR of each module) except for external
   level 5, which is autovectored. The harness answers the IACK cycle. */
#define M68K_EMULATE_INT_ACK      2 /* M68K_OPT_SPECIFY_HANDLER */
#define M68K_INT_ACK_CALLBACK(A)  harness_int_ack(A)
int harness_int_ack(int level);

#ifdef HARNESS_TRACE
#define M68K_INSTRUCTION_HOOK         2 /* M68K_OPT_SPECIFY_HANDLER */
#define M68K_INSTRUCTION_CALLBACK(pc) harness_instr_hook(pc)
void harness_instr_hook(unsigned int pc);
#endif

#endif
