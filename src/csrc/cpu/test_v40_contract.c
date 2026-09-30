/* test_v40_contract.c -- CONTRACT.md's behaviour of a cpu.h V40 core on the CORRECTED path (v40_step / v40_run).
 *
 * Each test is a few instructions in a flat 1 MB, driven through cpu.h only, and checks one clause.  Clocks are
 * MAME's (the V20 column of its CLKS tables); where the contract chose (a step per REP iteration, a 2-clock HALT
 * slot, HALT released at a step's A) the test names it.  The interrupt deferrals and HALT's release are NEC's (the
 * 1990 V-Series Data Book's uPD70208 section, printed p.34; Instruction Manual U11301EJ5V0UMJ1 pp.80, 98, 118), after
 * Astra's Reply 103.  The machine: reset's FFFF0h holds a far JMP to 0000:0400h, the code;
 * the stack at 0000:8000h; INT answers vector 0Ch (0000:0600h), NMI is vector 2 (0000:0680h), the divide error
 * vector 0 (0000:06C0h).  The handlers count their entries at 9000h (INT), 9001h (NMI), 9002h (divide) and IRET.
 *   zero_budget      v40_run(0) returns 0 and changes nothing (2)
 *   reset_vector     after reset PS:IP = FFFF:0000, IE off, the first fetch at FFFF0h (9, 12)
 *   reset            reset zeroes the counts and drops a pending NMI edge; a held INT is sampled again (9)
 *   two_cores        two instances interleaved step by step behave as each alone (9)
 *   int_accept       INT raised at a boundary is accepted at the next step's A: irq_ack(V40_INT, 0) once, FLAGS CS
 *                    IP pushed (IP = the next instruction, v40_pc() the same during the pushes), IE cleared, 12
 *                    clocks (MAME's PUSHF), the request consumed (4, 7)
 *   int_masked       with IE = 0 a held INT is not taken
 *   ei_shadow        an INT held through STI is taken after the ONE instruction after it (NEC's EI)
 *   ei_nmi           an NMI raised at STI is not delayed: taken before the instruction after it
 *   ei_halt          STI; HLT with an INT held: the HLT runs, the INT is taken at the first slot
 *   sreg_shadow      after POP SS and after MOV SS,AW one more instruction runs before a pending INT or NMI
 *   sreg_from_shadow after MOV AW,DS0 (a move FROM a segment register) likewise, INT and NMI; after a NOP none does
 *   pop_sreg_shadow  after POP DS0 and POP DS1 likewise, INT and NMI
 *   poll_shadow      after a completed POLL likewise, INT and NMI; after a NOP none does
 *   prefix_atomic    a segment prefix and its instruction are one step: an INT raised at its boundary is taken after
 *                    the whole instruction, which read through the override (12)
 *   rep_steps        REP STOSB of 5: five steps, one iteration each, v40_pc() on the REP each time; the prefix's 2
 *                    clocks charged once, each later step STOSB's 4 (12)
 *   rep_irq          an INT raised between iterations is taken there: IP pushed = the REP prefix, CW = what is left;
 *                    after IRET the REP finishes, every byte written once (12)
 *   rep_seg_irq      ES: REP MOVSB and REP ES: MOVSB interrupted: the whole prefixed instruction restarts, the
 *                    override kept (12)
 *   rep_seg_steps    without an interrupt the override holds through every iteration's step (12)
 *   halt_int         an INT raised at a HALT slot's boundary is accepted at the next step; a slot is 2 clocks,
 *                    v40_pc() on the HLT during slots, the pushed IP and v40_pc() at A are after it (6, 7)
 *   halt_masked_wake with IE = 0 an INT releases HALT at the next step's A without an acknowledge: the instruction
 *                    after the HLT runs; the INT stays pending and is taken after a later STI and one instruction
 *   halt_nmi         with IE = 0 an NMI ends HALT and is taken before the instruction after the HLT
 *   nmi              NMI is an edge: taken with IE = 0, no irq_ack, vector 2; a held line once; a new edge again (4)
 *   nmi_priority     NMI and INT at one boundary: NMI first, INT after the NMI handler's IRET (4)
 *   undefined        opcode 63h: counted with its address, 10 clocks, nothing else; FPO (D8h) is not counted
 *   mode8080_fault   BRKEM enters the 8080 mode, not modelled: v40_regs.fault, then 2-clock slots
 *   div_overflow     DIV by 0: vector 0, the pushed IP after the DIV, AW unchanged (no V20 DIV quirk on the V40)
 *   word_order       a word write, OUT DW,AW and IN AW,DW: two byte accesses, low byte first (8-bit bus)
 *   wrap20           FFFF:0010h is address 0: 20 address lines
 *
 *   build: gcc -c test_v40_contract.c; g++ ... v40_mame.cpp   (../speakout/build_speakout.py does it)
 * Each line: "ok"/"FAIL", the test's name, the detail; the last line "all passed" or "FAILED"; exit status 0/1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

#define MEMSZ 0x100000
#define CODE 0x400
#define ISR_INT 0x600
#define ISR_NMI 0x680
#define ISR_DIV 0x6C0
#define ISR_BRK 0x700

typedef struct { long step; int line, level; } action;
typedef struct { uint32_t addr; uint8_t val; uint32_t pc; } wlog;

typedef struct {
    uint8_t *mem;
    int pos;
    v40 *cpu;
    action acts[8];
    int n_acts;
    int acks, ack_line[8], ack_n[8];
    uint32_t pcs[1024];
    uint64_t bcycles[1024];                    /* v40_cycles() at each boundary */
    uint64_t ecycles[1024];                    /* v40_cycles() after each step (steps()) */
    int n_pcs;
    wlog w[64];                                /* writes outside the code: address, value, v40_pc() then */
    int n_w;
    uint16_t ports[16];
    uint8_t pvals[16];
    int n_ports;
    uint32_t reads[16];
    int n_reads, log_reads;
} machine;

