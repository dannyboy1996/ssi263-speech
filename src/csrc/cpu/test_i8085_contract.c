/* test_i8085_contract.c -- CONTRACT.md's behaviour of a cpu.h 8085 core on the CORRECTED path (i8085_step / i8085_run).
 *
 * Each test is a few instructions in a flat 64K, driven through cpu.h only, and checks one clause.  T-states are
 * Intel's (MCS-80/85 Family User's Manual, Jan 1983): an acceptance of TRAP or RST 5.5-7.5 is the RST's 12 (section
 * 2.3.5, Figure 2-19; the RST listing: "12 (8085)"); an injected instruction takes its own (section 2.3.4: the INA
 * cycle is timed as the opcode fetch); a HALT slot is 4 (the contract's model).
 *   zero_budget     i8085_run(0) returns 0 and changes nothing (2)
 *   reset           reset zeroes the counts, drops a pending TRAP edge and the RST 7.5 latch, sets the masks,
 *                   and a held RST 5.5 level is sampled again (9)
 *   two_cores       two instances interleaved step by step behave as each alone (9)
 *   trap_ei         a TRAP raised at EI's boundary is taken at the next step, before the instruction after EI:
 *                   TRAP is not delayed by EI (5); its acceptance is 12 T
 *   ei_shadow       a pending RST 7.5 waits for the instruction after EI (5)
 *   halt_edge       an RST 7.5 raised at a HALT slot's boundary is accepted at the next step; a slot is 4 T (6)
 *   accept_pc       during that acceptance, i8085_pc() and the pushed address are the address after the HLT (7)
 *   halt_masked     with IE off, a HALT stays halted through an RST 7.5 edge, and a TRAP wakes it (4, 6)
 *   trap_cancel     a TRAP edge whose line drops before acceptance is not taken (4: edge AND level)
 *   trap_edge       a TRAP held high is taken once; a new edge is taken again (4)
 *   trap_priority   TRAP beats a pending RST 7.5, whatever IE and the masks say (4)
 *   rim_after_trap  the first RIM after TRAP reports the IE before it, the next the current one; i8085_regs_get
 *                   does not consume that (4, 7)
 *   priority        pending 7.5, 6.5, 5.5 and INTR are taken in that order (4)
 *   rst75_latch     an RST 7.5 edge while masked is latched (RIM bit 6) and not taken (4)
 *   sim_reveal      unmasked by SIM, it is taken at the next step's acceptance, not inside SIM's step (1, 4)
 *   sim_r75         SIM's R7.5 bit clears the latch: never taken (4)
 *   rst_levels      RST 5.5/6.5 are levels: masked or with IE off not taken, taken once enabled, not after the
 *                   line drops; RIM shows them live (4)
 *   intr_rst        INTR, IE on, every SIM mask set: the injected RST 5 (acknowledge byte 0) pushes the
 *                   interrupted PC; 12 T; no instruction after it in the step (4)
 *   intr_call       an injected CALL 1234h: operands from acknowledge bytes 1 and 2, 18 T (4)
 *   intr_nop        an injected NOP: nothing pushed, 4 T, IE cleared, the interrupted PC runs next (4)
 *   intr_jcc        an injected JZ not taken: 7 T, acknowledge bytes 0 and 1 (Intel: Jcond not taken is F R), the
 *                   PC held (INA inhibits the PC increment) (4); intr_jcc_taken: JNZ taken, 10 T, bytes 0 1 2
 *   intr_ccc        an injected CZ not taken: 9 T, bytes 0 and 1 (Ccond not taken is S R), nothing stacked;
 *                   intr_ccc_taken: CNZ taken, 18 T, bytes 0 1 2, the interrupted PC stacked
 *   jcc_reads       an ordinary JZ / CZ not taken reads its opcode and its second byte, not its third (7 / 9 T)
 *   intr_twice      each INTR acceptance asks the acknowledge from byte 0 again (4)
 *   intr_halt       INTR ends a HALT: the injected RST pushes the address after the HLT (4, 6)
 *   sid_sod         RIM reads SID into bit 7; SIM with SDE drives SOD, without it leaves SOD alone
 *
 *   build: gcc -c test_i8085_contract.c; g++ ... i8085_mame.cpp   (../blazie/build_board.py does it)
 * Each line: "ok"/"FAIL", the test's name, the detail; the last line "all passed" or "FAILED"; exit status 0/1.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

/* ---- a machine: 64K of RAM, a recorder at each boundary ------------------------------------------------------ */
typedef struct { int step, line, level; } action;

typedef struct {
    uint8_t mem[65536];
    int pos;                                   /* the assembler's cursor */
    uint32_t pcs[512];
    int n_pcs;
    i8085 *cpu;
    action acts[8];                            /* set a line at a step's boundary */
    int n_acts;
    int ack[4];                                /* the INTR acknowledge's bytes, by index */
    int ack_idx[8], n_ack;
    uint32_t wr_addr[16], wr_pc[16];           /* stack writes (7000h-7FFFh): address, i8085_pc() then */
    int n_wr;
    int outs[16], n_outs;                      /* ports written */
    int drop[256];                             /* OUT to this port lowers line drop[p] - 1 */
    int sid, sod[8], n_sod;
    int log_reads;                             /* record every read address while set */
    uint32_t rd_addr[16];
    int n_rd;
} machine;

