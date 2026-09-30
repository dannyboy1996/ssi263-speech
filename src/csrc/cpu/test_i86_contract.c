/* test_i86_contract.c -- CONTRACT.md's behaviour of a cpu.h 8086 core on the CORRECTED path (i86_step / i86_run).
 *
 * Each test is a few instructions in a flat 1 MB, driven through cpu.h only, and checks one clause.  T-states are
 * MAME's table (i86.cpp, the pinned revision) with the corrections from Intel's manual (Astra, Reply 104: The 8086
 * Family User's Manual, Oct 1979, Table 2-21, cited as printed page / PDF page); where a test asserts one it says
 * whose figure it is.
 *   zero_budget    i86_run(0) returns 0 and changes nothing (2)
 *   reset          CS:IP = FFFF:0000 and FLAGS F002h (IF off); counts zeroed; a pending NMI edge dropped; a held
 *                  INTR sampled again once IF is set (9)
 *   two_cores      two instances interleaved step by step behave as each alone (9)
 *   intr_vector    INTR, IF on: FLAGS, CS, IP pushed (the interrupted IP), IF and TF cleared, the vector from
 *                  irq_ack(I86_INTR, 0) once; the acceptance charges 61 T (Intel, 2-56/PDF 79; MAME 0) (4)
 *   intr_masked    with IF off INTR is not taken; after STI it is, one instruction later (4, 5)
 *   intr_level     INTR is a level: dropped inside STI's shadow, before acceptance, it is never taken (4)
 *   sti_shadow     an INTR pending at STI is taken after the instruction that follows STI (5)
 *   ss_shadow      after MOV SS and after POP SS the next instruction runs before a pending INTR (5)
 *   nmi_edge       NMI is taken with IF off, once per rising edge (vector 2) (4)
 *   int_iret       INT 21h without the seam vectors through the table and IRET returns: INT n 51 T, IRET 24 T
 *                  (Intel, 2-56/PDF 79; MAME 0 and 32 + its POPF 12) (4)
 *   intercept      the seam: INT 21h offered at E with IP past it, nothing pushed, the host's regs_set kept; a
 *                  vector the host declines is taken through the table (4)
 *   intercept_kinds INT 3 and INTO (OF set) as software, a divide error as an exception with IP past the DIV;
 *                  INTO with OF clear offers nothing (4)
 *   hw_not_offered NMI and INTR are never offered to the seam, even by a host that claims everything (4)
 *   halt           HLT is 2 T; each HALT slot 2 T with i86_pc() past the HLT; an INTR raised at a slot's boundary
 *                  is accepted at the next step, pushing the address after the HLT (6, 7)
 *   halt_masked    with IF off a HALT stays halted through INTR; NMI wakes it (4, 6)
 *   wait           WAIT with TEST low is a slot of 3 T per step (MAME ends the slice); with TEST high it passes
 *   prefix_atomic  a segment override and its instruction are one step: no boundary, no interrupt between (1)
 *   rep_iteration  CS: REP MOVSB is one iteration per step; an INTR between iterations pushes the first prefix's
 *                  IP, and after IRET the copy resumes with the override intact; the first pass 2 + 9 + 17 T, a
 *                  continuing one 17, the resumed one a first pass again (Intel 9 + 17/rep, 2-61/PDF 84) (1, 4)
 *   trap           TF set by POPF: the next instruction runs, then INT 1 (TF cleared in the handler), never
 *                  offered to the seam (4)
 *   trap_ss        with TF set, a MOV SS delays the trap by one more instruction (4, 5)
 *   aliased        60h (JO on the 8086, PUSHA from the 80186 on) is counted with its address; plain code is not
 *   tstates        MOV r16,imm 4, OUT DX,AL 8, IN AL,DX 8, CLI 2, STI 2, JMP short 15 (MAME = Intel); NOP 3,
 *                  LOCK 2, ESC 2 with a register and 8 + EA with memory (Intel; MAME 2, 2, 2 + EA)
 *   io             byte I/O through in/out; a word OUT DX,AX / IN AX,DX is two byte accesses, port then port + 1
 *   flags          FLAGS bits 12-15 read 1 (regs_set, PUSHF after POPF of 0000h)
 *   int_costs      INT n 51, INT 3 52, INTO 53 taken and 4 not (Intel, 2-56/PDF 79); each the SAME when the host's
 *                  seam services it: the instruction's own entry, the omitted handler's time not modelled (4)
 *   accept_costs   an INTR acceptance 61 T, an NMI 50, the single-step trap 50 (2-56/PDF 79, 2-60/PDF 83,
 *                  2-66/PDF 89), each beside the handler's first instruction in its step (4)
 *   divide_error   DIV BL by 0: DIV r8's 80 + the entry 51 (a model: no figure in the manual); AAM 0: 51 (MAME
 *                  charged it nothing); the same when the seam services it (4)
 *   rep_counts     REP MOVSB/CMPSB/SCASB/LODSB/STOSB with CX = 2: 9 + n, then n, with n = 17, 22, 15, 13, 10
 *                  (2-61, 2-53, 2-65, 2-60, 2-66); CX = 0: 9; a REP before a non-string instruction 2 (2-63) (1)
 *   odd_word       4 more T per word transfer at an odd address (every page of Table 2-21): MOV AX,[BX] 13 -> 17,
 *                  MOVSW 18 -> 22, INT 21h at an odd SP 51 -> 63, IRET 24 -> 36, OUT DX,AX to an odd port 12 -> 16
 *                  (12 is MAME's word-port row: Intel's 8 + the 8088's 4 -- open, CONTRACT.md 4) (4)
 *   forward_progress Astra's probe (Reply 104): INT 21h vectored at itself, i86_run(1) returns after one step --
 *                  and so do an intercepted INT 21h in a loop, AAM 0 vectored at itself, a REP before NOP, REP
 *                  MOVSB with CX = 0.  Guarded: a run past 1000 boundaries has its instruction overwritten with a
 *                  NOP by the boundary callback, so a regression FAILS here instead of hanging (2, 4)
 *   step_cost_floor every first byte 00h-FFh, followed by 00h (a memory ModRM) or C0h (a register one), alone and
 *                  after REP / REPNE, with no seam and with a seam that services every interrupt: every step costs
 *                  at least 2 T (2, 4)
 *
 *   build: gcc -c test_i86_contract.c; g++ ... i86_mame.cpp   (../blazie/build_board.py does it)
 * Each line: "ok"/"FAIL", the test's name, the detail; the last line "all passed" or "FAILED"; exit status 0/1.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

typedef struct { int step, line, level; } action;
typedef struct { int vector, kind; uint16_t ip, sp; } offer;

typedef struct {
    uint8_t *mem;                              /* 1 MB */
    uint32_t pos;                              /* the assembler's cursor, linear */
    uint32_t pcs[256];
    int n_pcs;
    i86 *cpu;
    action acts[8];
    int n_acts;
    int vector, n_ack, ack_line, ack_n;        /* irq_ack's answer and what it was asked */
    int n_stack_wr;                            /* writes to the stack page 2000:0000-2000:1000 */
    uint32_t stack_wr_pc[16];
    int claim;                                 /* intercept: -1 all, a vector, or -2 none (declines) */
    offer offers[16];
    int n_offers;
    int out_port[16], out_val[16], n_out;
    int in_port[16], n_in;
    int guard, guard_count, guard_tripped;     /* boundaries allowed before the guard breaks a loop (0: off) */
} machine;