static int failures;

static void report(const char *name, int ok, const char *detail)
{
    printf("%-4s %-16s %s\n", ok ? "ok" : "FAIL", name, detail);
    if (!ok)
        failures++;
}

static uint8_t rd(void *ctx, uint32_t a)
{
    machine *m = (machine *)ctx;
    if (m->log_reads && m->n_reads < 16)
        m->reads[m->n_reads++] = a;
    return m->mem[a & 0xFFFFF];
}
static void wr(void *ctx, uint32_t a, uint8_t v)
{
    machine *m = (machine *)ctx;
    m->mem[a & 0xFFFFF] = v;
    if (m->n_w < 64 && (a < CODE || a >= 0x1000)) {
        m->w[m->n_w].addr = a;
        m->w[m->n_w].val = v;
        m->w[m->n_w++].pc = v40_pc(m->cpu);
    }
}
static uint8_t in(void *ctx, uint16_t p)
{
    machine *m = (machine *)ctx;
    if (m->n_ports < 16) {
        m->ports[m->n_ports] = p;
        m->pvals[m->n_ports++] = 0;
    }
    return (uint8_t)(p & 0xFF);                /* IN returns the port's low byte */
}
static void out(void *ctx, uint16_t p, uint8_t v)
{
    machine *m = (machine *)ctx;
    if (m->n_ports < 16) {
        m->ports[m->n_ports] = p;
        m->pvals[m->n_ports++] = v;
    }
}
static int ack(void *ctx, int line, int n)
{
    machine *m = (machine *)ctx;
    if (m->acks < 8) {
        m->ack_line[m->acks] = line;
        m->ack_n[m->acks] = n;
    }
    m->acks++;
    return 0x0C;
}
static void boundary(void *ctx, uint32_t pc)
{
    machine *m = (machine *)ctx;
    int i;
    if (m->n_pcs < 1024) {
        m->bcycles[m->n_pcs] = v40_cycles(m->cpu);
        m->pcs[m->n_pcs++] = pc;
    }
    for (i = 0; i < m->n_acts; i++)
        if ((long)v40_steps(m->cpu) == m->acts[i].step)
            v40_set_irq(m->cpu, m->acts[i].line, m->acts[i].level);
}

static void put(machine *m, int n, const uint8_t *b)
{
    memcpy(m->mem + m->pos, b, (size_t)n);
    m->pos += n;
}
#define EMIT(m, ...) do { static const uint8_t b_[] = {__VA_ARGS__}; put((m), (int)sizeof b_, b_); } while (0)

static void vec(machine *m, int n, uint16_t off)
{
    m->mem[n * 4] = (uint8_t)off;
    m->mem[n * 4 + 1] = (uint8_t)(off >> 8);
    m->mem[n * 4 + 2] = m->mem[n * 4 + 3] = 0;
}

static machine *new_machine(void)
{
    machine *m = (machine *)calloc(1, sizeof(machine));
    cpu_bus bus;
    m->mem = (uint8_t *)calloc(1, MEMSZ);
    memset(&bus, 0, sizeof bus);
    bus.ctx = m;
    bus.read = rd;
    bus.write = wr;
    bus.in = in;
    bus.out = out;
    bus.irq_ack = ack;
    bus.boundary = boundary;
    /* FFFF0: JMP FAR 0000:0400 */
    m->mem[0xFFFF0] = 0xEA; m->mem[0xFFFF1] = 0x00; m->mem[0xFFFF2] = 0x04; m->mem[0xFFFF3] = 0; m->mem[0xFFFF4] = 0;
    vec(m, 0x0C, ISR_INT);
    vec(m, 2, ISR_NMI);
    vec(m, 0, ISR_DIV);
    vec(m, 0x40, ISR_BRK);
    m->pos = ISR_INT; EMIT(m, 0xFE, 0x06, 0x00, 0x90, 0xCF);    /* INC byte [9000]; IRET */
    m->pos = ISR_NMI; EMIT(m, 0xFE, 0x06, 0x01, 0x90, 0xCF);    /* INC byte [9001]; IRET */
    m->pos = ISR_DIV; EMIT(m, 0xFE, 0x06, 0x02, 0x90, 0xCF);    /* INC byte [9002]; IRET */
    m->pos = CODE;
    EMIT(m, 0xBC, 0x00, 0x80);                                  /* MOV SP,8000h */
    m->cpu = v40_create(&bus, 8e6);
    return m;
}
static void free_machine(machine *m)
{
    v40_destroy(m->cpu);
    free(m->mem);
    free(m);
}
static void at(machine *m, long step, int line, int level)
{
    m->acts[m->n_acts].step = step;
    m->acts[m->n_acts].line = line;
    m->acts[m->n_acts++].level = level;
}
static void steps(machine *m, int n)
{
    while (n-- > 0) {
        v40_step(m->cpu);
        if (v40_steps(m->cpu) <= 1024)
            m->ecycles[v40_steps(m->cpu) - 1] = v40_cycles(m->cpu);
    }
}
/* the step index (1-based, as v40_steps() at its boundary) whose boundary pc was `pc`, from the k-th such */
static long step_at(const machine *m, uint32_t pc, int k)
{
    int i;
    for (i = 0; i < m->n_pcs; i++)
        if (m->pcs[i] == pc && k-- == 0)
            return i + 1;
    return -1;
}
static int count_pc(const machine *m, uint32_t pc)
{
    int i, n = 0;
    for (i = 0; i < m->n_pcs; i++)
        n += m->pcs[i] == pc;
    return n;
}
static uint16_t stacked(const machine *m, int k)          /* the k-th word from SP (0 = IP, 1 = CS, 2 = FLAGS) */
{
    return (uint16_t)(m->mem[0x7FFA + 2 * k] | (m->mem[0x7FFB + 2 * k] << 8));
}

/* ---- the tests --------------------------------------------------------------------------------------------- */