static uint8_t rd(void *ctx, uint32_t a)
{
    machine *m = (machine *)ctx;
    if (m->log_reads && m->n_rd < 16)
        m->rd_addr[m->n_rd++] = a & 0xFFFF;
    return m->mem[a & 0xFFFF];
}
static void wr(void *ctx, uint32_t a, uint8_t v)
{
    machine *m = (machine *)ctx;
    m->mem[a & 0xFFFF] = v;
    if ((a & 0xFFFF) >= 0x7000 && (a & 0xFFFF) < 0x8000 && m->n_wr < 16) {
        m->wr_addr[m->n_wr] = a & 0xFFFF;
        m->wr_pc[m->n_wr++] = i8085_pc(m->cpu);
    }
}
static int ack(void *ctx, int line, int n)
{
    machine *m = (machine *)ctx;
    if (line != I8085_INTR)
        return -2;                             /* never asked for anything else */
    if (m->n_ack < 8)
        m->ack_idx[m->n_ack++] = n;
    return n < 4 ? m->ack[n] : -1;
}
static uint8_t in(void *ctx, uint16_t p) { (void)ctx; (void)p; return 0xFF; }
static void out(void *ctx, uint16_t p, uint8_t v)
{
    machine *m = (machine *)ctx;
    (void)v;
    if (m->n_outs < 16)
        m->outs[m->n_outs++] = p & 0xFF;
    if (m->drop[p & 0xFF])
        i8085_set_irq(m->cpu, m->drop[p & 0xFF] - 1, 0);
}
static int sid_pin(void *ctx, int pin) { return pin == I8085_PIN_SID ? ((machine *)ctx)->sid : 0; }
static void sod_pin(void *ctx, int pin, int level)
{
    machine *m = (machine *)ctx;
    if (pin == I8085_PIN_SOD && m->n_sod < 8)
        m->sod[m->n_sod++] = level;
}

static void boundary(void *ctx, uint32_t pc)
{
    machine *m = (machine *)ctx;
    int i;
    if (m->n_pcs < (int)(sizeof m->pcs / sizeof m->pcs[0]))
        m->pcs[m->n_pcs++] = pc;
    for (i = 0; i < m->n_acts; i++)
        if ((long)i8085_steps(m->cpu) == m->acts[i].step)
            i8085_set_irq(m->cpu, m->acts[i].line, m->acts[i].level);
}

static machine *new_machine(void)
{
    machine *m = (machine *)calloc(1, sizeof(machine));
    cpu_bus bus;
    memset(&bus, 0, sizeof bus);
    bus.ctx = m;
    bus.read = rd;
    bus.write = wr;
    bus.in = in;
    bus.out = out;
    bus.boundary = boundary;
    bus.irq_ack = ack;
    bus.serial_pin = sid_pin;
    bus.serial_out_pin = sod_pin;
    m->cpu = i8085_create(&bus, 3072000.0);
    return m;
}

static void free_machine(machine *m)
{
    i8085_destroy(m->cpu);
    free(m);
}

static void at(machine *m, int step, int line, int level)
{
    m->acts[m->n_acts].step = step;
    m->acts[m->n_acts].line = line;
    m->acts[m->n_acts++].level = level;
}

/* the assembler: bytes at the cursor */
static void org(machine *m, int a) { m->pos = a; }
static void db(machine *m, int n, ...)
{
    va_list ap;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++)
        m->mem[m->pos++ & 0xFFFF] = (uint8_t)va_arg(ap, int);
    va_end(ap);
}
static void lxi_sp(machine *m, int a) { db(m, 3, 0x31, a & 0xFF, a >> 8); }
static void sta(machine *m, int a) { db(m, 3, 0x32, a & 0xFF, a >> 8); }
static void mvi_a(machine *m, int v) { db(m, 2, 0x3E, v); }
static void sim_a(machine *m, int v) { mvi_a(m, v); db(m, 1, 0x30); }       /* MVI A,v; SIM */
enum { NOP = 0x00, HLT = 0x76, EI = 0xFB, DI = 0xF3, RIM = 0x20, SIM = 0x30, RET = 0xC9, OUT = 0xD3 };

static int first_step_at(const machine *m, uint32_t pc)
{
    int i;
    for (i = 0; i < m->n_pcs; i++)
        if (m->pcs[i] == pc)
            return i + 1;                       /* steps count from 1 */
    return -1;
}
static int count_at(const machine *m, uint32_t pc)
{
    int i, n = 0;
    for (i = 0; i < m->n_pcs; i++)
        n += m->pcs[i] == pc;
    return n;
}

static int failures;
static void report(const char *name, int ok, const char *detail)
{
    printf("%-4s %-14s %s\n", ok ? "ok" : "FAIL", name, detail);
    if (!ok)
        failures++;
}