static uint8_t rd(void *ctx, uint32_t a) { return ((machine *)ctx)->mem[a & 0xFFFFF]; }
static void wr(void *ctx, uint32_t a, uint8_t v)
{
    machine *m = (machine *)ctx;
    a &= 0xFFFFF;
    m->mem[a] = v;
    if (a >= 0x20000 && a < 0x21000) {
        if (m->n_stack_wr < 16)
            m->stack_wr_pc[m->n_stack_wr] = i86_pc(m->cpu);
        m->n_stack_wr++;
    }
}
static uint8_t in(void *ctx, uint16_t p)
{
    machine *m = (machine *)ctx;
    if (m->n_in < 16)
        m->in_port[m->n_in++] = p;
    return (uint8_t)(0x40 + (p & 0x0F));
}
static void out(void *ctx, uint16_t p, uint8_t v)
{
    machine *m = (machine *)ctx;
    if (m->n_out < 16) {
        m->out_port[m->n_out] = p;
        m->out_val[m->n_out++] = v;
    }
}
static int ack(void *ctx, int line, int n)
{
    machine *m = (machine *)ctx;
    m->n_ack++;
    m->ack_line = line;
    m->ack_n = n;
    return m->vector;
}
static int intercept(void *ctx, int vector, int kind)
{
    machine *m = (machine *)ctx;
    i86_regs r;
    i86_regs_get(m->cpu, &r);
    if (m->n_offers < 16) {
        m->offers[m->n_offers].vector = vector;
        m->offers[m->n_offers].kind = kind;
        m->offers[m->n_offers].ip = r.ip;
        m->offers[m->n_offers++].sp = r.sp;
    }
    if (m->claim == -1 || m->claim == vector) {
        r.ax = 0x1234;                         /* the host's service result */
        i86_regs_set(m->cpu, &r);
        return 1;
    }
    return 0;
}
static void boundary(void *ctx, uint32_t pc)
{
    machine *m = (machine *)ctx;
    int i;
    if (m->n_pcs < 256)
        m->pcs[m->n_pcs++] = pc;
    if (m->guard && ++m->guard_count > m->guard) {
        m->mem[pc & 0xFFFFF] = 0x90;           /* break the loop: the instruction about to run becomes a NOP */
        m->guard_tripped = 1;
        m->guard = 0;
    }
    for (i = 0; i < m->n_acts; i++)
        if ((long)i86_steps(m->cpu) == m->acts[i].step)
            i86_set_irq(m->cpu, m->acts[i].line, m->acts[i].level);
}

/* a machine at 1000:0000, the stack at 2000:1000, DS = ES = 3000h, IF as asked; the seam only if asked */
static machine *new_machine_ex(int with_intercept, uint16_t flags)
{
    machine *m = (machine *)calloc(1, sizeof(machine));
    cpu_bus bus;
    i86_regs r;
    m->mem = (uint8_t *)calloc(1, 0x100000);
    m->vector = 0x40;
    m->claim = -2;
    memset(&bus, 0, sizeof bus);
    bus.ctx = m;
    bus.read = rd;
    bus.write = wr;
    bus.in = in;
    bus.out = out;
    bus.boundary = boundary;
    bus.irq_ack = ack;
    if (with_intercept)
        bus.intercept = intercept;
    m->cpu = i86_create(&bus, 4772727.0);
    i86_regs_get(m->cpu, &r);
    r.cs = 0x1000; r.ip = 0; r.ss = 0x2000; r.sp = 0x1000; r.ds = r.es = 0x3000;
    r.flags = flags;
    i86_regs_set(m->cpu, &r);
    m->pos = 0x10000;
    return m;
}
static machine *new_machine(uint16_t flags) { return new_machine_ex(0, flags); }

static void free_machine(machine *m)
{
    i86_destroy(m->cpu);
    free(m->mem);
    free(m);
}

static void at(machine *m, int step, int line, int level)
{
    m->acts[m->n_acts].step = step;
    m->acts[m->n_acts].line = line;
    m->acts[m->n_acts++].level = level;
}
static void org(machine *m, uint32_t a) { m->pos = a; }
static void db(machine *m, int n, ...)
{
    va_list ap;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++)
        m->mem[m->pos++ & 0xFFFFF] = (uint8_t)va_arg(ap, int);
    va_end(ap);
}
static void set_vec(machine *m, int n, uint16_t seg, uint16_t off)
{
    m->mem[n * 4] = (uint8_t)off; m->mem[n * 4 + 1] = (uint8_t)(off >> 8);
    m->mem[n * 4 + 2] = (uint8_t)seg; m->mem[n * 4 + 3] = (uint8_t)(seg >> 8);
}
static uint16_t word(const machine *m, uint32_t a) { return (uint16_t)(m->mem[a] | (m->mem[a + 1] << 8)); }
static void regs(machine *m, i86_regs *r) { i86_regs_get(m->cpu, r); }
enum { NOP = 0x90, STI = 0xFB, CLI = 0xFA, HLT = 0xF4, IRET = 0xCF, PUSHF = 0x9C, POPF = 0x9D, PUSH_AX = 0x50 };
enum { F_IF = 0x0200, F_TF = 0x0100, F_OF = 0x0800 };
#define STACK_TOP 0x21000u                      /* 2000:1000 */

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
    printf("%-4s %-15s %s\n", ok ? "ok" : "FAIL", name, detail);
    if (!ok)
        failures++;
}

/* ---- the tests ------------------------------------------------------------------------------------------------ */
static void t_zero_budget(void)
{
    machine *m = new_machine(0x0002);
    char d[160];
    uint64_t done = i86_run(m->cpu, 0);
    sprintf(d, "run(0) = %llu, cycles %llu, steps %llu, boundaries %d", (unsigned long long)done,
            (unsigned long long)i86_cycles(m->cpu), (unsigned long long)i86_steps(m->cpu), m->n_pcs);
    report("zero_budget", done == 0 && i86_cycles(m->cpu) == 0 && i86_steps(m->cpu) == 0 && m->n_pcs == 0, d);
    free_machine(m);
}

static void t_reset(void)
{
    machine *m = new_machine(F_IF | 0x0002);
    i86_regs r;
    char d[240];
    int i, zeroed;
    org(m, 0xFFFF0); db(m, 5, 0xEA, 0x00, 0x00, 0x00, 0x10);     /* JMP FAR 1000:0000 */
    org(m, 0x10000); db(m, 1, STI);                             /* 10000 */
    for (i = 0; i < 6; i++) db(m, 1, NOP);                      /* 10001.. */
    set_vec(m, 2, 0x4000, 0x0020); org(m, 0x40020); db(m, 2, 0xEB, 0xFE);   /* NMI: JMP $ */
    set_vec(m, 0x40, 0x4000, 0x0040); org(m, 0x40040); db(m, 2, 0xEB, 0xFE); /* INTR: JMP $ */
    for (i = 0; i < 3; i++) i86_step(m->cpu);
    i86_set_irq(m->cpu, I86_NMI, 1);             /* an edge, pending */
    i86_set_irq(m->cpu, I86_INTR, 1);            /* a level, held over the reset */
    i86_reset(m->cpu);
    regs(m, &r);
    zeroed = i86_cycles(m->cpu) == 0 && i86_steps(m->cpu) == 0 && i86_pc(m->cpu) == 0xFFFF0;
    m->n_pcs = 0;
    for (i = 0; i < 6; i++) i86_step(m->cpu);
    /* 1 JMP FAR (FFFF0), 2 STI, 3 NOP (the shadow), 4 the INTR vector; never the NMI's */
    sprintf(d, "after reset CS:IP %04X:%04X flags %04X, counts zeroed %d; boundaries %05X %05X %05X %05X; NMI "
            "handler reached %d", r.cs, r.ip, r.flags, zeroed, m->pcs[0], m->pcs[1], m->pcs[2], m->pcs[3],
            count_at(m, 0x40020));
    report("reset", zeroed && r.cs == 0xFFFF && r.ip == 0 && r.flags == 0xF002 && m->pcs[0] == 0xFFFF0
           && m->pcs[1] == 0x10000 && m->pcs[2] == 0x10001 && m->pcs[3] == 0x40040 && count_at(m, 0x40020) == 0, d);
    free_machine(m);
}