static void t_zero_budget(void)
{
    machine *m = new_machine();
    char d[128];
    uint64_t r;
    steps(m, 3);
    r = v40_run(m->cpu, 0);
    sprintf(d, "run(0) = %llu; steps %llu, cycles %llu unchanged: %s", (unsigned long long)r,
            (unsigned long long)v40_steps(m->cpu), (unsigned long long)v40_cycles(m->cpu),
            v40_steps(m->cpu) == 3 ? "yes" : "NO");
    report("zero_budget", r == 0 && v40_steps(m->cpu) == 3, d);
    free_machine(m);
}

static void t_reset_vector(void)
{
    machine *m = new_machine();
    v40_regs r;
    char d[200];
    uint32_t pc0 = v40_pc(m->cpu);
    v40_regs_get(m->cpu, &r);
    m->log_reads = 1;
    v40_step(m->cpu);
    m->log_reads = 0;
    v40_step(m->cpu);
    sprintf(d, "PS:IP %04X:%04X, IE %d, pc %05X; first boundary %05X, first read %05X; second boundary %05X (want "
            "FFFF:0000, 0, FFFF0, FFFF0, FFFF0, 00400)", r.ps, r.ip, (r.psw >> 9) & 1, pc0, m->pcs[0], m->reads[0],
            m->pcs[1]);
    report("reset_vector", r.ps == 0xFFFF && r.ip == 0 && !(r.psw & 0x200) && pc0 == 0xFFFF0 && m->pcs[0] == 0xFFFF0
                           && m->reads[0] == 0xFFFF0 && m->pcs[1] == CODE, d);
    free_machine(m);
}

static void t_reset(void)
{
    machine *m = new_machine();
    char d[240];
    int nmi_taken, int_acks;
    long sti, isr;
    EMIT(m, 0x90, 0x90, 0xFB, 0x90, 0x90, 0x90, 0xEB, 0xFE);   /* NOP NOP STI NOP NOP NOP JMP $ */
    steps(m, 3);
    v40_set_irq(m->cpu, V40_NMI, 1);                           /* pending, not yet taken ... */
    v40_set_irq(m->cpu, V40_INT, 1);                           /* ... and a held INT */
    v40_reset(m->cpu);
    sprintf(d, "after reset: steps %llu cycles %llu", (unsigned long long)v40_steps(m->cpu),
            (unsigned long long)v40_cycles(m->cpu));
    m->n_pcs = 0;
    steps(m, 12);
    nmi_taken = count_pc(m, ISR_NMI);
    int_acks = m->acks;
    sti = step_at(m, CODE + 5, 0);
    isr = step_at(m, ISR_INT, 0);
    sprintf(d + strlen(d), "; NMI handler entered %d time(s) (want 0); INT: %d acknowledge(s), handler at step %ld, "
            "STI at %ld (want 1, STI + 2: EI's delay)", nmi_taken, int_acks, isr, sti);
    report("reset", v40_steps(m->cpu) == 12 && nmi_taken == 0 && int_acks == 1 && sti > 0 && isr == sti + 2, d);
    free_machine(m);
}

static machine *two_program(int k)
{
    machine *m = new_machine();
    if (k) {
        EMIT(m, 0xFB, 0xB9, 0x40, 0x00, 0xBF, 0x00, 0x30, 0xB0, 0x11, 0xF3, 0xAA, 0xEB, 0xF4);  /* STI; loop REP STOSB */
        at(m, 20, V40_INT, 1);
        at(m, 90, V40_NMI, 1);
    } else {
        EMIT(m, 0xFB, 0xF4, 0xEB, 0xFD);                       /* STI; HLT; JMP back to HLT */
        at(m, 30, V40_INT, 1);
        at(m, 60, V40_INT, 1);
    }
    return m;
}

typedef struct { uint32_t pc; uint64_t cycles; int t; } rec;