/* ---- the tests ------------------------------------------------------------------------------------------------ */
static void t_zero_budget(void)
{
    machine *m = new_machine();
    char d[160];
    uint64_t done, c0 = i8085_cycles(m->cpu), s0 = i8085_steps(m->cpu);
    done = i8085_run(m->cpu, 0);
    sprintf(d, "run(0) = %llu, cycles %llu -> %llu, steps %llu -> %llu", (unsigned long long)done,
            (unsigned long long)c0, (unsigned long long)i8085_cycles(m->cpu), (unsigned long long)s0,
            (unsigned long long)i8085_steps(m->cpu));
    report("zero_budget", done == 0 && i8085_cycles(m->cpu) == c0 && i8085_steps(m->cpu) == s0 && m->n_pcs == 0, d);
    free_machine(m);
}

static void t_reset(void)
{
    machine *m = new_machine();
    i8085_regs r;
    char d[240];
    int i, zeroed;
    org(m, 0);
    lxi_sp(m, 0x8000);                          /* 0000 */
    sim_a(m, 0x08);                             /* 0003 MVI A,08h; 0005 SIM: all three unmasked */
    db(m, 1, EI);                               /* 0006 */
    for (i = 0; i < 8; i++)
        db(m, 1, NOP);                          /* 0007.. */
    org(m, 0x24); db(m, 1, HLT);
    org(m, 0x2C); db(m, 1, HLT);
    org(m, 0x3C); db(m, 1, HLT);
    for (i = 0; i < 2; i++)
        i8085_step(m->cpu);
    i8085_set_irq(m->cpu, I8085_TRAP, 1);       /* an edge, pending, and the line left high */
    i8085_set_irq(m->cpu, I8085_RST75, 1);      /* the 7.5 latch set */
    i8085_set_irq(m->cpu, I8085_RST75, 0);
    i8085_set_irq(m->cpu, I8085_RST55, 1);      /* a level, held over the reset */
    i8085_reset(m->cpu);
    zeroed = i8085_cycles(m->cpu) == 0 && i8085_steps(m->cpu) == 0 && i8085_pc(m->cpu) == 0;
    i8085_regs_get(m->cpu, &r);
    m->n_pcs = 0;
    for (i = 0; i < 10; i++)
        i8085_step(m->cpu);
    /* steps: 1 LXI, 2 MVI, 3 SIM, 4 EI, 5 NOP (the EI shadow), 6 the RST 5.5 vector */
    sprintf(d, "counts zeroed: %s; RIM view after reset %02X (want 17: the masks, the 5.5 level, no 7.5 latch, IE off); "
            "TRAP vector %s, 7.5 vector %s, the held 5.5 taken at step %d (want 6)", zeroed ? "yes" : "no", r.im,
            first_step_at(m, 0x24) < 0 ? "not entered" : "ENTERED",
            first_step_at(m, 0x3C) < 0 ? "not entered" : "ENTERED", first_step_at(m, 0x2C));
    report("reset", zeroed && r.im == 0x17 && first_step_at(m, 0x24) < 0 && first_step_at(m, 0x3C) < 0
                    && first_step_at(m, 0x2C) == 6, d);
    free_machine(m);
}

/* LXI SP; EI; NOPs; the TRAP vector's first instruction a HLT */
static void t_trap_ei(void)
{
    machine *m = new_machine();
    char d[200];
    int i, t[6];
    org(m, 0);
    lxi_sp(m, 0x8000);                          /* 0000 */
    db(m, 1, EI);                               /* 0003: step 2 */
    for (i = 0; i < 6; i++)
        db(m, 1, NOP);                          /* 0004.. */
    org(m, 0x24); db(m, 1, HLT);
    at(m, 2, I8085_TRAP, 1);                    /* raised at EI's boundary: EI then runs */
    for (i = 0; i < 6; i++)
        t[i] = i8085_step(m->cpu);
    sprintf(d, "TRAP raised at EI's boundary (step 2); the vector's first boundary at step %d (want 3, before the "
            "NOP at 0004); that step %d T (want 12 + the HLT's 5)", first_step_at(m, 0x24), t[2]);
    report("trap_ei", first_step_at(m, 0x24) == 3 && t[2] == 17 && first_step_at(m, 0x04) < 0, d);
    free_machine(m);
}

static void t_ei_shadow(void)
{
    machine *m = new_machine();
    char d[200];
    int i;
    org(m, 0);
    lxi_sp(m, 0x8000);                          /* 0000 */
    sim_a(m, 0x08);                             /* 0003, 0005 */
    db(m, 1, EI);                               /* 0006: step 4 */
    db(m, 4, NOP, NOP, NOP, NOP);               /* 0007: step 5 */
    org(m, 0x3C); db(m, 1, HLT);
    i8085_set_irq(m->cpu, I8085_RST75, 1);      /* latched from the start */
    for (i = 0; i < 8; i++)
        i8085_step(m->cpu);
    sprintf(d, "boundaries %04X %04X %04X %04X %04X %04X (want 0000 0003 0005 0006 0007 003C)", m->pcs[0], m->pcs[1],
            m->pcs[2], m->pcs[3], m->pcs[4], m->pcs[5]);
    report("ei_shadow", m->pcs[3] == 0x06 && m->pcs[4] == 0x07 && m->pcs[5] == 0x3C, d);
    free_machine(m);
}