static machine *two_program(int k)
{
    machine *m = new_machine(0x0002);
    /* MOV DX,3EEh+k; loop: INC AX; OUT DX,AL; ADD BX,AX; JMP loop */
    db(m, 3, 0xBA, 0xEE + k, 0x03);
    db(m, 1, 0x40); db(m, 1, 0xEE); db(m, 2, 0x01, 0xC3); db(m, 2, 0xEB, 0xFA);
    return m;
}
static void t_two_cores(void)
{
    enum { N = 200 };
    static struct { int t; uint32_t pc; uint64_t cycles; } alone[2][N], mixed[2][N];
    machine *a, *b;
    char d[120];
    int i, k, same = 1;
    for (k = 0; k < 2; k++) {
        machine *m = two_program(k);
        for (i = 0; i < N; i++) {
            alone[k][i].t = i86_step(m->cpu);
            alone[k][i].pc = i86_pc(m->cpu);
            alone[k][i].cycles = i86_cycles(m->cpu);
        }
        free_machine(m);
    }
    a = two_program(0);
    b = two_program(1);
    for (i = 0; i < N; i++) {
        mixed[0][i].t = i86_step(a->cpu); mixed[0][i].pc = i86_pc(a->cpu); mixed[0][i].cycles = i86_cycles(a->cpu);
        mixed[1][i].t = i86_step(b->cpu); mixed[1][i].pc = i86_pc(b->cpu); mixed[1][i].cycles = i86_cycles(b->cpu);
    }
    for (k = 0; k < 2; k++)
        for (i = 0; i < N; i++)
            if (alone[k][i].t != mixed[k][i].t || alone[k][i].pc != mixed[k][i].pc || alone[k][i].cycles != mixed[k][i].cycles)
                same = 0;
    sprintf(d, "%d steps each, interleaved against alone: %s", N, same ? "identical" : "DIFFERENT");
    report("two_cores", same, d);
    free_machine(a);
    free_machine(b);
}

/* STI; NOP x6 at 1000:0000, the INTR vector 40h -> 4000:0040 (NOP; JMP $) */
static machine *intr_program(uint16_t flags)
{
    machine *m = new_machine(flags);
    int i;
    db(m, 1, STI);
    for (i = 0; i < 6; i++) db(m, 1, NOP);
    set_vec(m, 0x40, 0x4000, 0x0040);
    org(m, 0x40040); db(m, 1, NOP); db(m, 2, 0xEB, 0xFE);
    return m;
}

static void t_intr_vector(void)
{
    machine *m = intr_program(0x0002);
    i86_regs r;
    char d[260];
    int t[6], i;
    at(m, 3, I86_INTR, 1);                      /* raised at step 3's boundary (the NOP at 10002) */
    for (i = 0; i < 5; i++) t[i] = i86_step(m->cpu);
    regs(m, &r);
    /* step 4: accepted at A, the handler's NOP at E */
    sprintf(d, "step 4 at %05X; stacked IP %04X CS %04X FLAGS %04X; SP %04X, IF in handler %d; acks %d (line %d, byte "
            "%d); step 4 %d T against a plain NOP's %d (want + 61); i86_pc() at the pushes %05X", m->pcs[3], word(m, STACK_TOP - 6),
            word(m, STACK_TOP - 4), word(m, STACK_TOP - 2), r.sp, !!(r.flags & F_IF), m->n_ack, m->ack_line, m->ack_n,
            t[3], t[2], m->stack_wr_pc[0]);
    report("intr_vector", m->pcs[3] == 0x40040 && word(m, STACK_TOP - 6) == 0x0003 && word(m, STACK_TOP - 4) == 0x1000
           && word(m, STACK_TOP - 2) == (0xF002 | F_IF) && r.sp == 0x0FFA && !(r.flags & (F_IF | F_TF))
           && m->n_ack == 1 && m->ack_line == I86_INTR && m->ack_n == 0 && t[3] == t[2] + 61
           && m->n_stack_wr > 0 && m->stack_wr_pc[0] == 0x10003, d);
    free_machine(m);
}

static void t_intr_masked(void)
{
    machine *m = new_machine(0x0002);
    char d[200];
    int i;
    for (i = 0; i < 5; i++) db(m, 1, NOP);     /* 10000-10004, IF off */
    db(m, 1, STI);                              /* 10005 */
    for (i = 0; i < 4; i++) db(m, 1, NOP);     /* 10006.. */
    set_vec(m, 0x40, 0x4000, 0x0040); org(m, 0x40040); db(m, 2, 0xEB, 0xFE);
    at(m, 1, I86_INTR, 1);
    for (i = 0; i < 9; i++) i86_step(m->cpu);
    /* 1-5 NOPs, 6 STI, 7 NOP (the shadow), 8 the vector */
    sprintf(d, "boundaries 6-8: %05X %05X %05X", m->pcs[5], m->pcs[6], m->pcs[7]);
    report("intr_masked", m->pcs[4] == 0x10004 && m->pcs[5] == 0x10005 && m->pcs[6] == 0x10006 && m->pcs[7] == 0x40040, d);
    free_machine(m);
}

static void t_intr_level(void)
{
    machine *m = intr_program(0x0002);
    char d[160];
    int i;
    at(m, 1, I86_INTR, 1);                      /* before STI runs */
    at(m, 2, I86_INTR, 0);                      /* dropped inside the shadow */
    for (i = 0; i < 6; i++) i86_step(m->cpu);
    sprintf(d, "handler reached %d times, acks %d", count_at(m, 0x40040), m->n_ack);
    report("intr_level", count_at(m, 0x40040) == 0 && m->n_ack == 0, d);
    free_machine(m);
}

static void t_sti_shadow(void)
{
    machine *m = intr_program(0x0002);
    char d[160];
    int i;
    at(m, 1, I86_INTR, 1);                      /* pending when STI executes */
    for (i = 0; i < 4; i++) i86_step(m->cpu);
    sprintf(d, "boundaries %05X %05X %05X; stacked IP %04X", m->pcs[0], m->pcs[1], m->pcs[2], word(m, STACK_TOP - 6));
    report("sti_shadow", m->pcs[0] == 0x10000 && m->pcs[1] == 0x10001 && m->pcs[2] == 0x40040
           && word(m, STACK_TOP - 6) == 0x0002, d);
    free_machine(m);
}

