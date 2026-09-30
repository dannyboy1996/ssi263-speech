/* pc86.h -- a bare PC for a host that stands in for DOS: MAME's 8086 (../cpu/i86_mame.cpp, through cpu.h) with 1 MB
 * of flat memory, and the host's I/O and software interrupts handed to callbacks.  Built as pc86.dll / libpc86.so for
 * src/hosts/pc86.py, which gives it the shape of the Unicorn calls src/hosts/accent.py makes (the Accent-mini's
 * SPKEMS.DVC; SSI263_ACCENT_CORE=mame).  MIT.
 *
 * Memory is the host's to read and write directly (pc86_mem), between runs and from callbacks.  A run executes whole
 * cpu.h steps (a prefixed instruction is one; a REP string instruction one iteration) and stops before a step whose
 * start is `until`, after `count` steps, or after the step in which a callback called pc86_stop -- the three ways
 * Unicorn's uc_emu_start(begin, until, 0, count) stops.
 */
#ifndef SSI263_PC86_H
#define SSI263_PC86_H

#include <stdint.h>
#include "cpu.h"

#ifdef _WIN32
#define PC86_API __declspec(dllexport)
#else
#define PC86_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pc86 pc86;
typedef int (*pc86_in_fn)(void *user, int port);                  /* the byte read; the core masks it to 8 bits */
typedef void (*pc86_out_fn)(void *user, int port, int value);
typedef int (*pc86_int_fn)(void *user, int vector, int kind);     /* cpu_bus.intercept; nonzero = serviced */

#define PC86_MEM_SIZE 0x100000
#define PC86_NO_UNTIL 0xFFFFFFFFu

PC86_API pc86 *pc86_create(void);
PC86_API void pc86_destroy(pc86 *p);
PC86_API uint8_t *pc86_mem(pc86 *p);
PC86_API void pc86_set_hooks(pc86 *p, pc86_in_fn in, pc86_out_fn out, pc86_int_fn intr, void *user);
PC86_API uint64_t pc86_run(pc86 *p, uint64_t count, uint32_t until);   /* returns the steps run */
PC86_API void pc86_stop(pc86 *p);
PC86_API void pc86_regs_get(pc86 *p, i86_regs *out);
PC86_API void pc86_regs_set(pc86 *p, const i86_regs *in);
PC86_API void pc86_set_irq(pc86 *p, int line, int asserted);
PC86_API void pc86_set_vector(pc86 *p, int vector);                   /* the byte irq_ack answers for INTR */
PC86_API uint64_t pc86_cycles(pc86 *p);
PC86_API uint64_t pc86_steps(pc86 *p);
PC86_API uint64_t pc86_aliased(pc86 *p, uint32_t *last_addr, uint8_t *last_op);

#ifdef __cplusplus
}
#endif
#endif