/* LXI SP; unmask; EI; HLT at 0007; the RST 7.5 vector a HLT */
static machine *halt_program(void)
{
    machine *m = new_machine();
    org(m, 0);
    lxi_sp(m, 0x8000);
    sim_a(m, 0x08);
    db(m, 1, EI);
    db(m, 1, HLT);                              /* 0007: step 5 */
    org(m, 0x3C); db(m, 1, HLT);
    at(m, 8, I8085_RST75, 1);                   /* a HALT slot's boundary */
    return m;
}

static void t_halt_edge(void)
{
    machine *m = halt_program();
    char d[220];
    int i, t[12];
    for (i = 0; i < 12; i++)
        t[i] = i8085_step(m->cpu);
    sprintf(d, "HLT step %d T (want 5); boundary pc at step 8: %04X, that slot %d T (want 4); RST 7.5 raised there, "
            "vector's first boundary at step %d (want 9), that step %d T (want 12 + 5)", t[4], m->pcs[7], t[7],
            first_step_at(m, 0x3C), t[8]);
    report("halt_edge", t[4] == 5 && m->pcs[7] == 0x07 && t[5] == 4 && t[6] == 4 && t[7] == 4
                        && first_step_at(m, 0x3C) == 9 && t[8] == 17, d);
    free_machine(m);
}

static void t_accept_pc(void)
{
    machine *m = halt_program();
    char d[200];
    int i;
    for (i = 0; i < 10; i++)
        i8085_step(m->cpu);
    sprintf(d, "%d stack writes: %04X %04X with i8085_pc() %04X %04X (want 7FFF 7FFE, 0008 0008); stacked %02X%02X "
            "(want 0008)", m->n_wr, m->wr_addr[0], m->wr_addr[1], m->wr_pc[0], m->wr_pc[1], m->mem[0x7FFF],
            m->mem[0x7FFE]);
    report("accept_pc", m->n_wr == 2 && m->wr_addr[0] == 0x7FFF && m->wr_addr[1] == 0x7FFE && m->wr_pc[0] == 0x08
                        && m->wr_pc[1] == 0x08 && m->mem[0x7FFF] == 0x00 && m->mem[0x7FFE] == 0x08, d);
    free_machine(m);
}

static void t_halt_masked(void)
{
    machine *m = new_machine();
    i8085_regs r;
    char d[200];
    int i, still;
    org(m, 0);
    lxi_sp(m, 0x8000);
    sim_a(m, 0x08);
    db(m, 1, DI);
    db(m, 1, HLT);                              /* 0007: step 5 */
    org(m, 0x24); db(m, 1, HLT);
    org(m, 0x3C); db(m, 1, HLT);
    at(m, 7, I8085_RST75, 1);
    at(m, 12, I8085_TRAP, 1);
    for (i = 0; i < 11; i++)
        i8085_step(m->cpu);
    i8085_regs_get(m->cpu, &r);
    still = r.halted && r.pc == 0x07 && first_step_at(m, 0x3C) < 0;
    for (i = 0; i < 3; i++)
        i8085_step(m->cpu);
    sprintf(d, "IE off: after an RST 7.5 edge still halted at %04X: %s; a TRAP at step 12 enters 0024 at step %d "
            "(want 13)", r.pc, still ? "yes" : "no", first_step_at(m, 0x24));
    report("halt_masked", still && first_step_at(m, 0x24) == 13, d);
    free_machine(m);
}

/* NOPs from 0003; the TRAP vector: INX B; RET */
static machine *trap_program(void)
{
    machine *m = new_machine();
    int i;
    org(m, 0);
    lxi_sp(m, 0x8000);
    for (i = 0; i < 60; i++)
        db(m, 1, NOP);
    org(m, 0x24);
    db(m, 2, 0x03, RET);                        /* INX B; RET */
    return m;
}

static void t_trap_cancel(void)
{
    machine *m = trap_program();
    char d[160];
    int i;
    at(m, 4, I8085_TRAP, 1);
    at(m, 4, I8085_TRAP, 0);                    /* dropped at the same boundary, before any acceptance */
    for (i = 0; i < 12; i++)
        i8085_step(m->cpu);
    sprintf(d, "TRAP up and down at step 4's boundary: the vector %s", first_step_at(m, 0x24) < 0 ? "not entered" :
            "ENTERED");
    report("trap_cancel", first_step_at(m, 0x24) < 0, d);
    free_machine(m);
}

static void t_trap_edge(void)
{
    machine *m = trap_program();
    i8085_regs r;
    char d[200];
    int i, once;
    at(m, 3, I8085_TRAP, 1);                    /* held high from here */
    for (i = 0; i < 20; i++)
        i8085_step(m->cpu);
    once = count_at(m, 0x24);
    i8085_set_irq(m->cpu, I8085_TRAP, 0);
    i8085_set_irq(m->cpu, I8085_TRAP, 1);       /* a new edge */
    for (i = 0; i < 10; i++)
        i8085_step(m->cpu);
    i8085_regs_get(m->cpu, &r);
    sprintf(d, "held high: taken %d time(s) in 20 steps (want 1); after a new edge: %d, BC = %04X (want 2, 0002)", once,
            count_at(m, 0x24), r.bc);
    report("trap_edge", once == 1 && count_at(m, 0x24) == 2 && r.bc == 2, d);
    free_machine(m);
}