static void t_ss_shadow(void)
{
    char d[240];
    int ok = 1, k, i;
    uint16_t stacked[2];
    for (k = 0; k < 2; k++) {
        machine *m = new_machine(F_IF | 0x0002);
        db(m, 3, 0xB8, 0x00, 0x20);             /* 10000 MOV AX,2000h */
        if (k == 0) {
            db(m, 1, NOP);                      /* 10003 */
            db(m, 2, 0x8E, 0xD0);               /* 10004 MOV SS,AX */
        } else {
            db(m, 1, PUSH_AX);                  /* 10003 */
            db(m, 1, 0x17);                     /* 10004 POP SS */
            db(m, 1, NOP);                      /* 10005 (pads to the same address) */
        }
        db(m, 1, NOP); db(m, 1, NOP);          /* 10006, 10007 */
        set_vec(m, 0x40, 0x4000, 0x0040); org(m, 0x40040); db(m, 2, 0xEB, 0xFE);
        /* raised at the MOV SS / POP SS step's boundary: pending when it executes */
        at(m, 3, I86_INTR, 1);
        for (i = 0; i < 6; i++) i86_step(m->cpu);
        /* MOV SS: steps 1 MOV, 2 NOP, 3 MOV SS, 4 NOP at 10006 (shadow), 5 vector: stacked IP 0007.
           POP SS: 1 MOV, 2 PUSH, 3 POP SS, 4 NOP at 10005 (shadow), 5 vector: stacked IP 0006 */
        stacked[k] = word(m, STACK_TOP - 6);
        ok &= m->pcs[4] == 0x40040 && stacked[k] == (k == 0 ? 0x0007 : 0x0006);
        free_machine(m);
    }
    sprintf(d, "the instruction after MOV SS / POP SS ran first: stacked IP %04X (want 0007), %04X (want 0006)",
            stacked[0], stacked[1]);
    report("ss_shadow", ok, d);
}

static void t_nmi_edge(void)
{
    machine *m = new_machine(0x0002);
    char d[200];
    int i, first, second;
    for (i = 0; i < 20; i++) db(m, 1, NOP);
    set_vec(m, 2, 0x4000, 0x0020); org(m, 0x40020); db(m, 1, NOP); db(m, 1, IRET);
    at(m, 2, I86_NMI, 1);                       /* held high afterwards */
    for (i = 0; i < 8; i++) i86_step(m->cpu);
    i86_set_irq(m->cpu, I86_NMI, 1);            /* asserted again while high: no edge */
    for (i = 0; i < 4; i++) i86_step(m->cpu);
    first = count_at(m, 0x40020);
    i86_set_irq(m->cpu, I86_NMI, 0);
    i86_set_irq(m->cpu, I86_NMI, 1);            /* a new edge */
    for (i = 0; i < 4; i++) i86_step(m->cpu);
    second = count_at(m, 0x40020);
    sprintf(d, "IF off; handler entered %d time(s) while the line stayed high (asserted twice), %d after a new edge; "
            "boundary 3 %05X",
            first, second, m->pcs[2]);
    report("nmi_edge", first == 1 && second == 2 && m->pcs[2] == 0x40020, d);
    free_machine(m);
}

static void t_int_iret(void)
{
    machine *m = new_machine(0x0002);
    i86_regs r;
    char d[240];
    int t1, t2;
    db(m, 2, 0xCD, 0x21); db(m, 1, NOP);       /* 10000 INT 21h; 10002 NOP */
    set_vec(m, 0x21, 0x4000, 0x0100); org(m, 0x40100); db(m, 1, IRET);
    t1 = i86_step(m->cpu);
    {
        uint16_t sip = word(m, STACK_TOP - 6), scs = word(m, STACK_TOP - 4);
        t2 = i86_step(m->cpu);                  /* IRET */
        i86_step(m->cpu);                       /* the NOP after INT */
        regs(m, &r);
        sprintf(d, "boundaries %05X %05X %05X; stacked %04X:%04X; SP %04X after IRET; INT 21h %d T (Intel 51), "
                "IRET %d T (Intel 24)", m->pcs[0], m->pcs[1], m->pcs[2], scs, sip, r.sp, t1, t2);
        report("int_iret", m->pcs[1] == 0x40100 && m->pcs[2] == 0x10002 && sip == 0x0002 && scs == 0x1000
               && r.sp == 0x1000 && t1 == 51 && t2 == 24, d);
    }
    free_machine(m);
}

static void t_intercept(void)
{
    machine *m = new_machine_ex(1, 0x0002);
    i86_regs r;
    char d[260];
    int i, ok;
    m->claim = 0x21;
    db(m, 2, 0xCD, 0x21);                       /* 10000 INT 21h: the host's */
    db(m, 2, 0xCD, 0x22);                       /* 10002 INT 22h: declined, taken through the table */
    db(m, 1, NOP);
    set_vec(m, 0x21, 0x4000, 0x0100); org(m, 0x40100); db(m, 2, 0xEB, 0xFE);
    set_vec(m, 0x22, 0x4000, 0x0200); org(m, 0x40200); db(m, 2, 0xEB, 0xFE);
    i86_step(m->cpu);
    regs(m, &r);
    ok = m->n_offers == 1 && m->offers[0].vector == 0x21 && m->offers[0].kind == I86_INT_SOFTWARE
         && m->offers[0].ip == 0x0002 && m->offers[0].sp == 0x1000 && m->n_stack_wr == 0 && r.ax == 0x1234
         && r.ip == 0x0002 && r.cs == 0x1000;
    sprintf(d, "INT 21h offered %d time(s) (vector %02X, kind %d, IP %04X, SP %04X), stack writes %d, AX %04X after, "
            "next %04X:%04X", m->n_offers, m->offers[0].vector, m->offers[0].kind, m->offers[0].ip, m->offers[0].sp,
            m->n_stack_wr, r.ax, r.cs, r.ip);
    for (i = 0; i < 2; i++) i86_step(m->cpu);
    ok &= m->n_offers == 2 && m->offers[1].vector == 0x22 && m->pcs[2] == 0x40200 && word(m, STACK_TOP - 6) == 0x0004;
    sprintf(d + strlen(d), "; INT 22h declined -> %05X, stacked IP %04X", m->pcs[2], word(m, STACK_TOP - 6));
    report("intercept", ok, d);
    free_machine(m);
}

static void t_intercept_kinds(void)
{
    machine *m = new_machine_ex(1, 0x0002);
    char d[300];
    int i, ok;
    m->claim = -1;
    db(m, 2, 0xB0, 0x7F);                       /* 10000 MOV AL,7Fh */
    db(m, 2, 0x04, 0x01);                       /* 10002 ADD AL,1: OF set */
    db(m, 1, 0xCE);                             /* 10004 INTO */
    db(m, 1, 0xCC);                             /* 10005 INT 3 */
    db(m, 3, 0xB8, 0x05, 0x00);                 /* 10006 MOV AX,5 */
    db(m, 2, 0xB3, 0x00);                       /* 10009 MOV BL,0 */
    db(m, 2, 0xF6, 0xF3);                       /* 1000B DIV BL: divide error */
    db(m, 2, 0x31, 0xC0);                       /* 1000D XOR AX,AX: OF clear */
    db(m, 1, 0xCE);                             /* 1000F INTO: nothing */
    db(m, 1, NOP);
    for (i = 0; i < 10; i++) i86_step(m->cpu);
    ok = m->n_offers == 3
         && m->offers[0].vector == 4 && m->offers[0].kind == I86_INT_SOFTWARE && m->offers[0].ip == 0x0005
         && m->offers[1].vector == 3 && m->offers[1].kind == I86_INT_SOFTWARE && m->offers[1].ip == 0x0006
         && m->offers[2].vector == 0 && m->offers[2].kind == I86_INT_EXCEPTION && m->offers[2].ip == 0x000D;
    sprintf(d, "%d offers: %02X/%d@%04X %02X/%d@%04X %02X/%d@%04X", m->n_offers, m->offers[0].vector, m->offers[0].kind,
            m->offers[0].ip, m->offers[1].vector, m->offers[1].kind, m->offers[1].ip, m->offers[2].vector,
            m->offers[2].kind, m->offers[2].ip);
    report("intercept_kinds", ok, d);
    free_machine(m);
}

