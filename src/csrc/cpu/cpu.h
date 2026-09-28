/* cpu.h -- the contract every CPU core in libssi263speech implements (DRAFT for review: Astra, Reply 73/76).
 *
 * One small C interface per core, the same shape for each: the Z180 (the Braille Lite), the 8085 (the Accent SA)
 * and, later, an x86 real-mode core (Speak-Out, Accent-mini).  A board drives a core through it and never reaches
 * into a core's internals; a core never knows which board it is in.  CONTRACT.md defines the timing: what a step
 * is, when interrupts are sampled, what HALT, SLP and EI do, and what cycle count a callback sees.
 *
 * Nothing implements this yet.  Today's Braille Lite board calls z180emu directly (bl_board.c); the first
 * implementation will be an adapter around that core, which must reproduce the golden vectors bit for bit.
 */
#ifndef SSI263_CPU_H
#define SSI263_CPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a core sees of its board.  Every callback gets ctx, the board's own instance: no globals anywhere, so two
   boards (two units) run side by side in one process, and on different threads.  A callback may call the core's
   *_cycles, *_steps, *_pc and *_set_irq; it must not call *_step or *_run. */
typedef struct cpu_bus {
    void *ctx;
    uint8_t (*read)(void *ctx, uint32_t addr);                /* memory: the PHYSICAL address (after any MMU) */
    uint8_t (*fetch)(void *ctx, uint32_t addr);               /* an opcode fetch; NULL: read */
    void (*write)(void *ctx, uint32_t addr, uint8_t value);
    uint8_t (*in)(void *ctx, uint16_t port);                  /* EXTERNAL I/O only: on-chip registers stay inside */
    void (*out)(void *ctx, uint16_t port, uint8_t value);
    int (*irq_ack)(void *ctx, int line);                      /* the data bus during an interrupt acknowledge; -1 = FFh */
    int (*serial_rx)(void *ctx, int channel);                 /* on-chip serial in: the next byte on the line, -1 = none */
    void (*serial_tx)(void *ctx, int channel, uint8_t byte);  /* on-chip serial out: a byte leaves the chip */
    int (*serial_pin)(void *ctx, int pin);                    /* input pins a core samples (Z180 /DCD0 /CTS; 8085 SID) */
    void (*serial_out_pin)(void *ctx, int pin, int level);    /* output pins (8085 SOD) */
    /* Every step, AFTER interrupt acceptance and BEFORE the instruction (CONTRACT.md 1): the board's per-instruction
       work -- scheduled keys, A/R readiness -- happens here, so what it raises is sampled at the NEXT boundary.  NULL:
       none. */
    void (*boundary)(void *ctx, uint32_t pc);
} cpu_bus;

/* ---- Z180 (Zilog Z80180 / Hitachi HD64180 family) ---------------------------------------------------------------- */
typedef struct z180 z180;

enum { Z180_INT0, Z180_INT1, Z180_INT2, Z180_NMI };            /* external lines; on-chip sources are internal */

typedef struct {                                               /* for lockstep tests: the programmer-visible state */
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, sp, pc;
    uint8_t i, r, im, iff1, iff2, halted, sleeping;
} z180_regs;

z180 *z180_create(const cpu_bus *bus, double clock_hz);
void z180_destroy(z180 *c);
void z180_reset(z180 *c);
int z180_step(z180 *c);                            /* one step (CONTRACT.md 1); returns its T-states */
uint64_t z180_run(z180 *c, uint64_t budget);       /* whole steps until >= budget T-states; returns those run */
void z180_set_irq(z180 *c, int line, int asserted);
uint64_t z180_cycles(const z180 *c);               /* T-states since reset (CONTRACT.md 5) */
uint64_t z180_steps(const z180 *c);                /* steps since reset, HALT and SLP slots included */
uint32_t z180_pc(const z180 *c);
void z180_regs_get(const z180 *c, z180_regs *out);

/* ---- 8085 (Intel 8085A: the Accent SA) ------------------------------------------------------------------------- */
typedef struct i8085 i8085;

enum { I8085_INTR, I8085_RST55, I8085_RST65, I8085_RST75, I8085_TRAP };   /* RST7.5 and TRAP edge-sensitive */
enum { I8085_PIN_SID = 0, I8085_PIN_SOD = 0 };

typedef struct {
    uint16_t af, bc, de, hl, sp, pc;
    uint8_t im;                                    /* the RIM view: masks, pending, IE */
    uint8_t halted;
} i8085_regs;

i8085 *i8085_create(const cpu_bus *bus, double clock_hz);
void i8085_destroy(i8085 *c);
void i8085_reset(i8085 *c);
int i8085_step(i8085 *c);
uint64_t i8085_run(i8085 *c, uint64_t budget);
void i8085_set_irq(i8085 *c, int line, int asserted);
uint64_t i8085_cycles(const i8085 *c);
uint64_t i8085_steps(const i8085 *c);
uint32_t i8085_pc(const i8085 *c);
void i8085_regs_get(const i8085 *c, i8085_regs *out);

#ifdef __cplusplus
}
#endif
#endif
