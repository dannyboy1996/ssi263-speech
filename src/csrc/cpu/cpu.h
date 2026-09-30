/* cpu.h -- the contract every CPU core in libssi263speech implements (DRAFT for review: Astra, Reply 73/76).
 *
 * One small C interface per core, the same shape for each: the Z180 (the Braille Lite), the 8085 (the Accent SA)
 * and, later, an x86 real-mode core (Speak-Out, Accent-mini).  A board drives a core through it and never reaches
 * into a core's internals; a core never knows which board it is in.  CONTRACT.md defines the timing: what a step
 * is, when interrupts are sampled, what HALT, SLP and EI do, and what cycle count a callback sees.
 *
 * Version 2 (after Astra, Reply 78): a corrected path (*_step, *_run) and, for the Z180 only, a legacy compatibility
 * path (z180_run_legacy) that reproduces today's z180emu slice with its exceptions, so the current goldens hold
 * while the board moves onto this interface.  The Z180 for new work comes from MAME's BSD-3 core (Tomi's decision).
 * Nothing implements this yet.
 */
#ifndef SSI263_CPU_H
#define SSI263_CPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a core sees of its board.  *_create COPIES this struct; ctx must outlive the core.  Every callback gets ctx,
   the board's own instance: no globals anywhere, so two boards (two units) run side by side in one process, and on
   different threads.  Required: read, write, in, out.  Optional (NULL): fetch (-> read), irq_ack (-> FFh), serial_*,
   boundary.  A callback may call *_cycles, *_steps, *_pc, *_regs_get and *_set_irq; never *_step, *_run, *_reset or
   *_destroy.  CONTRACT.md 7 says what each callback sees, phase by phase. */
typedef struct cpu_bus {
    void *ctx;
    uint8_t (*read)(void *ctx, uint32_t addr);                /* memory: the PHYSICAL address (after any MMU) */
    uint8_t (*fetch)(void *ctx, uint32_t addr);               /* an opcode fetch; NULL: read */
    void (*write)(void *ctx, uint32_t addr, uint8_t value);
    uint8_t (*in)(void *ctx, uint16_t port);                  /* EXTERNAL I/O only: on-chip registers stay inside */
    void (*out)(void *ctx, uint16_t port, uint8_t value);
    /* the data bus during an interrupt acknowledge: byte n of the vector or, for the 8085's INTR, of the injected
       instruction (opcode, then operands); -1 = FFh */
    int (*irq_ack)(void *ctx, int line, int n);
    int (*serial_rx)(void *ctx, int channel);                 /* on-chip serial in: the next byte on the line, -1 = none */
    void (*serial_tx)(void *ctx, int channel, uint8_t byte);  /* on-chip serial out: a byte leaves the chip */
    int (*serial_pin)(void *ctx, int pin);                    /* input pins a core samples (Z180 /DCD0 /CTS; 8085 SID) */
    void (*serial_out_pin)(void *ctx, int pin, int level);    /* output pins (8085 SOD) */
    /* Phase D of every step (CONTRACT.md 1): after interrupt acceptance and its charge, after *_steps increments and
       the on-chip serial port has caught up; before the instruction.  The board's per-instruction work (scheduled
       keys, A/R readiness) happens here, so what it raises is sampled at the NEXT step's acceptance.  NULL: none. */
    void (*boundary)(void *ctx, uint32_t pc);
    /* x86 only: the host's software-interrupt seam (what Unicorn's UC_HOOK_INTR was; CONTRACT.md 4).  Called at E,
       from inside the instruction, for an INT n, INT 3, INTO taken (kind I86_INT_SOFTWARE) or a divide error
       (I86_INT_EXCEPTION), before anything is pushed; CS:IP are already past the instruction.  Nonzero: the host
       has serviced it -- nothing is pushed, the instruction ends there, and the registers are what the host left
       (it may call *_regs_set here); 0: the core vectors through the table as the CPU does.  Hardware interrupts
       and the single-step trap are never offered.  NULL: none. */
    int (*intercept)(void *ctx, int vector, int kind);
} cpu_bus;

/* ---- Z180 (Zilog Z80180 / Hitachi HD64180 family) ---------------------------------------------------------------- */
typedef struct z180 z180;

enum { Z180_INT0, Z180_INT1, Z180_INT2, Z180_NMI };            /* external lines; on-chip sources are internal */
enum { Z180_PIN_DCD0, Z180_PIN_CTS0 };                         /* bus->serial_pin: the ASCI's input pins */
#define Z180_DMA_CHUNK 16                                      /* burst DMA bytes per step (corrected path; model) */

typedef struct {                                               /* for lockstep tests: the programmer-visible state */
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, sp, pc;
    uint8_t i, r, im, iff1, iff2, halted, sleeping;
} z180_regs;

z180 *z180_create(const cpu_bus *bus, double clock_hz);
void z180_destroy(z180 *c);
void z180_reset(z180 *c);
int z180_step(z180 *c);                            /* one step (CONTRACT.md 1); returns its T-states */
uint64_t z180_run(z180 *c, uint64_t budget);       /* whole steps until >= budget T-states; returns those run */
/* Today's cpu_execute_z180(budget), exactly, exceptions included (CONTRACT.md 3): NMI only at slice entry, a burst
   DMA chunk taking the rest of the budget, SLP ending the slice.  NOT equivalent to repeated steps.  For the current
   goldens only; retired with them. */