static void t_hw_not_offered(void)
{
    machine *m = new_machine_ex(1, F_IF | 0x0002);
    char d[200];
    int i;
    m->claim = -1;
    for (i = 0; i < 12; i++) db(m, 1, NOP);
    set_vec(m, 2, 0x4000, 0x0020); org(m, 0x40020); db(m, 1, IRET);
    set_vec(m, 0x40, 0x4000, 0x0040); org(m, 0x40040); db(m, 1, IRET);
    at(m, 2, I86_NMI, 1);
    at(m, 5, I86_INTR, 1);
    at(m, 6, I86_INTR, 0);
    for (i = 0; i < 9; i++) i86_step(m->cpu);
    sprintf(d, "offers %d; NMI handler %d, INTR handler %d", m->n_offers, count_at(m, 0x40020), count_at(m, 0x40040));
    report("hw_not_offered", m->n_offers == 0 && count_at(m, 0x40020) == 1 && count_at(m, 0x40040) == 1, d);
    free_machine(m);
}

static void t_halt(void)
{
    machine *m = new_machine(0x0002);
    char d[300];
    int t[8], i;
    uint32_t slot_pc;
    db(m, 1, STI); db(m, 1, HLT); db(m, 1, NOP);    /* 10000 STI, 10001 HLT, 10002 */
    set_vec(m, 0x40, 0x4000, 0x0040); org(m, 0x40040); db(m, 1, NOP); db(m, 2, 0xEB, 0xFE);
    at(m, 4, I86_INTR, 1);                      /* at the second slot's boundary */
    for (i = 0; i < 3; i++) t[i] = i86_step(m->cpu);
    slot_pc = i86_pc(m->cpu);
    for (i = 3; i < 6; i++) t[i] = i86_step(m->cpu);
    /* 1 STI, 2 HLT, 3 and 4 slots, 5 the acceptance and the handler's NOP */
    sprintf(d, "HLT %d T, slots %d %d T at %05X; boundary 5 %05X; stacked IP %04X, i86_pc() at the pushes %05X",
            t[1], t[2], t[3], slot_pc, m->pcs[4], word(m, STACK_TOP - 6), m->stack_wr_pc[0]);
    report("halt", t[1] == 2 && t[2] == 2 && t[3] == 2 && slot_pc == 0x10002 && m->pcs[2] == 0x10002
           && m->pcs[3] == 0x10002 && m->pcs[4] == 0x40040 && word(m, STACK_TOP - 6) == 0x0002
           && m->n_stack_wr > 0 && m->stack_wr_pc[0] == 0x10002, d);
    free_machine(m);
}

static void t_halt_masked(void)
{
    machine *m = new_machine(0x0002);
    char d[200];
    int i, still;
    db(m, 1, HLT); db(m, 1, NOP);
    set_vec(m, 0x40, 0x4000, 0x0040); org(m, 0x40040); db(m, 2, 0xEB, 0xFE);
    set_vec(m, 2, 0x4000, 0x0020); org(m, 0x40020); db(m, 2, 0xEB, 0xFE);
    at(m, 2, I86_INTR, 1);
    at(m, 6, I86_NMI, 1);
    for (i = 0; i < 6; i++) i86_step(m->cpu);
    still = count_at(m, 0x10001) == 5 && count_at(m, 0x40040) == 0;
    i86_step(m->cpu);
    sprintf(d, "IF off: %d slots through INTR, INTR handler %d; after NMI boundary %05X", count_at(m, 0x10001),
            count_at(m, 0x40040), m->pcs[6]);
    report("halt_masked", still && m->pcs[6] == 0x40020, d);
    free_machine(m);
}

static void t_wait(void)
{
    machine *m = new_machine(0x0002);
    char d[200];
    int t[5], i;
    db(m, 1, 0x9B); db(m, 1, NOP);              /* 10000 WAIT; 10001 NOP */
    i86_set_irq(m->cpu, I86_TEST, 0);           /* MAME's WAIT waits while this line is low */
    for (i = 0; i < 3; i++) t[i] = i86_step(m->cpu);
    i86_set_irq(m->cpu, I86_TEST, 1);
    for (i = 3; i < 5; i++) t[i] = i86_step(m->cpu);
    sprintf(d, "waiting: %d %d %d T at %05X %05X %05X; released: WAIT %d T, then %05X", t[0], t[1], t[2], m->pcs[0],
            m->pcs[1], m->pcs[2], t[3], m->pcs[4]);
    report("wait", t[0] == 3 && t[1] == 3 && t[2] == 3 && m->pcs[2] == 0x10000 && m->pcs[3] == 0x10000 && t[3] == 3
           && m->pcs[4] == 0x10001, d);
    free_machine(m);
}

static void t_prefix_atomic(void)
{
    machine *m = new_machine(F_IF | 0x0002);
    i86_regs r;
    char d[240];
    int t;
    regs(m, &r); r.bx = 0x0010; i86_regs_set(m->cpu, &r);
    m->mem[0x30010] = 0x5A;
    db(m, 3, 0x26, 0x8A, 0x07);                 /* 10000 ES: MOV AL,[BX] */
    db(m, 1, NOP);
    set_vec(m, 0x40, 0x4000, 0x0040); org(m, 0x40040); db(m, 2, 0xEB, 0xFE);
    at(m, 1, I86_INTR, 1);                      /* pending from the first boundary on */
    t = i86_step(m->cpu);
    i86_step(m->cpu);
    regs(m, &r);
    sprintf(d, "boundaries %05X %05X, stacked IP %04X, AL %02X; the step %d T (MAME: override 2 + MOV r8,m 8 + EA 5; "
            "Intel 15)", m->pcs[0], m->pcs[1], word(m, STACK_TOP - 6), r.ax & 0xFF, t);
    report("prefix_atomic", m->n_pcs == 2 && m->pcs[0] == 0x10000 && m->pcs[1] == 0x40040
           && word(m, STACK_TOP - 6) == 0x0003 && (r.ax & 0xFF) == 0x5A && t == 15, d);
    free_machine(m);
}

static void t_rep_iteration(void)
{
    machine *m = new_machine(F_IF | 0x0002);
    i86_regs r;
    char d[300];
    int t[10], i, ok;
    regs(m, &r); r.cx = 4; r.si = 0x0100; r.di = 0x0200; i86_regs_set(m->cpu, &r);
    memcpy(m->mem + 0x10100, "ABCD", 4);        /* CS:0100 */
    memcpy(m->mem + 0x30100, "wxyz", 4);        /* DS:0100: must not be read */
    org(m, 0x10000); db(m, 3, 0x2E, 0xF3, 0xA4); db(m, 1, NOP);   /* CS: REP MOVSB; NOP */
    set_vec(m, 0x40, 0x4000, 0x0040); org(m, 0x40040); db(m, 1, IRET);
    at(m, 2, I86_INTR, 1);
    at(m, 3, I86_INTR, 0);
    for (i = 0; i < 8; i++) t[i] = i86_step(m->cpu);
    regs(m, &r);
    /* 1, 2 iterations at 10000; 3 the vector (IRET); 4, 5 iterations at 10000; 6 NOP at 10003 */
    ok = m->pcs[0] == 0x10000 && m->pcs[1] == 0x10000 && m->pcs[2] == 0x40040 && m->pcs[3] == 0x10000
         && m->pcs[4] == 0x10000 && m->pcs[5] == 0x10003 && memcmp(m->mem + 0x30200, "ABCD", 4) == 0 && r.cx == 0
         && m->stack_wr_pc[0] == 0x10000 && t[0] == 28 && t[1] == 17 && t[2] == 61 + 24 && t[3] == 28 && t[4] == 17;
    sprintf(d, "boundaries %05X %05X %05X %05X %05X %05X; ES:0200 \"%.4s\", CX %u; passes %d %d T, the INTR + IRET "
            "%d, resumed %d %d (want 28 17, 85, 28 17: CS: 2 + 9 + 17, then 17)", m->pcs[0], m->pcs[1], m->pcs[2],
            m->pcs[3], m->pcs[4], m->pcs[5], (const char *)m->mem + 0x30200, r.cx, t[0], t[1], t[2], t[3], t[4]);
    report("rep_iteration", ok, d);
    free_machine(m);
}