/* TRAP and RST 7.5 pending together, IE on and 7.5 unmasked; the TRAP handler: NOP, RIM, STA 9000h, RIM, STA 9001h */
static void t_trap_priority_rim(void)
{
    machine *m = new_machine();
    i8085_regs r1, r2;
    char d[260];
    int i, ok;
    org(m, 0);
    lxi_sp(m, 0x8000);
    sim_a(m, 0x08);
    db(m, 1, EI);                               /* 0006: step 4 */
    db(m, 4, NOP, NOP, NOP, NOP);
    org(m, 0x24);
    db(m, 2, NOP, RIM); sta(m, 0x9000); db(m, 1, RIM); sta(m, 0x9001); db(m, 1, HLT);
    org(m, 0x3C); db(m, 1, HLT);
    at(m, 5, I8085_RST75, 1);                   /* at the NOP after EI: both pending at step 6 */
    at(m, 5, I8085_TRAP, 1);
    for (i = 0; i < 6; i++)
        i8085_step(m->cpu);
    i8085_regs_get(m->cpu, &r1);                /* after the handler's NOP, before its RIM: twice, no side effect */
    i8085_regs_get(m->cpu, &r2);
    for (i = 0; i < 6; i++)
        i8085_step(m->cpu);
    ok = first_step_at(m, 0x24) == 6 && first_step_at(m, 0x3C) < 0;
    sprintf(d, "TRAP's vector at step %d (want 6), 7.5's %s", first_step_at(m, 0x24),
            first_step_at(m, 0x3C) < 0 ? "not entered (IE off after TRAP)" : "ENTERED");
    report("trap_priority", ok, d);
    sprintf(d, "first RIM %02X (want 48: IE before TRAP, 7.5 pending), second %02X (want 40: IE now off); regs_get's "
            "view %02X %02X (want 40 40)", m->mem[0x9000], m->mem[0x9001], r1.im, r2.im);
    report("rim_after_trap", m->mem[0x9000] == 0x48 && m->mem[0x9001] == 0x40 && r1.im == 0x40 && r2.im == 0x40, d);
    free_machine(m);
}

/* All four pending at once; each handler says who it is with an OUT (which also drops its level line), EI, RET */
static void t_priority(void)
{
    machine *m = new_machine();
    char d[200];
    int i;
    org(m, 0);
    lxi_sp(m, 0x8000);
    sim_a(m, 0x08);
    db(m, 1, EI);
    for (i = 0; i < 40; i++)
        db(m, 1, NOP);
    org(m, 0x2C); db(m, 5, OUT, 0x55, EI, RET, 0);
    org(m, 0x34); db(m, 5, OUT, 0x65, EI, RET, 0);
    org(m, 0x38); db(m, 4, OUT, 0x11, EI, RET);  /* RST 7, injected by INTR */
    org(m, 0x3C); db(m, 4, OUT, 0x75, EI, RET);
    m->ack[0] = 0xFF;
    m->drop[0x55] = I8085_RST55 + 1;
    m->drop[0x65] = I8085_RST65 + 1;
    m->drop[0x11] = I8085_INTR + 1;
    i8085_set_irq(m->cpu, I8085_INTR, 1);
    i8085_set_irq(m->cpu, I8085_RST55, 1);
    i8085_set_irq(m->cpu, I8085_RST65, 1);
    i8085_set_irq(m->cpu, I8085_RST75, 1);
    for (i = 0; i < 40; i++)
        i8085_step(m->cpu);
    sprintf(d, "handlers in order: %02X %02X %02X %02X (want 75 65 55 11), %d in all", m->outs[0], m->outs[1],
            m->outs[2], m->outs[3], m->n_outs);
    report("priority", m->n_outs == 4 && m->outs[0] == 0x75 && m->outs[1] == 0x65 && m->outs[2] == 0x55
                       && m->outs[3] == 0x11, d);
    free_machine(m);
}

/* IE on, 7.5 masked (M7.5; the others open), an edge at step 5; RIM at 0008 -> 9000h; then SIM `sim` at 000D */
static machine *latch_program(int sim)
{
    machine *m = new_machine();
    org(m, 0);
    lxi_sp(m, 0x8000);                          /* 0000 */
    sim_a(m, 0x0C);                             /* 0003, 0005: MSE, M7.5 */
    db(m, 1, EI);                               /* 0006 */
    db(m, 1, NOP);                              /* 0007: step 5 */
    db(m, 1, RIM);                              /* 0008 */
    sta(m, 0x9000);                             /* 0009 */
    sim_a(m, sim);                              /* 000C, 000E: step 9 is the SIM */
    db(m, 4, NOP, NOP, NOP, NOP);               /* 000F */
    org(m, 0x3C); db(m, 1, HLT);
    at(m, 5, I8085_RST75, 1);
    return m;
}

static void t_rst75_latch(void)
{
    machine *m = latch_program(0x08);
    char d[160];
    int i;
    for (i = 0; i < 8; i++)
        i8085_step(m->cpu);
    sprintf(d, "masked: RIM %02X (want 4C: 7.5 pending, IE, M7.5), the vector %s before the SIM", m->mem[0x9000],
            first_step_at(m, 0x3C) < 0 ? "not entered" : "ENTERED");
    report("rst75_latch", m->mem[0x9000] == 0x4C && first_step_at(m, 0x3C) < 0, d);
    free_machine(m);
}