static void t_two_cores(void)
{
    enum { N = 400 };
    static rec alone[2][N], mixed[2][N];
    machine *a, *b, *mm[2];
    char d[160];
    int i, k, same = 1;
    for (k = 0; k < 2; k++) {
        machine *m = two_program(k);
        for (i = 0; i < N; i++) {
            alone[k][i].t = v40_step(m->cpu);
            alone[k][i].pc = v40_pc(m->cpu);
            alone[k][i].cycles = v40_cycles(m->cpu);
        }
        free_machine(m);
    }
    a = two_program(0);
    b = two_program(1);
    mm[0] = a;
    mm[1] = b;
    for (i = 0; i < N; i++)
        for (k = 0; k < 2; k++) {
            mixed[k][i].t = v40_step(mm[k]->cpu);
            mixed[k][i].pc = v40_pc(mm[k]->cpu);
            mixed[k][i].cycles = v40_cycles(mm[k]->cpu);
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

static void t_int_accept(void)
{
    machine *m = new_machine();
    char d[300];
    long raised, isr;
    uint64_t acc;
    int pcs_ok, i;
    v40_regs r;
    EMIT(m, 0xFB, 0x90, 0x90, 0x90, 0xEB, 0xFE);              /* STI; NOP NOP NOP; JMP $ */
    steps(m, 3);                                               /* JMP FAR, MOV SP, STI */
    raised = (long)v40_steps(m->cpu) + 1;                      /* the first NOP's step (0403h) */
    at(m, raised, V40_INT, 1);
    steps(m, 2);
    v40_regs_get(m->cpu, &r);
    steps(m, 20);
    isr = step_at(m, ISR_INT, 0);
    acc = isr >= 2 ? m->bcycles[isr - 1] - m->ecycles[isr - 2] : 0;   /* the step's boundary minus its start */
    for (pcs_ok = 1, i = 0; i < m->n_w; i++)
        if (m->w[i].addr >= 0x7FFA && m->w[i].addr < 0x8000 && m->w[i].pc != CODE + 5)
            pcs_ok = 0;
    sprintf(d, "handler at step %ld (want %ld); acks %d (line %d, n %d); pushed IP %04X CS %04X FLAGS %04X "
            "(want 0405, 0000, IE set); IE in handler %d; pc at pushes ok %d; acceptance %llu clocks (want 12); "
            "handler entries %d (want 1)", isr, raised + 1, m->acks, m->ack_line[0], m->ack_n[0], stacked(m, 0),
            stacked(m, 1), stacked(m, 2), (r.psw >> 9) & 1, pcs_ok, (unsigned long long)acc, m->mem[0x9000]);
    report("int_accept", isr == raised + 1 && m->acks == 1 && m->ack_line[0] == V40_INT && m->ack_n[0] == 0
                         && stacked(m, 0) == CODE + 5 && stacked(m, 1) == 0 && (stacked(m, 2) & 0x200)
                         && !(r.psw & 0x200) && pcs_ok && acc == 12 && m->mem[0x9000] == 1, d);
    free_machine(m);
}

static void t_int_masked(void)
{
    machine *m = new_machine();
    char d[120];
    EMIT(m, 0xFA, 0x90, 0x90, 0xEB, 0xFE);                    /* CLI; NOP NOP; JMP $ */
    at(m, 3, V40_INT, 1);
    steps(m, 30);
    sprintf(d, "acks %d, handler entries %d (want 0, 0)", m->acks, m->mem[0x9000]);
    report("int_masked", m->acks == 0 && m->mem[0x9000] == 0, d);
    free_machine(m);
}

/* EI (NEC's V40 data book, 1990, printed p.34; Instruction Manual U11301EJ5V0UMJ1 p.80): the maskable interrupt is
   enabled once the one instruction after EI has run.  CLI; NOP; STI (0405); NOP A (0406); NOP B (0407); JMP $. */
static void t_ei_shadow(void)
{
    machine *m = new_machine();
    char d[200];
    long sti, isr;
    EMIT(m, 0xFA, 0x90, 0xFB, 0x90, 0x90, 0xEB, 0xFE);
    at(m, 3, V40_INT, 1);                                      /* held from the CLI on: pending at the STI */
    steps(m, 12);
    sti = step_at(m, CODE + 5, 0);
    isr = step_at(m, ISR_INT, 0);
    sprintf(d, "STI at step %ld, handler at %ld (want STI + 2: NOP A first); pushed IP %04X (want 0407: NOP B); acks "
            "%d (want 1)", sti, isr, stacked(m, 0), m->acks);
    report("ei_shadow", sti > 0 && isr == sti + 2 && stacked(m, 0) == CODE + 7 && m->acks == 1, d);
    free_machine(m);
}

/* EI's delay is the maskable interrupt's only (data book p.34: "EI instruction (maskable interrupts only)"): an NMI
   raised at the STI's boundary is taken at the next step, before NOP A. */
static void t_ei_nmi(void)
{
    machine *m = new_machine();
    char d[200];
    long sti, nmi;
    EMIT(m, 0xFA, 0x90, 0xFB, 0x90, 0x90, 0xEB, 0xFE);        /* as ei_shadow */
    at(m, 5, V40_NMI, 1);                                      /* the STI's step */
    steps(m, 12);
    sti = step_at(m, CODE + 5, 0);
    nmi = step_at(m, ISR_NMI, 0);
    sprintf(d, "STI at step %ld, NMI handler at %ld (want STI + 1); pushed IP %04X (want 0406: NOP A not yet run); "
            "handler entries %d (want 1)", sti, nmi, stacked(m, 0), m->mem[0x9001]);
    report("ei_nmi", sti == 5 && nmi == sti + 1 && stacked(m, 0) == CODE + 6 && m->mem[0x9001] == 1, d);
    free_machine(m);
}

/* STI; HLT with an INT held: the HLT is EI's one instruction, so the INT is taken at the first slot's A with the
   address after the HLT pushed -- the delay lasts exactly one instruction.  CLI; STI (0404); HLT (0405); NOP; JMP $. */
static void t_ei_halt(void)
{
    machine *m = new_machine();
    char d[200];
    long hlt, isr;
    EMIT(m, 0xFA, 0xFB, 0xF4, 0x90, 0xEB, 0xFE);
    at(m, 3, V40_INT, 1);
    steps(m, 10);
    hlt = step_at(m, CODE + 5, 0);
    isr = step_at(m, ISR_INT, 0);
    sprintf(d, "HLT at step %ld, handler at %ld (want HLT + 1); pushed IP %04X (want 0406, after the HLT)", hlt, isr,
            stacked(m, 0));
    report("ei_halt", hlt == 5 && isr == hlt + 1 && stacked(m, 0) == CODE + 6 && m->mem[0x9000] == 1, d);
    free_machine(m);
}

/* XOR AW,AW (0403); STI (0405); `op` from 0406 (a leading PUSH AW is its own step); then NOP A, NOP B.  `line` is
   raised at the boundary of op's (last) instruction; SS stays 0 and SP 8000h, so the pushed IP is at 7FFAh.  Returns
   the pushed IP, or 0 if the handler did not run exactly once. */
static uint16_t shadow_case(const uint8_t *op, int len, int line)
{
    machine *m = new_machine();
    uint16_t ip;
    long s;
    EMIT(m, 0x31, 0xC0, 0xFB);
    put(m, len, op);
    EMIT(m, 0x90, 0x90, 0xEB, 0xFE);
    steps(m, 4 + (op[0] == 0x50));                             /* JMP FAR, MOV SP, XOR, STI (, PUSH) */
    s = (long)v40_steps(m->cpu) + 1;
    at(m, s, line, 1);
    steps(m, 10);
    ip = m->mem[line == V40_NMI ? 0x9001 : 0x9000] == 1 ? stacked(m, 0) : 0;
    free_machine(m);
    return ip;
}

/* one deferral rule on INT and on NMI: after `op` the instruction after it (NOP A) runs first, so NOP B's address is
   pushed (`want`); after a NOP, none does (0407) */
static void shadow_test(const char *name, const char *what, const uint8_t *op, int len, uint16_t want)
{
    static const uint8_t nop[] = {0x90};
    char d[240];
    uint16_t i = shadow_case(op, len, V40_INT), n = shadow_case(op, len, V40_NMI);
    uint16_t bi = shadow_case(nop, 1, V40_INT), bn = shadow_case(nop, 1, V40_NMI);
    sprintf(d, "pushed IP after %s: INT %04X, NMI %04X (want %04X, %04X: NOP B); after a NOP: INT %04X, NMI %04X "
            "(want 0407: NOP A)", what, i, n, want, want, bi, bn);
    report(name, i == want && n == want && bi == 0x407 && bn == 0x407, d);
}

/* Moves to and from segment registers, POP sreg and POLL defer NMI and INT through the next instruction (data book
   p.34; instruction manual p.98 MOV "dst = sreg or src = sreg", p.118 POP "dst = sreg") */
static void t_sreg_shadow(void)
{
    static const uint8_t pop_ss[] = {0x50, 0x17}, mov_ss[] = {0x8E, 0xD0};
    char d[240];
    uint16_t a = shadow_case(pop_ss, 2, V40_INT), b = shadow_case(mov_ss, 2, V40_INT);
    uint16_t an = shadow_case(pop_ss, 2, V40_NMI), bn = shadow_case(mov_ss, 2, V40_NMI);
    sprintf(d, "pushed IP after POP SS: INT %04X, NMI %04X; after MOV SS,AW: INT %04X, NMI %04X (want 0409: NOP B)",
            a, an, b, bn);
    report("sreg_shadow", a == 0x409 && b == 0x409 && an == 0x409 && bn == 0x409, d);
}

static void t_sreg_from_shadow(void)
{
    static const uint8_t mov_aw_ds0[] = {0x8C, 0xD8};                          /* MOV AW,DS0 (0406) */
    shadow_test("sreg_from_shadow", "MOV AW,DS0", mov_aw_ds0, 2, 0x409);
}

static void t_pop_sreg_shadow(void)
{
    static const uint8_t pop_ds0[] = {0x50, 0x1F}, pop_ds1[] = {0x50, 0x07};  /* PUSH AW; POP DS0 / POP DS1 */
    char d[240];
    uint16_t a = shadow_case(pop_ds0, 2, V40_INT), an = shadow_case(pop_ds0, 2, V40_NMI);
    uint16_t b = shadow_case(pop_ds1, 2, V40_INT), bn = shadow_case(pop_ds1, 2, V40_NMI);
    sprintf(d, "pushed IP after POP DS0: INT %04X, NMI %04X; after POP DS1: INT %04X, NMI %04X (want 0409: NOP B)",
            a, an, b, bn);
    report("pop_sreg_shadow", a == 0x409 && an == 0x409 && b == 0x409 && bn == 0x409, d);
}

static void t_poll_shadow(void)
{
    static const uint8_t poll[] = {0x9B};          /* POLL (0406): completes at once (cpu.h has no POLL line) */
    shadow_test("poll_shadow", "a completed POLL", poll, 1, 0x408);
}

static void t_prefix_atomic(void)
{
    machine *m = new_machine();
    char d[220];
    long s;
    v40_regs r;
    m->mem[0x1000 + 0x1234] = 0x5A;                            /* ES:1234h, ES = 0100h */
    m->mem[0x1234] = 0xA5;                                     /* DS:1234h */
    EMIT(m, 0xB8, 0x00, 0x01, 0x8E, 0xC0, 0xFB,               /* MOV AW,0100h; MOV ES,AW; STI */
         0x26, 0xA0, 0x34, 0x12, 0x90, 0xEB, 0xFE);            /* 0409: ES: MOV AL,[1234h]; NOP; JMP $ */
    steps(m, 5);
    s = (long)v40_steps(m->cpu) + 1;
    at(m, s, V40_INT, 1);
    steps(m, 1);
    v40_regs_get(m->cpu, &r);
    steps(m, 6);
    sprintf(d, "prefixed step pc %05X (want 00409); AL %02X (want 5A, through ES); a boundary at 040A: %d (want 0); "
            "pushed IP %04X (want 040D)", m->pcs[s - 1], r.aw & 0xFF, count_pc(m, CODE + 0xA), stacked(m, 0));
    report("prefix_atomic", m->pcs[s - 1] == CODE + 0x9 && (r.aw & 0xFF) == 0x5A && count_pc(m, CODE + 0xA) == 0
                            && stacked(m, 0) == CODE + 0xD, d);
    free_machine(m);
}

static machine *rep_stosb(int n)
{
    machine *m = new_machine();
    EMIT(m, 0xFB, 0xFC, 0xB9, 0x00, 0x00, 0xBF, 0x00, 0x30, 0xB0, 0x77,   /* STI CLD MOV CW,n MOV IY,3000 MOV AL,77 */
         0xF3, 0xAA, 0x90, 0xEB, 0xFE);                        /* 040D: REP STOSB; NOP; JMP $ */
    m->mem[CODE + 6] = (uint8_t)n;
    return m;
}

static void t_rep_steps(void)
{
    machine *m = rep_stosb(5);
    char d[260];
    int t[8], i, writes = 0, later_ok = 1;
    v40_regs r;
    steps(m, 7);                                               /* up to the REP */
    for (i = 0; i < 6; i++)
        t[i] = v40_step(m->cpu);
    v40_regs_get(m->cpu, &r);
    for (i = 0; i < m->n_w; i++)
        writes += m->w[i].addr >= 0x3000 && m->w[i].addr < 0x3010;
    for (i = 1; i < 5; i++)
        later_ok &= t[i] == 4;
    sprintf(d, "boundaries on the REP %d (want 5), then %05X (want 0040F); CW %d; bytes written %d, 3005h = %02X; "
            "clocks %d %d %d %d %d (want the first with the prefix's 2, then 4 each)", count_pc(m, CODE + 0xD),
            m->pcs[12], r.cw, writes, m->mem[0x3005], t[0], t[1], t[2], t[3], t[4]);
    report("rep_steps", count_pc(m, CODE + 0xD) == 5 && m->pcs[12] == CODE + 0xF && r.cw == 0 && writes == 5
                        && m->mem[0x3005] == 0 && later_ok && t[0] >= 6, d);
    free_machine(m);
}

static void t_rep_irq(void)
{
    machine *m = rep_stosb(6);
    char d[240];
    int i, writes = 0, ok_bytes = 1, before = 0;
    long isr;
    v40_regs r;
    steps(m, 7);
    at(m, (long)v40_steps(m->cpu) + 3, V40_INT, 1);           /* raised at the 3rd iteration's boundary */
    steps(m, 40);
    v40_regs_get(m->cpu, &r);
    isr = step_at(m, ISR_INT, 0);
    for (i = 0; i < isr - 1; i++)
        before += m->pcs[i] == CODE + 0xD;                     /* iterations run before the handler */
    for (i = 0; i < m->n_w; i++)
        writes += m->w[i].addr >= 0x3000 && m->w[i].addr < 0x3010;
    for (i = 0; i < 6; i++)
        ok_bytes &= m->mem[0x3000 + i] == 0x77;
    sprintf(d, "handler after %d iteration(s) (want 3, CW = 3 left); pushed IP %04X (want 040D, the REP); bytes "
            "written %d (want 6, each once), all 77h %d, 3006h = %02X, CW at the end %d", before, stacked(m, 0),
            writes, ok_bytes, m->mem[0x3006], r.cw);
    report("rep_irq", isr > 0 && before == 3 && stacked(m, 0) == CODE + 0xD && writes == 6 && ok_bytes
                      && m->mem[0x3006] == 0 && m->mem[0x9000] == 1 && r.cw == 0, d);
    free_machine(m);
}

/* ES: REP MOVSB (seg_first) or REP ES: MOVSB, 6 bytes from ES:2000h (ES = 0100h: 3000h..) to ES:2100h (3100h..);
   DS:2000h holds EEh.  irq_at: raise INT at that iteration's boundary (0: never). */
static void rep_seg(int seg_first, int irq_at, int *ok_out, uint16_t *pushed, int *iters)
{
    machine *m = new_machine();
    int i, ok = 1;
    for (i = 0; i < 6; i++) {
        m->mem[0x3000 + i] = (uint8_t)(i + 1);
        m->mem[0x2000 + i] = 0xEE;
    }
    EMIT(m, 0xFB, 0xFC, 0xB8, 0x00, 0x01, 0x8E, 0xC0,          /* STI CLD MOV AW,0100h MOV ES,AW */
         0xB9, 0x06, 0x00, 0xBE, 0x00, 0x20, 0xBF, 0x00, 0x21); /* MOV CW,6 MOV IX,2000h MOV IY,2100h */
    if (seg_first)
        EMIT(m, 0x26, 0xF3, 0xA4);                            /* 0413: ES: REP MOVSB */
    else
        EMIT(m, 0xF3, 0x26, 0xA4);                            /* 0413: REP ES: MOVSB */
    EMIT(m, 0x90, 0xEB, 0xFE);
    steps(m, 9);
    if (irq_at)
        at(m, (long)v40_steps(m->cpu) + irq_at, V40_INT, 1);
    steps(m, 30);
    for (i = 0; i < 6; i++)
        ok &= m->mem[0x3100 + i] == i + 1;
    *ok_out = ok && m->mem[0x3106] == 0 && (irq_at == 0 || m->mem[0x9000] == 1);
    *pushed = stacked(m, 0);
    *iters = count_pc(m, CODE + 0x13);
    free_machine(m);
}

static void t_rep_seg_irq(void)
{
    char d[240];
    int ok1, ok2, it1, it2;
    uint16_t p1, p2;
    rep_seg(1, 2, &ok1, &p1, &it1);
    rep_seg(0, 2, &ok2, &p2, &it2);
    sprintf(d, "ES: REP MOVSB: copied from ES %d, pushed IP %04X (want 0413, the ES prefix); REP ES: MOVSB: copied %d, "
            "pushed %04X (want 0413)", ok1, p1, ok2, p2);
    report("rep_seg_irq", ok1 && ok2 && p1 == CODE + 0x13 && p2 == CODE + 0x13, d);
}

static void t_rep_seg_steps(void)
{
    char d[200];
    int ok1, ok2, it1, it2;
    uint16_t p1, p2;
    rep_seg(1, 0, &ok1, &p1, &it1);
    rep_seg(0, 0, &ok2, &p2, &it2);
    sprintf(d, "ES: REP MOVSB copied from ES %d in %d step(s); REP ES: MOVSB %d in %d (want 1 in 6, 1 in 6)", ok1, it1,
            ok2, it2);
    report("rep_seg_steps", ok1 && ok2 && it1 == 6 && it2 == 6, d);
}

static void t_halt_int(void)
{
    machine *m = new_machine();
    char d[260];
    long hlt, s;
    int slots_ok = 1, i, t_slot = 0, pcs_ok = 1;
    EMIT(m, 0xFB, 0xF4, 0x90, 0xEB, 0xFE);                    /* STI; HLT (0404); NOP; JMP $ */
    steps(m, 4);                                               /* ..., HLT */
    for (i = 0; i < 3; i++) {
        t_slot = v40_step(m->cpu);
        slots_ok &= t_slot == 2 && v40_pc(m->cpu) == CODE + 4;
    }
    s = (long)v40_steps(m->cpu) + 1;
    at(m, s, V40_INT, 1);
    steps(m, 6);
    hlt = step_at(m, CODE + 4, 0);
    for (i = 0; i < m->n_w; i++)
        if (m->w[i].addr >= 0x7FFA && m->w[i].addr < 0x8000 && m->w[i].pc != CODE + 5)
            pcs_ok = 0;
    sprintf(d, "slots 2 clocks with pc on the HLT %d (last %d); handler at step %ld (want %ld); pushed IP %04X "
            "(want 0405); pc at the pushes 00405 %d; handler run %d time(s) (want 1)", slots_ok, t_slot,
            step_at(m, ISR_INT, 0), s + 1, stacked(m, 0), pcs_ok, m->mem[0x9000]);
    report("halt_int", hlt > 0 && slots_ok && step_at(m, ISR_INT, 0) == s + 1 && stacked(m, 0) == CODE + 5 && pcs_ok
                       && m->mem[0x9000] == 1, d);
    free_machine(m);
}

/* NEC's V40 data book (1990, printed p.34): "In the case of the INT input being masked, execution will begin with the
   instruction immediately following the HALT instruction without an intervening interrupt acknowledge bus cycle.
   When maskable interrupts are again enabled, the interrupt will be serviced."  Release and acceptance are separate,
   both at a step's A.  CLI; HLT (0404); NOP (0405); NOP; STI (0407); NOP A (0408); NOP B (0409); JMP $.  The INT is
   raised at a slot's boundary (step 6) and held. */
static void t_halt_masked_wake(void)
{
    machine *m = new_machine();
    char d[320];
    v40_regs r5, r6;
    long resume, sti, isr;
    EMIT(m, 0xFA, 0xF4, 0x90, 0x90, 0xFB, 0x90, 0x90, 0xEB, 0xFE);
    at(m, 6, V40_INT, 1);
    steps(m, 5);                                               /* JMP FAR, MOV SP, CLI, HLT, a slot */
    v40_regs_get(m->cpu, &r5);
    steps(m, 1);                                               /* the slot at whose boundary the INT rises */
    v40_regs_get(m->cpu, &r6);
    steps(m, 20);
    resume = step_at(m, CODE + 5, 0);
    sti = step_at(m, CODE + 7, 0);
    isr = step_at(m, ISR_INT, 0);
    sprintf(d, "halted before %d, at the raise's step %d (want 1, 1); resumed at 0405 at step %ld (want 7, the next), "
            "run %d time(s) (want 1); STI at %ld, handler at %ld (want STI + 2), pushed IP %04X (want 0409); acks %d "
            "(want 1: none at the release)", r5.halted, r6.halted, resume, count_pc(m, CODE + 5), sti, isr,
            stacked(m, 0), m->acks);
    report("halt_masked_wake", r5.halted && r6.halted && resume == 7 && count_pc(m, CODE + 5) == 1 && sti > resume
                               && isr == sti + 2 && stacked(m, 0) == CODE + 9 && m->acks == 1
                               && m->mem[0x9000] == 1, d);
    free_machine(m);
}

/* With IE = 0, an NMI ends HALT and is processed before the instruction after the HLT (data book p.34). */
static void t_halt_nmi(void)
{
    machine *m = new_machine();
    char d[200];
    v40_regs r;
    long n;
    EMIT(m, 0xFA, 0xF4, 0x90, 0xEB, 0xFE);                    /* CLI; HLT; NOP; JMP $ */
    steps(m, 8);
    v40_regs_get(m->cpu, &r);
    n = (long)v40_steps(m->cpu) + 1;
    at(m, n, V40_NMI, 1);
    steps(m, 4);
    sprintf(d, "halted %d (want 1); NMI: handler at step %ld (want %ld), pushed IP %04X (want 0405), handler run %d "
            "time(s) (want 1)", r.halted, step_at(m, ISR_NMI, 0), n + 1, stacked(m, 0), m->mem[0x9001]);
    report("halt_nmi", r.halted && step_at(m, ISR_NMI, 0) == n + 1 && stacked(m, 0) == CODE + 5
                       && m->mem[0x9001] == 1, d);
    free_machine(m);
}

static void t_nmi(void)
{
    machine *m = new_machine();
    char d[200];
    int first;
    EMIT(m, 0xFA, 0x90, 0x90, 0x90, 0xEB, 0xFD);              /* CLI; NOP NOP NOP; loop to the last NOP */
    at(m, 4, V40_NMI, 1);
    steps(m, 40);
    first = m->mem[0x9001];
    at(m, 45, V40_NMI, 0);
    at(m, 50, V40_NMI, 1);
    steps(m, 30);
    sprintf(d, "IE = 0: handler entries while held %d (want 1), after a new edge %d (want 2); acks %d (want 0); "
            "vector 2 -> 0680h, IE after IRET %d", first, m->mem[0x9001], m->acks, 0);
    report("nmi", first == 1 && m->mem[0x9001] == 2 && m->acks == 0, d);
    free_machine(m);
}

static void t_nmi_priority(void)
{
    machine *m = new_machine();
    char d[160];
    long nmi, isr;
    EMIT(m, 0xFB, 0x90, 0x90, 0xEB, 0xFE);
    at(m, 4, V40_INT, 1);
    at(m, 4, V40_NMI, 1);
    steps(m, 20);
    nmi = step_at(m, ISR_NMI, 0);
    isr = step_at(m, ISR_INT, 0);
    sprintf(d, "NMI handler at step %ld, INT handler at %ld (want 5, then after the NMI handler's 2 steps: 7)", nmi, isr);
    report("nmi_priority", nmi == 5 && isr == 7, d);
    free_machine(m);
}

static void t_undefined(void)
{
    machine *m = new_machine();
    char d[200];
    v40_regs a, b, c;
    int t;
    EMIT(m, 0x90, 0x63, 0xD8, 0xC0, 0xEB, 0xFE);              /* NOP; 63h; FPO1 D8 C0; JMP $ */
    steps(m, 3);
    v40_regs_get(m->cpu, &a);
    t = v40_step(m->cpu);                                      /* 63h */
    v40_regs_get(m->cpu, &b);
    v40_step(m->cpu);                                          /* FPO */
    v40_regs_get(m->cpu, &c);
    sprintf(d, "63h: counted %u at %05X (want 1 at 00404), IP %04X (want 0405), clocks %d (want 10 + queue), registers "
            "and flags kept %d; FPO counted %u (want 1 still)", b.undefined, b.undefined_at, b.ip, t,
            a.aw == b.aw && a.cw == b.cw && a.psw == b.psw && a.sp == b.sp, c.undefined);
    report("undefined", a.undefined == 0 && b.undefined == 1 && b.undefined_at == CODE + 4 && b.ip == CODE + 5
                        && t >= 10 && a.aw == b.aw && a.psw == b.psw && c.undefined == 1 && c.ip == CODE + 7, d);
    free_machine(m);
}

static void t_mode8080_fault(void)
{
    machine *m = new_machine();
    char d[200];
    v40_regs a, b;
    int t1, t2;
    EMIT(m, 0x0F, 0xFF, 0x40, 0x90);                           /* BRKEM 40h */
    steps(m, 3);
    v40_regs_get(m->cpu, &a);
    t1 = v40_step(m->cpu);
    t2 = v40_step(m->cpu);
    v40_regs_get(m->cpu, &b);
    sprintf(d, "after BRKEM: IP %04X (want 0700: its vector); then slots of %d, %d clocks (want 2), fault %d (want 1), "
            "IP kept %04X", a.ip, t1, t2, b.fault, b.ip);
    report("mode8080_fault", a.ip == ISR_BRK && t1 == 2 && t2 == 2 && b.fault && b.ip == a.ip, d);
    free_machine(m);
}

static void t_div_overflow(void)
{
    machine *m = new_machine();
    char d[200];
    v40_regs r;
    EMIT(m, 0xB8, 0x34, 0x12, 0xB3, 0x00, 0xF6, 0xF3, 0x90, 0xEB, 0xFE);   /* MOV AW,1234h; MOV BL,0; DIV BL */
    steps(m, 5);
    v40_regs_get(m->cpu, &r);
    steps(m, 5);
    sprintf(d, "after DIV BL (BL = 0): IP %04X (want 06C0: vector 0), pushed IP %04X (want 040A, after the DIV), AW "
            "%04X (want 1234), handler entries %d", r.ip, stacked(m, 0), r.aw, m->mem[0x9002]);
    report("div_overflow", r.ip == ISR_DIV && stacked(m, 0) == CODE + 0xA && r.aw == 0x1234 && m->mem[0x9002] == 1, d);
    free_machine(m);
}

static void t_word_order(void)
{
    machine *m = new_machine();
    char d[260];
    v40_regs r;
    int i, w0 = -1;
    EMIT(m, 0xB8, 0xAA, 0xBB, 0xA3, 0x00, 0x50,               /* MOV AW,BBAAh; MOV [5000h],AW */
         0xBA, 0x40, 0x00, 0xEF, 0xED, 0xEB, 0xFE);            /* MOV DW,40h; OUT DW,AW; IN AW,DW */
    steps(m, 7);
    v40_regs_get(m->cpu, &r);
    for (i = 0; i < m->n_w; i++)
        if (m->w[i].addr == 0x5000) {
            w0 = i;
            break;
        }
    sprintf(d, "memory: %05X=%02X then %05X=%02X; out %02X=%02X then %02X=%02X; in %02X then %02X, AW %04X (want 5000 "
            "AA, 5001 BB; 40 AA, 41 BB; 40, 41, 4140)", w0 >= 0 ? m->w[w0].addr : 0, w0 >= 0 ? m->w[w0].val : 0,
            w0 >= 0 ? m->w[w0 + 1].addr : 0, w0 >= 0 ? m->w[w0 + 1].val : 0, m->ports[0], m->pvals[0], m->ports[1],
            m->pvals[1], m->ports[2], m->ports[3], r.aw);
    report("word_order", w0 >= 0 && m->w[w0].val == 0xAA && m->w[w0 + 1].addr == 0x5001 && m->w[w0 + 1].val == 0xBB
                         && m->n_ports == 4 && m->ports[0] == 0x40 && m->pvals[0] == 0xAA && m->ports[1] == 0x41
                         && m->pvals[1] == 0xBB && m->ports[2] == 0x40 && m->ports[3] == 0x41 && r.aw == 0x4140, d);
    free_machine(m);
}

static void t_wrap20(void)
{
    machine *m = new_machine();
    char d[160];
    EMIT(m, 0xB8, 0xFF, 0xFF, 0x8E, 0xD8, 0xC6, 0x06, 0x10, 0x00, 0x42, 0xEB, 0xFE);   /* DS = FFFFh; MOV [10h],42h */
    steps(m, 5);
    sprintf(d, "the write went to %05X = %02X (want 00000 = 42)", m->n_w ? m->w[m->n_w - 1].addr : 0xFFFFFu,
            m->n_w ? m->w[m->n_w - 1].val : 0);
    report("wrap20", m->n_w && m->w[m->n_w - 1].addr == 0 && m->mem[0] == 0x42, d);
    free_machine(m);
}

int main(void)
{
    t_zero_budget();
    t_reset_vector();
    t_reset();
    t_two_cores();
    t_int_accept();
    t_int_masked();
    t_ei_shadow();
    t_ei_nmi();
    t_ei_halt();
    t_sreg_shadow();
    t_sreg_from_shadow();
    t_pop_sreg_shadow();
    t_poll_shadow();
    t_prefix_atomic();
    t_rep_steps();
    t_rep_irq();
    t_rep_seg_irq();
    t_rep_seg_steps();
    t_halt_int();
    t_halt_masked_wake();
    t_halt_nmi();
    t_nmi();
    t_nmi_priority();
    t_undefined();
    t_mode8080_fault();
    t_div_overflow();
    t_word_order();
    t_wrap20();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