static void t_trap(void)
{
    machine *m = new_machine_ex(1, 0x0002);
    i86_regs r;
    char d[260];
    int i;
    m->claim = -1;                              /* the seam claims everything: the trap is not offered */
    db(m, 3, 0xB8, 0x02, 0x01);                 /* 10000 MOV AX,0102h (TF) */
    db(m, 1, PUSH_AX); db(m, 1, POPF);          /* 10003, 10004 */
    db(m, 1, NOP); db(m, 1, NOP);               /* 10005, 10006 */
    set_vec(m, 1, 0x4000, 0x0010); org(m, 0x40010); db(m, 2, 0xEB, 0xFE);
    for (i = 0; i < 5; i++) i86_step(m->cpu);
    regs(m, &r);
    sprintf(d, "boundaries 4-5 %05X %05X; stacked IP %04X FLAGS %04X; TF in handler %d; offers %d", m->pcs[3], m->pcs[4],
            word(m, STACK_TOP - 6), word(m, STACK_TOP - 2), !!(r.flags & F_TF), m->n_offers);
    report("trap", m->pcs[3] == 0x10005 && m->pcs[4] == 0x40010 && word(m, STACK_TOP - 6) == 0x0006
           && (word(m, STACK_TOP - 2) & F_TF) && !(r.flags & F_TF) && m->n_offers == 0, d);
    free_machine(m);
}

static void t_trap_ss(void)
{
    machine *m = new_machine(0x0002);
    char d[200];
    int i;
    db(m, 3, 0xB8, 0x02, 0x01);                 /* 10000 MOV AX,0102h */
    db(m, 3, 0xBB, 0x00, 0x20);                 /* 10003 MOV BX,2000h */
    db(m, 1, PUSH_AX); db(m, 1, POPF);          /* 10006, 10007 */
    db(m, 2, 0x8E, 0xD3);                       /* 10008 MOV SS,BX (the same stack) */
    db(m, 1, NOP); db(m, 1, NOP);               /* 1000A, 1000B */
    set_vec(m, 1, 0x4000, 0x0010); org(m, 0x40010); db(m, 2, 0xEB, 0xFE);
    for (i = 0; i < 7; i++) i86_step(m->cpu);
    /* without MOV SS the trap follows it (stacked 000A); with it, one instruction later */
    sprintf(d, "boundary 7 %05X, stacked IP %04X (want 000B)", m->pcs[6], word(m, STACK_TOP - 6));
    report("trap_ss", m->pcs[6] == 0x40010 && word(m, STACK_TOP - 6) == 0x000B, d);
    free_machine(m);
}

static void t_aliased(void)
{
    machine *m = new_machine(0x0002), *plain = two_program(0);
    char d[200];
    uint32_t addr = 0;
    uint8_t op = 0;
    uint64_t n, n_plain;
    int i;
    db(m, 1, NOP); db(m, 2, 0x60, 0x00); db(m, 1, NOP);     /* 10001: 60h, JO on the 8086 */
    for (i = 0; i < 3; i++) i86_step(m->cpu);
    n = i86_aliased(m->cpu, &addr, &op);
    for (i = 0; i < 50; i++) i86_step(plain->cpu);
    n_plain = i86_aliased(plain->cpu, NULL, NULL);
    sprintf(d, "%llu aliased (last %02Xh at %05X); plain code %llu", (unsigned long long)n, op, addr,
            (unsigned long long)n_plain);
    report("aliased", n == 1 && addr == 0x10001 && op == 0x60 && n_plain == 0, d);
    free_machine(m);
    free_machine(plain);
}

static void t_tstates(void)
{
    machine *m = new_machine(0x0002);
    char d[200];
    int t[11], i;
    static const int want[11] = { 4, 4, 8, 8, 2, 2, 15, 3, 2, 2, 13 };
    db(m, 3, 0xB8, 0x34, 0x12);                 /* MOV AX,1234h */
    db(m, 3, 0xBA, 0xEE, 0x03);                 /* MOV DX,3EEh */
    db(m, 1, 0xEE);                             /* OUT DX,AL */
    db(m, 1, 0xEC);                             /* IN AL,DX */
    db(m, 1, CLI); db(m, 1, STI);
    db(m, 2, 0xEB, 0x00);                       /* JMP short +0 */
    db(m, 1, NOP);                              /* NOP: 3 (Intel, 2-62/PDF 85) */
    db(m, 1, 0xF0);                             /* LOCK: 2, its own step (2-60/PDF 83) */
    db(m, 2, 0xD8, 0xC0);                       /* ESC with a register: 2 (2-54/PDF 77) */
    db(m, 2, 0xD8, 0x07);                       /* ESC [BX]: 8 + EA 5 */
    for (i = 0; i < 11; i++) t[i] = i86_step(m->cpu);
    sprintf(d, "MOV AX %d, MOV DX %d, OUT %d, IN %d, CLI %d, STI %d, JMP short %d, NOP %d, LOCK %d, ESC reg %d, ESC "
            "[BX] %d; cycles %llu", t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10],
            (unsigned long long)i86_cycles(m->cpu));
    report("tstates", memcmp(t, want, sizeof t) == 0 && i86_cycles(m->cpu) == 63, d);
    free_machine(m);
}

static void t_io(void)
{
    machine *m = new_machine(0x0002);
    i86_regs r;
    char d[240];
    int i;
    db(m, 3, 0xBA, 0xEE, 0x03);                 /* MOV DX,3EEh */
    db(m, 3, 0xB8, 0x34, 0x12);                 /* MOV AX,1234h */
    db(m, 1, 0xEE);                             /* OUT DX,AL */
    db(m, 1, 0xEF);                             /* OUT DX,AX */
    db(m, 1, 0xED);                             /* IN AX,DX */
    for (i = 0; i < 5; i++) i86_step(m->cpu);
    regs(m, &r);
    sprintf(d, "outs %03X=%02X %03X=%02X %03X=%02X; ins %03X %03X -> AX %04X", m->out_port[0], m->out_val[0],
            m->out_port[1], m->out_val[1], m->out_port[2], m->out_val[2], m->in_port[0], m->in_port[1], r.ax);
    report("io", m->n_out == 3 && m->out_port[0] == 0x3EE && m->out_val[0] == 0x34 && m->out_port[1] == 0x3EE
           && m->out_val[1] == 0x34 && m->out_port[2] == 0x3EF && m->out_val[2] == 0x12 && m->n_in == 2
           && m->in_port[0] == 0x3EE && m->in_port[1] == 0x3EF && r.ax == 0x4F4E, d);
    free_machine(m);
}

static void t_flags(void)
{
    machine *m = new_machine(0x0002);
    i86_regs r;
    char d[200];
    uint16_t after_set;
    regs(m, &r);
    after_set = r.flags;
    db(m, 3, 0xB8, 0x00, 0x00);                 /* MOV AX,0 */
    db(m, 1, PUSH_AX); db(m, 1, POPF); db(m, 1, PUSHF);
    i86_run(m->cpu, 1);
    i86_step(m->cpu); i86_step(m->cpu); i86_step(m->cpu);
    sprintf(d, "regs_set 0002h reads %04X; PUSHF after POPF of 0000h stores %04X", after_set, word(m, STACK_TOP - 2));
    report("flags", after_set == 0xF002 && word(m, STACK_TOP - 2) == 0xF002, d);
    free_machine(m);
}