static void t_sim_reveal(void)
{
    machine *m = latch_program(0x08);
    char d[200];
    int i, t[12];
    for (i = 0; i < 12; i++)
        t[i] = i8085_step(m->cpu);
    sprintf(d, "SIM unmasks at step 9 (%d T, want 4); the vector's first boundary at step %d (want 10), %d T (want 12 "
            "+ 5)", t[8], first_step_at(m, 0x3C), t[9]);
    report("sim_reveal", m->pcs[8] == 0x0E && t[8] == 4 && first_step_at(m, 0x3C) == 10 && t[9] == 17, d);
    free_machine(m);
}

static void t_sim_r75(void)
{
    machine *m = latch_program(0x18);           /* MSE and R7.5: masks cleared, the latch reset */
    i8085_regs r;
    char d[160];
    int i;
    for (i = 0; i < 12; i++)
        i8085_step(m->cpu);
    i8085_regs_get(m->cpu, &r);
    sprintf(d, "SIM 18h: the vector %s, RIM view %02X (want 08: nothing pending, IE)",
            first_step_at(m, 0x3C) < 0 ? "not entered" : "ENTERED", r.im);
    report("sim_r75", first_step_at(m, 0x3C) < 0 && r.im == 0x08, d);
    free_machine(m);
}

static void t_rst_levels(void)
{
    machine *m = new_machine();
    char d[260];
    int i, a, b;
    org(m, 0);
    lxi_sp(m, 0x8000);                          /* 0000 */
    sim_a(m, 0x09);                             /* 0003, 0005: M5.5 only */
    db(m, 1, RIM);                              /* 0006 */
    sta(m, 0x9000);                             /* 0007 */
    db(m, 4, NOP, NOP, NOP, NOP);               /* 000A: IE still off */
    db(m, 1, EI);                               /* 000E: step 10 */
    for (i = 0; i < 20; i++)
        db(m, 1, NOP);                          /* 000F */
    org(m, 0x2C); db(m, 1, HLT);
    org(m, 0x34); db(m, 5, OUT, 0x65, EI, RET, 0);
    m->drop[0x65] = I8085_RST65 + 1;
    i8085_set_irq(m->cpu, I8085_RST55, 1);      /* masked all along */
    i8085_set_irq(m->cpu, I8085_RST65, 1);      /* unmasked; IE off until the EI */
    for (i = 0; i < 20; i++)
        i8085_step(m->cpu);
    a = first_step_at(m, 0x34);
    b = count_at(m, 0x34);
    sprintf(d, "RIM %02X (want 31: 6.5 and 5.5 live, M5.5); 6.5 taken at step %d (want 12, after EI and its shadow), "
            "%d time(s) (want 1: its handler drops it); 5.5 (masked) %s", m->mem[0x9000], a, b,
            first_step_at(m, 0x2C) < 0 ? "not taken" : "TAKEN");
    report("rst_levels", m->mem[0x9000] == 0x31 && a == 12 && b == 1 && first_step_at(m, 0x2C) < 0, d);
    free_machine(m);
}

/* IE on (every SIM mask set, as after reset), NOPs; INTR raised at step 4's boundary with the acknowledge `a` */
static machine *intr_program(int a0, int a1, int a2)
{
    machine *m = new_machine();
    int i;
    org(m, 0);
    lxi_sp(m, 0x8000);                          /* 0000 */
    db(m, 1, EI);                               /* 0003 */
    for (i = 0; i < 20; i++)
        db(m, 1, NOP);                          /* 0004..: step 4 is at 0005 */
    org(m, 0x28); db(m, 1, HLT);                /* RST 5 */
    org(m, 0x1234); db(m, 1, HLT);
    m->ack[0] = a0; m->ack[1] = a1; m->ack[2] = a2; m->ack[3] = 0x76;
    at(m, 4, I8085_INTR, 1);
    return m;
}

static void t_intr_rst(void)
{
    machine *m = intr_program(0xEF, 0x00, 0x00);
    i8085_regs r;
    char d[260];
    int i, t[8];
    for (i = 0; i < 8; i++)
        t[i] = i8085_step(m->cpu);
    i8085_regs_get(m->cpu, &r);
    /* step 5: accepted at A, the RST at E; step 6: the vector */
    sprintf(d, "step 5 %d T (want 12), boundary %04X (want 0006, the interrupted PC); acknowledge bytes asked %d "
            "(want 1: byte 0); stacked %02X%02X (want 0006); vector's first boundary at step %d (want 6)",
            t[4], m->pcs[4], m->n_ack, m->mem[0x7FFF], m->mem[0x7FFE], first_step_at(m, 0x28));
    report("intr_rst", t[4] == 12 && m->pcs[4] == 0x06 && m->n_ack == 1 && m->ack_idx[0] == 0 && m->mem[0x7FFF] == 0
                       && m->mem[0x7FFE] == 0x06 && first_step_at(m, 0x28) == 6 && r.sp == 0x7FFE, d);
    free_machine(m);
}