uint64_t z180_run_legacy(z180 *c, uint64_t budget);
void z180_set_irq(z180 *c, int line, int asserted);
uint64_t z180_cycles(const z180 *c);               /* T-states since reset (CONTRACT.md 5) */
uint64_t z180_steps(const z180 *c);                /* steps since reset, HALT and SLP slots included */
uint32_t z180_pc(const z180 *c);                   /* the SAVED instruction-start PC (regs_get: architectural) */
void z180_regs_get(const z180 *c, z180_regs *out);

typedef struct {                                               /* an ASCI channel's control registers, as written */
    uint8_t cntla, cntlb, stat, asext;
    uint16_t astc;
} z180_asci_regs;
/* An ASCI channel's (0 or 1) control registers, read without side effects, as the core holds them (CONTRACT.md 9:
   the pin bits differ between cores).  A board whose host carries the serial line to a real port programs that
   port from them (the line format the firmware chose). */
void z180_asci_get(const z180 *c, int channel, z180_asci_regs *out);

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
uint32_t i8085_pc(const i8085 *c);                 /* the saved instruction-start PC; no side effects (no RIM) */
void i8085_regs_get(const i8085 *c, i8085_regs *out);

/* ---- V40 (NEC uPD70208: the Speak-Out) -------------------------------------------------------------------------
   The V20 instruction set on an 8-bit bus, 20-bit addresses.  The V40's on-chip peripherals (ICU, TCU, SCU, DMA) are
   NOT in the core: a board models what its firmware uses and drives INT from its interrupt controller.  Memory and
   port addresses reach the bus as they are (ports 16 bits, memory 20 bits).  CONTRACT.md 4 and 12 say what a step is
   here: a REP string instruction is one step per iteration. */
typedef struct v40 v40;

enum { V40_INT, V40_NMI };           /* INT: a level (the ICU's output), consumed by its acceptance; NMI: an edge */

typedef struct {                     /* NEC's names (Intel's): the programmer-visible state */
    uint16_t aw, cw, dw, bw, sp, bp, ix, iy;    /* AX CX DX BX SP BP SI DI */
    uint16_t ds1, ps, ss, ds0;                   /* ES CS SS DS */
    uint16_t ip, psw;                            /* psw: the flags as PUSHF stores them */
    uint8_t halted;
    uint8_t fault;                   /* BRKEM entered the 8080 emulation mode, which this core does not model: it stops */
    uint32_t undefined;              /* undefined or unimplemented opcodes executed since reset (MAME's own handling) */
    uint32_t undefined_at;           /* the linear address of the last one's step */
} v40_regs;

v40 *v40_create(const cpu_bus *bus, double clock_hz);
void v40_destroy(v40 *c);
void v40_reset(v40 *c);                            /* PS = FFFFh, IP = 0: the first fetch is at FFFF0h */
int v40_step(v40 *c);                              /* one step (CONTRACT.md 1, 12); returns its clocks */
uint64_t v40_run(v40 *c, uint64_t budget);         /* whole steps until >= budget clocks; returns those run */
void v40_set_irq(v40 *c, int line, int asserted);
uint64_t v40_cycles(const v40 *c);                 /* clocks since reset */
uint64_t v40_steps(const v40 *c);
uint32_t v40_pc(const v40 *c);                     /* the saved step-start address, LINEAR (PS * 16 + IP, 20 bits) */
void v40_regs_get(const v40 *c, v40_regs *out);

/* ---- x86 real mode (Intel 8086/8088: the Accent-mini's PC, running Aicom's SPKEMS.DVC) ---------------------------- */
typedef struct i86 i86;

enum { I86_INTR, I86_NMI, I86_TEST };              /* INTR a level (vector from irq_ack byte 0); NMI an edge; TEST */
enum { I86_INT_SOFTWARE, I86_INT_EXCEPTION };      /* cpu_bus.intercept's kind */

typedef struct {
    uint16_t ax, cx, dx, bx, sp, bp, si, di;
    uint16_t es, cs, ss, ds, ip;
    uint16_t flags;                                /* as PUSHF stores them: the 8086's bits 12-15 read 1 */
    uint8_t halted;
} i86_regs;

i86 *i86_create(const cpu_bus *bus, double clock_hz);
void i86_destroy(i86 *c);
void i86_reset(i86 *c);                            /* CS:IP = FFFF:0000, flags cleared (IF off) */
int i86_step(i86 *c);
uint64_t i86_run(i86 *c, uint64_t budget);
void i86_set_irq(i86 *c, int line, int asserted);
uint64_t i86_cycles(const i86 *c);
uint64_t i86_steps(const i86 *c);
uint32_t i86_pc(const i86 *c);                     /* the saved instruction start, linear (CS * 16 + IP, 20 bits) */
uint32_t i86_next_pc(const i86 *c);                /* x86 only: CS * 16 + IP now, where the next step starts unless
                                                      an interrupt is accepted (a host's "run until" test) */
void i86_regs_get(const i86 *c, i86_regs *out);
/* x86 only: a host that stands in for DOS and the BIOS (the Accent-mini's) writes registers between steps and from
   bus->intercept -- far calls into the driver, a service's results, the carry flag, a saved machine.  `halted` is
   ignored; setting TF does not arm the single-step trap (only POPF and IRET do, as MAME). */
void i86_regs_set(i86 *c, const i86_regs *in);
/* How many opcodes met so far mean something else on the 80186 and later (0Fh, 60h-6Fh, C0h, C1h, C8h, C9h, F1h:
   the 8086 runs them as POP CS, Jcc, RET and LOCK aliases); the last one's linear address and byte.  A program that
   needs a later CPU shows here. */
uint64_t i86_aliased(const i86 *c, uint32_t *last_addr, uint8_t *last_op);

#ifdef __cplusplus
}
#endif
#endif