/* ---- the clock counts from Intel's manual and forward progress (Astra, Reply 104) ------------------------------- */

/* INT n, INT 3, INTO taken and not: each step's T, without the seam and with a seam that services every vector */
static void t_int_costs(void)
{
    char d[300];
    int t[2][4], k, ok = 1;
    for (k = 0; k < 2; k++) {
        machine *m = new_machine_ex(k, 0x0002);
        m->claim = k ? -1 : -2;
        db(m, 2, 0xCD, 0x21);                   /* 10000 INT 21h */
        set_vec(m, 0x21, 0x1000, 0x0002);       /* ... returning nowhere: its handler is the next instruction */
        db(m, 1, 0xCC);                         /* 10002 INT 3 */
        set_vec(m, 3, 0x1000, 0x0003);
        db(m, 1, 0xCE);                         /* 10003 INTO, OF clear: not taken */
        db(m, 2, 0xB0, 0x7F); db(m, 2, 0x04, 0x01);   /* 10004 MOV AL,7Fh; 10006 ADD AL,1: OF set */
        db(m, 1, 0xCE);                         /* 10008 INTO: taken */
        set_vec(m, 4, 0x1000, 0x0009);
        db(m, 1, NOP);
        t[k][0] = i86_step(m->cpu);
        t[k][1] = i86_step(m->cpu);
        t[k][2] = i86_step(m->cpu);
        i86_step(m->cpu); i86_step(m->cpu);
        t[k][3] = i86_step(m->cpu);
        ok &= t[k][0] == 51 && t[k][1] == 52 && t[k][2] == 4 && t[k][3] == 53 && m->n_offers == (k ? 3 : 0);
        free_machine(m);
    }
    sprintf(d, "INT 21h / INT 3 / INTO not taken / INTO taken: %d %d %d %d T through the table, %d %d %d %d T serviced "
            "by the host (want 51 52 4 53 both)", t[0][0], t[0][1], t[0][2], t[0][3], t[1][0], t[1][1], t[1][2], t[1][3]);
    report("int_costs", ok, d);
}

/* INTR 61, NMI 50, the trap 50: the accepting step's T minus its first handler instruction's (a NOP, 3) */
static void t_accept_costs(void)
{
    machine *m;
    char d[260];
    int i, t[8], intr, nmi, trap;
    /* INTR: STI; NOP...; raised at step 2's boundary, accepted at step 3 */
    m = intr_program(0x0002);
    at(m, 2, I86_INTR, 1);
    for (i = 0; i < 4; i++) t[i] = i86_step(m->cpu);
    intr = t[2] - 3;
    free_machine(m);
    /* NMI */
    m = new_machine(0x0002);
    for (i = 0; i < 6; i++) db(m, 1, NOP);
    set_vec(m, 2, 0x4000, 0x0020); org(m, 0x40020); db(m, 1, NOP); db(m, 2, 0xEB, 0xFE);
    at(m, 1, I86_NMI, 1);
    for (i = 0; i < 3; i++) t[i] = i86_step(m->cpu);
    nmi = t[1] - 3;
    free_machine(m);
    /* the trap: MOV AX,0102h; PUSH AX; POPF; NOP (the one instruction); the trap with the handler's NOP */
    m = new_machine(0x0002);
    db(m, 3, 0xB8, 0x02, 0x01); db(m, 1, PUSH_AX); db(m, 1, POPF); db(m, 1, NOP); db(m, 1, NOP);
    set_vec(m, 1, 0x4000, 0x0010); org(m, 0x40010); db(m, 1, NOP); db(m, 2, 0xEB, 0xFE);
    for (i = 0; i < 6; i++) t[i] = i86_step(m->cpu);
    trap = t[4] - 3;
    sprintf(d, "INTR %d T, NMI %d T, the single-step trap %d T (want 61, 50, 50); the trap's step at %05X", intr, nmi,
            trap, m->pcs[4]);
    report("accept_costs", intr == 61 && nmi == 50 && trap == 50 && m->pcs[4] == 0x40010, d);
    free_machine(m);
}

/* the divide error's entry: DIV BL by 0 and AAM 0, through the table and serviced by the host */
static void t_divide_error(void)
{
    char d[260];
    int t[2][2], k, ok = 1;
    for (k = 0; k < 2; k++) {
        machine *m = new_machine_ex(k, 0x0002);
        m->claim = k ? -1 : -2;
        db(m, 2, 0xB3, 0x00);                   /* 10000 MOV BL,0 */
        db(m, 2, 0xF6, 0xF3);                   /* 10002 DIV BL: divide error */
        set_vec(m, 0, 0x1000, 0x0004);          /* its handler: the next instruction */
        i86_step(m->cpu);
        t[k][0] = i86_step(m->cpu);
        set_vec(m, 0, 0x1000, 0x0006);
        db(m, 2, 0xD4, 0x00);                   /* 10004 AAM 0: divide error */
        db(m, 1, NOP);
        t[k][1] = i86_step(m->cpu);
        ok &= t[k][0] == 80 + 51 && t[k][1] == 51 && m->n_offers == (k ? 2 : 0);
        free_machine(m);
    }
    sprintf(d, "DIV BL by 0 %d T, AAM 0 %d T through the table; %d, %d serviced by the host (want 131 = DIV r8 80 + "
            "51, and 51)", t[0][0], t[0][1], t[1][0], t[1][1]);
    report("divide_error", ok, d);
}

/* REP string forms: 9 + n per repetition; CX = 0 costs the 9; a REP before a non-string instruction its 2 */
static void t_rep_counts(void)
{
    static const struct { const char *name; uint8_t rep, op; int n; } ops[] = {
        { "MOVSB", 0xF3, 0xA4, 17 }, { "CMPSB", 0xF3, 0xA6, 22 }, { "SCASB", 0xF2, 0xAE, 15 },
        { "LODSB", 0xF3, 0xAC, 13 }, { "STOSB", 0xF3, 0xAA, 10 } };
    char d[400];
    int k, ok = 1, pos = 0, t0, t1, t2, z, nonstr, after;
    for (k = 0; k < 5; k++) {
        machine *m = new_machine(0x0002);
        i86_regs r;
        regs(m, &r); r.cx = 2; r.si = 0x0100; r.di = 0x0200; r.ax = 0x00FF; i86_regs_set(m->cpu, &r);
        memcpy(m->mem + 0x30100, "ab", 2); memcpy(m->mem + 0x30200, "ab", 2);   /* CMPSB equal: REPE runs on */
        db(m, 2, ops[k].rep, ops[k].op);        /* 10000 */
        db(m, 2, ops[k].rep, ops[k].op);        /* 10002: CX = 0 by then */
        t0 = i86_step(m->cpu); t1 = i86_step(m->cpu); t2 = i86_step(m->cpu);
        regs(m, &r);
        ok &= t0 == 9 + ops[k].n && t1 == ops[k].n && t2 == 9 && r.cx == 0 && m->pcs[2] == 0x10002;
        pos += sprintf(d + pos, "%s %d %d, CX=0 %d; ", ops[k].name, t0, t1, t2);
        free_machine(m);
    }
    {
        machine *m = new_machine(0x0002);
        db(m, 1, 0xF3); db(m, 1, NOP);          /* REP NOP: the prefix, then the NOP (MAME: two steps) */
        nonstr = i86_step(m->cpu);
        after = i86_step(m->cpu);
        z = m->pcs[1] == 0x10001;
        ok &= nonstr == 2 && after == 3 && z;
        sprintf(d + pos, "REP before NOP %d, then the NOP %d at %05X", nonstr, after, m->pcs[1]);
        free_machine(m);
    }
    report("rep_counts", ok, d);
}