static void t_intr_call(void)
{
    machine *m = intr_program(0xCD, 0x34, 0x12);
    char d[260];
    int i, t[8];
    for (i = 0; i < 8; i++)
        t[i] = i8085_step(m->cpu);
    sprintf(d, "step 5 %d T (want 18); acknowledge bytes %d %d %d, %d asked (want 0 1 2, 3); stacked %02X%02X (want "
            "0006); 1234h's first boundary at step %d (want 6)", t[4], m->ack_idx[0], m->ack_idx[1], m->ack_idx[2],
            m->n_ack, m->mem[0x7FFF], m->mem[0x7FFE], first_step_at(m, 0x1234));
    report("intr_call", t[4] == 18 && m->n_ack == 3 && m->ack_idx[0] == 0 && m->ack_idx[1] == 1 && m->ack_idx[2] == 2
                        && m->mem[0x7FFF] == 0 && m->mem[0x7FFE] == 0x06 && first_step_at(m, 0x1234) == 6, d);
    free_machine(m);
}

static void t_intr_nop(void)
{
    machine *m = intr_program(0x00, 0x00, 0x00);
    i8085_regs r;
    char d[220];
    int t5;
    int i;
    for (i = 0; i < 4; i++)
        i8085_step(m->cpu);
    t5 = i8085_step(m->cpu);
    i8085_regs_get(m->cpu, &r);
    i8085_step(m->cpu);
    sprintf(d, "the NOP's step %d T (want 4), %d stack writes (want 0), IE %s, the next boundary %04X (want 0006, the "
            "interrupted PC, not run in the NOP's step)", t5, m->n_wr, (r.im & 0x08) ? "ON" : "off", m->pcs[5]);
    report("intr_nop", t5 == 4 && m->n_wr == 0 && !(r.im & 0x08) && r.pc == 0x06 && m->pcs[5] == 0x06 && r.sp == 0x8000,
           d);
    free_machine(m);
}

/* an injected conditional (reset leaves F zero: Z clear): its T-states, the acknowledge bytes asked, where it goes */
static void intr_cond(const char *name, int op, int want_t, int want_acks, uint32_t want_next, int want_stack)
{
    machine *m = intr_program(op, 0x34, 0x12);
    char d[260];
    int i, t5, ok_idx = 1;
    for (i = 0; i < 4; i++)
        i8085_step(m->cpu);
    t5 = i8085_step(m->cpu);
    i8085_step(m->cpu);
    for (i = 0; i < m->n_ack; i++)
        ok_idx &= m->ack_idx[i] == i;
    sprintf(d, "%d T (want %d); %d acknowledge bytes asked, in order from 0: %s (want %d); next boundary %04X (want "
            "%04X); %d stack writes (want %d)", t5, want_t, m->n_ack, ok_idx ? "yes" : "NO", want_acks, m->pcs[5],
            want_next, m->n_wr, want_stack);
    report(name, t5 == want_t && m->n_ack == want_acks && ok_idx && m->pcs[5] == want_next && m->n_wr == want_stack, d);
    free_machine(m);
}

static void t_intr_jcc(void)
{
    intr_cond("intr_jcc", 0xCA, 7, 2, 0x0006, 0);          /* JZ, not taken: F R -- bytes 0 and 1; the PC held */
    intr_cond("intr_jcc_taken", 0xC2, 10, 3, 0x1234, 0);   /* JNZ, taken: bytes 0 1 2 */
    intr_cond("intr_ccc", 0xCC, 9, 2, 0x0006, 0);          /* CZ, not taken: S R */
    intr_cond("intr_ccc_taken", 0xC4, 18, 3, 0x1234, 2);   /* CNZ, taken: the interrupted PC stacked */
}

/* an ordinary conditional not taken: the opcode and byte 2 are read, byte 3 is not */
static void t_jcc_reads(void)
{
    static const struct { const char *name; int op, t; } cases[] = {{"jcc_reads", 0xCA, 7}, {"ccc_reads", 0xCC, 9}};
    int k;
    for (k = 0; k < 2; k++) {
        machine *m = new_machine();
        i8085_regs r;
        char d[200];
        int t;
        org(m, 0);
        lxi_sp(m, 0x8000);                      /* 0000 */
        db(m, 3, cases[k].op, 0x34, 0x12);      /* 0003 JZ / CZ 1234h, not taken (Z clear) */
        db(m, 1, NOP);                          /* 0006 */
        i8085_step(m->cpu);
        m->log_reads = 1;
        t = i8085_step(m->cpu);
        m->log_reads = 0;
        i8085_regs_get(m->cpu, &r);
        sprintf(d, "%d T (want %d); reads %d: %04X %04X (want 2: 0003 0004); PC %04X (want 0006)", t, cases[k].t,
                m->n_rd, m->n_rd > 0 ? m->rd_addr[0] : 0, m->n_rd > 1 ? m->rd_addr[1] : 0, r.pc);
        report(cases[k].name, t == cases[k].t && m->n_rd == 2 && m->rd_addr[0] == 3 && m->rd_addr[1] == 4
                              && r.pc == 6, d);
        free_machine(m);
    }
}