/* 4 more T per word transfer at an odd address */
static void t_odd_word(void)
{
    char d[400];
    int t[5], e[5], k, ok;
    for (k = 0; k < 2; k++) {                   /* k = 0 even addresses, 1 odd */
        machine *m = new_machine(0x0002);
        i86_regs r;
        int i;
        regs(m, &r); r.bx = (uint16_t)(0x0010 + k); r.si = (uint16_t)(0x0100 + k); r.di = 0x0200;
        r.sp = (uint16_t)(0x1000 - k); r.dx = (uint16_t)(0x3EE + k); i86_regs_set(m->cpu, &r);
        db(m, 2, 0x8B, 0x07);                   /* MOV AX,[BX]: 8 + EA 5 */
        db(m, 1, 0xA5);                         /* MOVSW: 18 (the source odd) */
        db(m, 2, 0xCD, 0x21);                   /* INT 21h: 51 (three pushes) */
        set_vec(m, 0x21, 0x4000, 0x0100); org(m, 0x40100); db(m, 1, IRET);   /* IRET: 24 (three pops) */
        org(m, 0x10005); db(m, 1, 0xEF);        /* OUT DX,AX: MAME's 12 */
        for (i = 0; i < 5; i++)
            (k ? t : e)[i] = i86_step(m->cpu);
        free_machine(m);
    }
    ok = e[0] == 13 && e[1] == 18 && e[2] == 51 && e[3] == 24 && e[4] == 12
         && t[0] == 17 && t[1] == 22 && t[2] == 63 && t[3] == 36 && t[4] == 16;
    sprintf(d, "even / odd: MOV AX,[BX] %d/%d, MOVSW %d/%d, INT 21h %d/%d, IRET %d/%d, OUT DX,AX %d/%d (want 13/17, "
            "18/22, 51/63, 24/36, 12/16)", e[0], t[0], e[1], t[1], e[2], t[2], e[3], t[3], e[4], t[4]);
    report("odd_word", ok, d);
}

/* Astra's probe (Reply 104, i86_zero_probe.c) and its relatives: i86_run(1) must return after one step */
static machine *progress_machine(int with_seam, int claim)
{
    machine *m = new_machine_ex(with_seam, 0x0002);
    m->claim = claim;
    m->guard = 1000;                            /* a regression is broken out of, and fails, instead of hanging */
    return m;
}
static void t_forward_progress(void)
{
    static const char *names[5] = { "INT 21h at itself", "INT 21h serviced, in a loop", "AAM 0 at itself",
                                     "REP before NOP", "REP MOVSB with CX = 0" };
    char d[600];
    int k, ok = 1, pos = 0;
    for (k = 0; k < 5; k++) {
        machine *m;
        uint64_t done, steps;
        int first;
        switch (k) {
        case 0:                                 /* Astra's probe: the vector points at the INT itself */
            m = progress_machine(0, -2);
            db(m, 2, 0xCD, 0x21); set_vec(m, 0x21, 0x1000, 0x0000);
            break;
        case 1:                                 /* the host services it; the loop jumps back to it */
            m = progress_machine(1, -1);
            db(m, 2, 0xCD, 0x21); db(m, 2, 0xEB, 0xFC);
            break;
        case 2:                                 /* AAM 0 raises a divide error vectored at itself */
            m = progress_machine(0, -2);
            db(m, 2, 0xD4, 0x00); set_vec(m, 0, 0x1000, 0x0000);
            break;
        case 3:
            m = progress_machine(0, -2);
            db(m, 1, 0xF3); db(m, 1, NOP); db(m, 2, 0xEB, 0xFC);
            break;
        default:                                /* CX is 0 */
            m = progress_machine(0, -2);
            db(m, 2, 0xF3, 0xA4); db(m, 2, 0xEB, 0xFC);
            break;
        }
        first = i86_step(m->cpu);
        i86_reset(m->cpu);
        {
            i86_regs r;
            regs(m, &r); r.cs = 0x1000; r.ip = 0; r.ss = 0x2000; r.sp = 0x1000; r.ds = r.es = 0x3000; r.flags = 0x0002;
            i86_regs_set(m->cpu, &r);
        }
        m->guard_count = 0;
        done = i86_run(m->cpu, 1);
        steps = i86_steps(m->cpu);
        ok &= first >= 1 && done >= 1 && steps == 1 && !m->guard_tripped;
        pos += sprintf(d + pos, "%s%s: a step %d T, run(1) %llu T in %llu step(s)%s", k ? "; " : "", names[k], first,
                       (unsigned long long)done, (unsigned long long)steps,
                       m->guard_tripped ? " ONLY AFTER THE GUARD BROKE THE LOOP" : "");
        free_machine(m);
    }
    report("forward_progress", ok, d);
}

/* every first byte, alone and after REP / REPNE, with no seam and with a seam servicing everything: >= 2 T */
static void t_step_cost_floor(void)
{
    static const int prefix[3] = { -1, 0xF3, 0xF2 };
    char d[240];
    int seam, p, b, mr, worst = 1000, worst_b = -1, worst_p = -1, worst_seam = -1, worst_mr = -1;
    for (seam = 0; seam < 2; seam++)
        for (p = 0; p < 3; p++)
            for (mr = 0; mr < 2; mr++)          /* the byte after: 00h (a memory ModRM), C0h (a register one) */
                for (b = 0; b < 256; b++) {
                    machine *m = new_machine_ex(seam, 0x0002);
                    i86_regs r;
                    int t;
                    m->claim = seam ? -1 : -2;
                    regs(m, &r); r.cx = 1; i86_regs_set(m->cpu, &r);
                    if (prefix[p] >= 0)
                        db(m, 1, prefix[p]);
                    db(m, 6, b, mr ? 0xC0 : 0, 0, 0, 0, 0);
                    t = i86_step(m->cpu);
                    if (t < worst) {
                        worst = t; worst_b = b; worst_p = prefix[p]; worst_seam = seam; worst_mr = mr;
                    }
                    free_machine(m);
                }
    sprintf(d, "the cheapest step: %d T (first byte %02Xh then %02Xh, prefix %s, %s)", worst, worst_b,
            worst_mr ? 0xC0 : 0, worst_p < 0 ? "none" : worst_p == 0xF3 ? "REP" : "REPNE",
            worst_seam ? "the seam servicing" : "no seam");
    report("step_cost_floor", worst >= 2, d);
}

int main(void)
{
    t_zero_budget();
    t_reset();
    t_two_cores();
    t_intr_vector();
    t_intr_masked();
    t_intr_level();
    t_sti_shadow();
    t_ss_shadow();
    t_nmi_edge();
    t_int_iret();
    t_intercept();
    t_intercept_kinds();
    t_hw_not_offered();
    t_halt();
    t_halt_masked();
    t_wait();
    t_prefix_atomic();
    t_rep_iteration();
    t_trap();
    t_trap_ss();
    t_aliased();
    t_tstates();
    t_io();
    t_flags();
    t_int_costs();
    t_accept_costs();
    t_divide_error();
    t_rep_counts();
    t_odd_word();
    t_forward_progress();
    t_step_cost_floor();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