/* INTR held; its handler (the injected RST 5's vector) is EI; RET: accepted again and again, the byte index from 0 */
static void t_intr_twice(void)
{
    machine *m = intr_program(0xEF, 0x00, 0x00);
    char d[160];
    int i, all0 = 1;
    org(m, 0x28); db(m, 2, EI, RET);
    for (i = 0; i < 30; i++)
        i8085_step(m->cpu);
    for (i = 0; i < m->n_ack; i++)
        all0 &= m->ack_idx[i] == 0;
    sprintf(d, "%d acceptance(s) of an injected RST (want 3 or more), every one asking byte %s", m->n_ack,
            all0 ? "0" : "n > 0: the index did not restart");
    report("intr_twice", m->n_ack >= 3 && all0, d);
    free_machine(m);
}

static void t_intr_halt(void)
{
    machine *m = new_machine();
    char d[200];
    int i;
    org(m, 0);
    lxi_sp(m, 0x8000);
    db(m, 2, EI, HLT);                          /* HLT at 0004: step 3 */
    org(m, 0x28); db(m, 1, HLT);
    m->ack[0] = 0xEF;
    at(m, 6, I8085_INTR, 1);                    /* a slot's boundary */
    for (i = 0; i < 9; i++)
        i8085_step(m->cpu);
    sprintf(d, "INTR at slot step 6: the RST at step 7, vector at step %d (want 8); stacked %02X%02X (want 0005)",
            first_step_at(m, 0x28), m->mem[0x7FFF], m->mem[0x7FFE]);
    report("intr_halt", first_step_at(m, 0x28) == 8 && m->mem[0x7FFF] == 0 && m->mem[0x7FFE] == 0x05, d);
    free_machine(m);
}

static void t_sid_sod(void)
{
    machine *m = new_machine();
    char d[200];
    int i, n0;
    org(m, 0);
    db(m, 1, RIM); sta(m, 0x9000);              /* SID low */
    sim_a(m, 0xC0);                             /* SDE, SOD = 1 */
    sim_a(m, 0x80);                             /* SOD = 1 but no SDE: no change */
    db(m, 1, RIM); sta(m, 0x9001);              /* SID high (set below) */
    sim_a(m, 0x40);                             /* SDE, SOD = 0 */
    db(m, 1, HLT);
    n0 = m->n_sod;                              /* reset's own SOD = 0 */
    for (i = 0; i < 20; i++) {
        if (i == 6)                             /* before the second RIM */
            m->sid = 1;
        i8085_step(m->cpu);
    }
    sprintf(d, "RIM bit 7: %d then %d (want 0 1); SOD levels after reset: %d change(s) %d %d (want 2: 1 0)",
            m->mem[0x9000] >> 7, m->mem[0x9001] >> 7, m->n_sod - n0, m->sod[n0], m->sod[n0 + 1]);
    report("sid_sod", !(m->mem[0x9000] & 0x80) && (m->mem[0x9001] & 0x80) && m->n_sod - n0 == 2 && m->sod[n0] == 1
                      && m->sod[n0 + 1] == 0, d);
    free_machine(m);
}

typedef struct { uint32_t pc; uint64_t cycles; int t; } rec;

static machine *two_program(int k)
{
    machine *m = k ? trap_program() : halt_program();
    if (k)
        at(m, 3, I8085_TRAP, 1);
    return m;
}

static void t_two_cores(void)
{
    enum { N = 400 };
    static rec alone[2][N], mixed[2][N];
    machine *a, *b;
    char d[160];
    int i, k, same = 1;
    for (k = 0; k < 2; k++) {
        machine *m = two_program(k);
        for (i = 0; i < N; i++) {
            alone[k][i].t = i8085_step(m->cpu);
            alone[k][i].pc = i8085_pc(m->cpu);
            alone[k][i].cycles = i8085_cycles(m->cpu);
        }
        free_machine(m);
    }
    a = two_program(0);
    b = two_program(1);
    for (i = 0; i < N; i++) {
        mixed[0][i].t = i8085_step(a->cpu);
        mixed[0][i].pc = i8085_pc(a->cpu);
        mixed[0][i].cycles = i8085_cycles(a->cpu);
        mixed[1][i].t = i8085_step(b->cpu);
        mixed[1][i].pc = i8085_pc(b->cpu);
        mixed[1][i].cycles = i8085_cycles(b->cpu);
    }
    for (k = 0; k < 2; k++)
        for (i = 0; i < N; i++)
            if (alone[k][i].t != mixed[k][i].t || alone[k][i].pc != mixed[k][i].pc
                || alone[k][i].cycles != mixed[k][i].cycles)
                same = 0;
    sprintf(d, "%d steps each, interleaved against alone: %s", N, same ? "identical" : "DIFFERENT");
    report("two_cores", same, d);
    free_machine(a);
    free_machine(b);
}

int main(void)
{
    t_zero_budget();
    t_reset();
    t_two_cores();
    t_trap_ei();
    t_ei_shadow();
    t_halt_edge();
    t_accept_pc();
    t_halt_masked();
    t_trap_cancel();
    t_trap_edge();
    t_trap_priority_rim();
    t_priority();
    t_rst75_latch();
    t_sim_reveal();
    t_sim_r75();
    t_rst_levels();
    t_intr_rst();
    t_intr_call();
    t_intr_nop();
    t_intr_jcc();
    t_jcc_reads();
    t_intr_twice();
    t_intr_halt();
    t_sid_sod();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
