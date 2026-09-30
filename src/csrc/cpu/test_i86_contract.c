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
 *   wait           WAIT is Intel's 3 + 5n (2-67/PDF 90): TEST asserted (the pin LOW), 3 and done; not asserted,
 *                  the entry 3 then a 5-T recheck a step with IP held, the recheck that finds TEST active ending it
 *                  (MAME ends the slice) (6)
 *   wait_interrupt an INTR between rechecks pushes the WAIT's address; after IRET the WAIT is entered again, 3 (4, 6)
 *   wait_prefixed  A REPRODUCER of today's behaviour, not a hardware claim (Reply 110): ES: WAIT pays 2 + 3, then
 *                  rechecks at the 9Bh byte without its prefix; an INTR between rechecks pushes the WAIT's address,
 *                  not the prefix's; after IRET it is entered again without the prefix, 3 (4)
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
 *                  MOVSW 18 -> 22, INT 21h at an odd SP 51 -> 63, IRET 24 -> 36, OUT DX,AX to an odd port 8 -> 12 (4)
 *   after Astra's Reply 106 (the 8086's own rows, not MAME's 8088-flavoured ones), each at even AND odd addresses,
 *   both absolute (an odd-minus-even check alone passes a wrong base):
 *   stack_counts   PUSH r16 11, PUSH sreg 10, PUSHF 10, PUSH mem 16 + EA, POP mem 17 + EA, POPF 8, POP sreg 8, POP
 *                  r16 8 (2-62, 2-63); + 4 at an odd SP, + 4 more for an odd memory operand (4)
 *   port_counts    IN/OUT AL and AX: imm8 10, DX 8 (2-55, 2-62); + 4 for a word at an odd port, never for a byte (4)
 *   return_counts  RET 8, RET n 12, RETF 18, RETF n 17 (2-64/PDF 87), CALL near 19, far 28 (2-52); + 4 a word
 *                  popped or pushed at an odd SP (RETF and CALL far: two) (4)
 *   word_memory_counts DIV/IDIV/MUL/IMUL m16 150/171/124/134 + EA, LES/LDS 16 + EA (2-54, 2-55, 2-61, 2-59); + 4 a
 *                  word at an odd address (LES/LDS: two) (4)
 *   after Astra's Reply 110:
 *   test_imm_counts TEST r8/r16,imm 5 and TEST m8/m16,imm 11 + EA (2-67/PDF 90; MAME 4 and 10 + EA) at an even and
 *                  an odd BX (a word + 4, a byte never); TEST AL/AX,imm stay 4, CMP r,imm 4 and CMP m,imm 10 + EA (4)
 *   loopne_counts  LOOPNE taken 19 (2-60/PDF 83; MAME 17), not taken 5 with CX run out and with ZF set; LOOP 17/5 and
 *                  LOOPE 18/6 unchanged
 *   imul_flags_byte, imul_flags_word  IMUL's CF = OF = 1 exactly when the signed product does not fit the source's
 *                  width (2-37/PDF 60; MAME: AH/DX nonzero): Astra's five cases, the edges and negative overflow,
 *                  register and memory, the product checked too; AF/PF/SF/ZF (undefined) not asserted
 *   mul_flags      MUL, unchanged: CF = OF = the upper half nonzero, byte and word, register and memory
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

/* WAIT, Intel's 3 + 5n (Astra, Reply 106).  cpu.h's I86_TEST is LOGICAL: asserted (1) = TEST active = the TEST pin
   electrically LOW ("If the TEST input is LOW execution continues", the 8086 data sheet, printed B-9/PDF 552);
   not asserted (0) = the pin HIGH = a WAIT waits.  A new core's line is asserted. */
static void t_wait(void)
{
    machine *m = new_machine(0x0002);
    char d[300];
    int ready[2], t[6], i, ok;
    uint32_t ready_next;
    /* ready at once: the line as a new core has it (asserted) -- the entry's 3 and done (n = 0) */
    db(m, 1, 0x9B); db(m, 1, NOP);              /* 10000 WAIT; 10001 NOP */
    ready[0] = i86_step(m->cpu);
    ready[1] = i86_step(m->cpu);
    ready_next = m->pcs[1];
    ok = ready[0] == 3 && ready[1] == 3 && ready_next == 0x10001;
    free_machine(m);
    /* not ready: the entry 3, rechecks of 5 with IP held, TEST asserted before the 4th step: that recheck ends it */
    m = new_machine(0x0002);
    db(m, 1, 0x9B); db(m, 1, NOP);
    i86_set_irq(m->cpu, I86_TEST, 0);           /* not asserted: the pin HIGH, inactive */
    for (i = 0; i < 3; i++) t[i] = i86_step(m->cpu);
    i86_set_irq(m->cpu, I86_TEST, 1);           /* asserted: the pin LOW, active */
    for (i = 3; i < 5; i++) t[i] = i86_step(m->cpu);
    ok &= t[0] == 3 && t[1] == 5 && t[2] == 5 && t[3] == 5 && t[4] == 3 && m->pcs[0] == 0x10000
          && m->pcs[1] == 0x10000 && m->pcs[2] == 0x10000 && m->pcs[3] == 0x10000 && m->pcs[4] == 0x10001;
    sprintf(d, "ready: WAIT %d T, then %05X; waiting: %d %d %d T at %05X %05X %05X, TEST active: %d T, then %05X "
            "(want 3; 3 5 5, 5: 3 + 5n = 18 with n = 3)", ready[0], ready_next, t[0], t[1], t[2], m->pcs[0], m->pcs[1],
            m->pcs[2], t[3], m->pcs[4]);
    report("wait", ok, d);
    free_machine(m);
}

/* an INTR between WAIT's rechecks: taken with the WAIT's address pushed; after IRET the WAIT is entered again (3) */
static void t_wait_interrupt(void)
{
    machine *m = new_machine(F_IF | 0x0002);
    char d[300];
    int t[7], i, ok;
    db(m, 1, 0x9B); db(m, 1, NOP);              /* 10000 WAIT; 10001 NOP */
    set_vec(m, 0x40, 0x4000, 0x0040); org(m, 0x40040); db(m, 1, IRET);
    i86_set_irq(m->cpu, I86_TEST, 0);
    at(m, 2, I86_INTR, 1);                      /* raised at the first recheck's boundary */
    at(m, 3, I86_INTR, 0);
    for (i = 0; i < 5; i++) t[i] = i86_step(m->cpu);
    i86_set_irq(m->cpu, I86_TEST, 1);
    for (i = 5; i < 7; i++) t[i] = i86_step(m->cpu);
    /* 1 entry 3, 2 recheck 5, 3 INTR 61 + the handler's IRET 24, 4 the WAIT entered again 3, 5 recheck 5,
       6 recheck 5 finds TEST active, 7 the NOP at 10001 */
    ok = t[0] == 3 && t[1] == 5 && t[2] == 61 + 24 && t[3] == 3 && t[4] == 5 && t[5] == 5 && t[6] == 3
         && word(m, STACK_TOP - 6) == 0x0000 && m->pcs[2] == 0x40040 && m->pcs[3] == 0x10000 && m->pcs[6] == 0x10001;
    sprintf(d, "steps %d %d, the INTR + IRET %d (stacked IP %04X), again %d %d %d, then %05X (want 3 5, 85 (0000), "
            "3 5 5, 10001)", t[0], t[1], t[2], word(m, STACK_TOP - 6), t[3], t[4], t[5], m->pcs[6]);
    report("wait_interrupt", ok, d);
    free_machine(m);
}

/* A REPRODUCER, NOT A HARDWARE CLAIM (Astra, Reply 110): a WAIT with a segment prefix, as this core runs it today --
   upstream's rule, which puts IP back one byte, on the WAIT and not on its prefix.  The first step pays the prefix and
   the entry (2 + 3) and leaves IP on the 9Bh byte, so every recheck starts there without the prefix, an INTR between
   rechecks pushes the WAIT's address (not the prefix's, unlike a REP string instruction: rep_iteration), and after
   IRET the WAIT is entered again without it (3, not 2 + 3).  What the real 8086 keeps of a prefix across a WAIT's
   rechecks and an interrupt is NOT established, so this pins the current behaviour and changes nothing: a future
   change to it must change this test on evidence.  The Accent-mini's driver has no prefixed WAIT (CONTRACT.md 4) */
static void t_wait_prefixed(void)
{
    machine *m = new_machine(F_IF | 0x0002);
    char d[400];
    int t[6], i, ok;
    db(m, 2, 0x26, 0x9B); db(m, 1, NOP);        /* 10000 ES: WAIT; 10002 NOP */
    set_vec(m, 0x40, 0x4000, 0x0040); org(m, 0x40040); db(m, 1, IRET);
    i86_set_irq(m->cpu, I86_TEST, 0);
    at(m, 2, I86_INTR, 1);                      /* raised at the first recheck's boundary */
    at(m, 3, I86_INTR, 0);
    for (i = 0; i < 5; i++) t[i] = i86_step(m->cpu);
    i86_set_irq(m->cpu, I86_TEST, 1);
    t[5] = i86_step(m->cpu);
    i86_step(m->cpu);
    /* 1 ES: + the entry 2 + 3 at 10000, 2 a recheck 5 at 10001, 3 INTR 61 + the handler's IRET 24 (pushed IP 0001),
       4 the WAIT entered again at 10001 without its prefix 3, 5 a recheck 5, 6 the recheck that finds TEST active 5,
       7 the NOP at 10002 */
    ok = t[0] == 2 + 3 && t[1] == 5 && t[2] == 61 + 24 && t[3] == 3 && t[4] == 5 && t[5] == 5
         && m->pcs[0] == 0x10000 && m->pcs[1] == 0x10001 && m->pcs[2] == 0x40040 && m->pcs[3] == 0x10001
         && m->pcs[4] == 0x10001 && m->pcs[5] == 0x10001 && m->pcs[6] == 0x10002 && word(m, STACK_TOP - 6) == 0x0001;
    sprintf(d, "CURRENT behaviour, documented (the 8086's prefix retention not established): steps %d %d at %05X "
            "%05X, the INTR + IRET %d (stacked IP %04X), again %d %d %d at %05X, then %05X (as today: 5 5 at 10000 "
            "10001, 85 (0001), 3 5 5 at 10001 -- the prefix not re-executed, then 10002)", t[0], t[1], m->pcs[0],
            m->pcs[1], t[2], word(m, STACK_TOP - 6), t[3], t[4], t[5], m->pcs[3], m->pcs[6]);
    report("wait_prefixed", ok, d);
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
        org(m, 0x10005); db(m, 1, 0xEF);        /* OUT DX,AX: 8 (Intel's 8086, 2-62/PDF 85) */
        for (i = 0; i < 5; i++)
            (k ? t : e)[i] = i86_step(m->cpu);
        free_machine(m);
    }
    ok = e[0] == 13 && e[1] == 18 && e[2] == 51 && e[3] == 24 && e[4] == 8
         && t[0] == 17 && t[1] == 22 && t[2] == 63 && t[3] == 36 && t[4] == 12;
    sprintf(d, "even / odd: MOV AX,[BX] %d/%d, MOVSW %d/%d, INT 21h %d/%d, IRET %d/%d, OUT DX,AX %d/%d (want 13/17, "
            "18/22, 51/63, 24/36, 8/12)", e[0], t[0], e[1], t[1], e[2], t[2], e[3], t[3], e[4], t[4]);
    report("odd_word", ok, d);
}

/* ---- the 8086's own rows, even AND odd addresses (Astra, Reply 106) ---------------------------------------------
   Each test runs its instructions at even addresses and at odd ones and checks both absolute counts: the base (the
   8086's figure, Table 2-21) and the 4 added once per word transfer at an odd address.  An odd-minus-even check
   alone would pass a wrong base (the 8088's rows, 4 more everywhere). */

static int counts_line(char *d, const char *const *names, const int *got, const int *want, int n)
{
    int i, pos = 0, ok = 1;
    for (i = 0; i < n; i++) {
        pos += sprintf(d + pos, "%s%s %d", i ? ", " : "", names[i], got[i]);
        if (got[i] != want[i]) {
            pos += sprintf(d + pos, " (want %d)", want[i]);
            ok = 0;
        }
    }
    return ok;
}

/* PUSH/POP: at an even SP, an odd SP, and an odd SP with an odd memory operand (PUSH/POP [BX]: two transfers) */
static void t_stack_counts(void)
{
    static const char *const names[8] = { "PUSH AX", "PUSH ES", "PUSHF", "PUSH [BX]", "POP [BX]", "POPF", "POP ES",
                                          "POP AX" };
    static const int want[3][8] = {
        { 11, 10, 10, 16 + 5, 17 + 5, 8, 8, 8 },          /* SP even, BX even: the 8086's figures (2-62, 2-63) */
        { 15, 14, 14, 16 + 5 + 4, 17 + 5 + 4, 12, 12, 12 },   /* SP odd: + 4 for the stack transfer */
        { 15, 14, 14, 16 + 5 + 8, 17 + 5 + 8, 12, 12, 12 } }; /* SP and BX odd: + 4 for each of two transfers */
    char d[900];
    int c, i, ok = 1, pos = 0;
    for (c = 0; c < 3; c++) {
        machine *m = new_machine(0x0002);
        i86_regs r;
        int t[8];
        regs(m, &r); r.sp = (uint16_t)(0x1000 - (c > 0)); r.bx = (uint16_t)(0x0010 + (c == 2)); r.ax = 0x1234;
        i86_regs_set(m->cpu, &r);
        db(m, 1, PUSH_AX); db(m, 1, 0x06); db(m, 1, PUSHF);   /* PUSH AX; PUSH ES; PUSHF */
        db(m, 2, 0xFF, 0x37); db(m, 2, 0x8F, 0x07);           /* PUSH [BX]; POP [BX] */
        db(m, 1, POPF); db(m, 1, 0x07); db(m, 1, 0x58);       /* POPF; POP ES; POP AX */
        for (i = 0; i < 8; i++) t[i] = i86_step(m->cpu);
        regs(m, &r);
        pos += sprintf(d + pos, "%s", c == 0 ? "SP even: " : c == 1 ? "; SP odd: " : "; SP and BX odd: ");
        ok &= counts_line(d + pos, names, t, want[c], 8) && r.ax == 0x1234 && r.es == 0x3000
              && r.sp == (uint16_t)(0x1000 - (c > 0));
        pos += (int)strlen(d + pos);
        free_machine(m);
    }
    report("stack_counts", ok, d);
}

/* IN/OUT: the word forms are the byte forms' 10 (imm8) and 8 (DX) on the 8086 (2-55, 2-62); + 4 at an odd port */
static void t_port_counts(void)
{
    static const char *const names[8] = { "IN AL,imm", "IN AX,imm", "OUT imm,AL", "OUT imm,AX", "IN AL,DX",
                                          "IN AX,DX", "OUT DX,AL", "OUT DX,AX" };
    static const int want[2][8] = { { 10, 10, 10, 10, 8, 8, 8, 8 },      /* even ports */
                                    { 10, 14, 10, 14, 8, 12, 8, 12 } };  /* odd ports: the word forms + 4 */
    char d[600];
    int k, i, ok = 1, pos = 0;
    for (k = 0; k < 2; k++) {
        machine *m = new_machine(0x0002);
        i86_regs r;
        int t[8];
        regs(m, &r); r.dx = (uint16_t)(0x3EE + k); i86_regs_set(m->cpu, &r);
        db(m, 2, 0xE4, 0x40 + k); db(m, 2, 0xE5, 0x40 + k); db(m, 2, 0xE6, 0x40 + k); db(m, 2, 0xE7, 0x40 + k);
        db(m, 1, 0xEC); db(m, 1, 0xED); db(m, 1, 0xEE); db(m, 1, 0xEF);
        for (i = 0; i < 8; i++) t[i] = i86_step(m->cpu);
        pos += sprintf(d + pos, "%s", k ? "; odd ports: " : "even ports: ");
        ok &= counts_line(d + pos, names, t, want[k], 8) && m->n_in == 6 && m->n_out == 6;
        pos += (int)strlen(d + pos);
        free_machine(m);
    }
    report("port_counts", ok, d);
}

/* returns, near and far, without and with the stack adjustment (2-64/PDF 87), and the calls (2-52/PDF 75): at an odd
   SP each word popped or pushed adds 4 -- RETF's and CALL far's two */
static void t_return_counts(void)
{
    static const char *const names[6] = { "RET", "RET 4", "RETF", "RETF 2", "CALL near", "CALL far" };
    static const int want[2][6] = { { 8, 12, 18, 17, 19, 28 },           /* SP even */
                                    { 12, 16, 26, 25, 23, 36 } };        /* SP odd: 4 a word transfer */
    static const uint16_t stack[9] = { 0x0001, 0x0004, 0, 0, 0x0005, 0x1000, 0x0008, 0x1000, 0 };
    char d[500];
    int k, i, ok = 1, pos = 0;
    for (k = 0; k < 2; k++) {
        machine *m = new_machine(0x0002);
        i86_regs r;
        int t[6];
        uint16_t sp = (uint16_t)(0x0F00 + k);
        regs(m, &r); r.sp = sp; i86_regs_set(m->cpu, &r);
        for (i = 0; i < 9; i++) {
            m->mem[0x20000 + sp + 2 * i] = (uint8_t)stack[i];
            m->mem[0x20000 + sp + 2 * i + 1] = (uint8_t)(stack[i] >> 8);
        }
        db(m, 1, 0xC3);                         /* 10000 RET -> 0001 */
        db(m, 3, 0xC2, 0x04, 0x00);             /* 10001 RET 4 -> 0004, SP + 2 + 4 */
        db(m, 1, 0xCB);                         /* 10004 RETF -> 1000:0005 */
        db(m, 3, 0xCA, 0x02, 0x00);             /* 10005 RETF 2 -> 1000:0008, SP + 4 + 2 */
        db(m, 3, 0xE8, 0x00, 0x00);             /* 10008 CALL near +0 -> 000B */
        db(m, 5, 0x9A, 0x10, 0x00, 0x00, 0x10); /* 1000B CALL FAR 1000:0010 */
        db(m, 1, NOP);                          /* 10010 */
        for (i = 0; i < 6; i++) t[i] = i86_step(m->cpu);
        i86_step(m->cpu);
        regs(m, &r);
        pos += sprintf(d + pos, "%s", k ? "; SP odd: " : "SP even: ");
        ok &= counts_line(d + pos, names, t, want[k], 6) && m->pcs[4] == 0x10008 && m->pcs[6] == 0x10010
              && r.sp == (uint16_t)(sp + 18 - 6);
        pos += (int)strlen(d + pos);
        free_machine(m);
    }
    report("return_counts", ok, d);
}

/* word memory operands with the 8088's 4 built into MAME's rows: LDS/LES 16 + EA (2-59/PDF 82, two transfers),
   MUL/IMUL/DIV/IDIV m16 124/134/150/171 + EA (Intel's lowest figures; 2-61, 2-55, 2-54, 2-55) -- [BX]: EA 5 */
static void t_word_memory_counts(void)
{
    static const char *const names[6] = { "DIV [BX]", "IDIV [BX]", "MUL [BX]", "IMUL [BX]", "LES SI,[BX]",
                                          "LDS DI,[BX]" };
    static const int want[2][6] = { { 155, 176, 129, 139, 21, 21 },     /* BX even */
                                    { 159, 180, 133, 143, 29, 29 } };   /* BX odd: + 4 a word, LDS/LES two words */
    char d[500];
    int k, i, ok = 1, pos = 0;
    for (k = 0; k < 2; k++) {
        machine *m = new_machine(0x0002);
        i86_regs r;
        int t[6];
        uint32_t a = 0x30010 + (uint32_t)k;
        regs(m, &r); r.bx = (uint16_t)(0x0010 + k); r.ax = 0x1234; r.dx = 0; i86_regs_set(m->cpu, &r);
        m->mem[a] = 0x10; m->mem[a + 1] = 0x00; m->mem[a + 2] = 0x00; m->mem[a + 3] = 0x30;  /* 3000:0010 */
        db(m, 2, 0xF7, 0x37); db(m, 2, 0xF7, 0x3F);   /* DIV word [BX]: 1234h / 10h; IDIV word [BX] */
        db(m, 2, 0xF7, 0x27); db(m, 2, 0xF7, 0x2F);   /* MUL word [BX]; IMUL word [BX] */
        db(m, 2, 0xC4, 0x37); db(m, 2, 0xC5, 0x3F);   /* LES SI,[BX]; LDS DI,[BX] (DS stays 3000h) */
        for (i = 0; i < 6; i++) t[i] = i86_step(m->cpu);
        regs(m, &r);
        pos += sprintf(d + pos, "%s", k ? "; BX odd: " : "BX even: ");
        ok &= counts_line(d + pos, names, t, want[k], 6) && r.si == 0x0010 && r.di == 0x0010 && r.es == 0x3000
              && r.ds == 0x3000 && m->pcs[5] == 0x1000A;
        pos += (int)strlen(d + pos);
        free_machine(m);
    }
    report("word_memory_counts", ok, d);
}

/* ---- after Astra's Reply 110 ----------------------------------------------------------------------------------- */

/* TEST r/m,imm: Intel's register,immediate 5 and memory,immediate 11 + EA (printed 2-67/PDF 90; MAME 4 and 10 + EA,
   the ALU's rows), byte and word, at an even and an odd BX: a word operand at an odd address + 4, a byte never.  The
   rows these share upstream stay: TEST AL/AX,imm (A8/A9) 4 (2-67), CMP r8,imm 4 and CMP mem,imm 10 + EA (2-51/PDF 74).
   [BX]: EA 5 */
static void t_test_imm_counts(void)
{
    static const char *const names[9] = { "TEST BL,imm", "TEST BX,imm", "TEST b[BX],imm", "TEST w[BX],imm",
                                          "TEST AL,imm", "TEST AX,imm", "CMP BL,imm", "CMP b[BX],imm",
                                          "CMP w[BX],imm" };
    static const int want[2][9] = { { 5, 5, 11 + 5, 11 + 5, 4, 4, 4, 10 + 5, 10 + 5 },          /* BX even */
                                    { 5, 5, 11 + 5, 11 + 5 + 4, 4, 4, 4, 10 + 5, 10 + 5 + 4 } }; /* BX odd */
    char d[700];
    int k, i, ok = 1, pos = 0;
    for (k = 0; k < 2; k++) {
        machine *m = new_machine(0x0002);
        i86_regs r;
        int t[9];
        regs(m, &r); r.bx = (uint16_t)(0x0010 + k); r.ax = 0x00F0; i86_regs_set(m->cpu, &r);
        m->mem[0x30010 + k] = 0x0F; m->mem[0x30011 + k] = 0xF0;
        db(m, 3, 0xF6, 0xC3, 0x01);              /* TEST BL,01h */
        db(m, 4, 0xF7, 0xC3, 0x01, 0x00);        /* TEST BX,0001h */
        db(m, 3, 0xF6, 0x07, 0x0F);              /* TEST byte [BX],0Fh */
        db(m, 4, 0xF7, 0x07, 0x00, 0xF0);        /* TEST word [BX],F000h */
        db(m, 2, 0xA8, 0xF0);                    /* TEST AL,F0h */
        db(m, 3, 0xA9, 0xF0, 0x00);              /* TEST AX,00F0h */
        db(m, 3, 0x80, 0xFB, 0x10);              /* CMP BL,10h */
        db(m, 3, 0x80, 0x3F, 0x0F);              /* CMP byte [BX],0Fh */
        db(m, 4, 0x81, 0x3F, 0x0F, 0xF0);        /* CMP word [BX],F00Fh */
        db(m, 1, NOP);
        for (i = 0; i < 9; i++) t[i] = i86_step(m->cpu);
        regs(m, &r);
        pos += sprintf(d + pos, "%s", k ? "; BX odd: " : "BX even: ");
        ok &= counts_line(d + pos, names, t, want[k], 9) && r.ip == 0x001D && r.bx == (uint16_t)(0x0010 + k)
              && m->mem[0x30010 + k] == 0x0F && m->mem[0x30011 + k] == 0xF0;   /* TEST and CMP write nothing */
        pos += (int)strlen(d + pos);
        free_machine(m);
    }
    report("test_imm_counts", ok, d);
}

/* LOOPNE: "19 or 5" (printed 2-60/PDF 83; MAME LOOP's 17 taken), not taken both ways -- CX run down to 0, and ZF set
   with CX left nonzero; LOOP 17/5 and LOOPE 18/6 (same page) stay */
static void t_loopne_counts(void)
{
    static const char *const names[9] = { "LOOPNE taken", "LOOPNE taken", "LOOPNE CX=0", "LOOPNE ZF=1",
                                          "LOOP taken", "LOOP CX=0", "LOOPE taken", "LOOPE CX=0", "LOOPE ZF=0" };
    static const int want[9] = { 19, 19, 5, 5, 17, 5, 18, 6, 6 };
    machine *m = new_machine(0x0002);
    i86_regs r;
    char d[500];
    int t[9], ok;
    regs(m, &r); r.cx = 3; i86_regs_set(m->cpu, &r);
    db(m, 2, 0xE0, 0xFE);                        /* 10000 LOOPNE 10000 */
    db(m, 2, 0xE0, 0xFE);                        /* 10002 LOOPNE 10002 */
    db(m, 2, 0xE2, 0xFE);                        /* 10004 LOOP 10004 */
    db(m, 2, 0xE1, 0xFE);                        /* 10006 LOOPE 10006 */
    db(m, 2, 0xE1, 0xFE);                        /* 10008 LOOPE 10008 */
    t[0] = i86_step(m->cpu); t[1] = i86_step(m->cpu); t[2] = i86_step(m->cpu);   /* CX 2, 1, 0 */
    ok = m->pcs[1] == 0x10000 && m->pcs[2] == 0x10000;
    regs(m, &r); ok &= r.cx == 0 && r.ip == 2; r.cx = 5; r.flags = 0x0042; i86_regs_set(m->cpu, &r);
    t[3] = i86_step(m->cpu);                     /* ZF set: falls through, CX 4 */
    regs(m, &r); ok &= r.cx == 4 && r.ip == 4; r.cx = 2; r.flags = 0x0002; i86_regs_set(m->cpu, &r);
    t[4] = i86_step(m->cpu); t[5] = i86_step(m->cpu);
    regs(m, &r); ok &= r.cx == 0 && r.ip == 6; r.cx = 2; r.flags = 0x0042; i86_regs_set(m->cpu, &r);
    t[6] = i86_step(m->cpu); t[7] = i86_step(m->cpu);
    regs(m, &r); ok &= r.cx == 0 && r.ip == 8; r.cx = 2; r.flags = 0x0002; i86_regs_set(m->cpu, &r);
    t[8] = i86_step(m->cpu);                     /* ZF clear: falls through */
    regs(m, &r); ok &= r.cx == 1 && r.ip == 10;
    ok &= counts_line(d, names, t, want, 9);
    report("loopne_counts", ok, d);
    free_machine(m);
}

/* MUL/IMUL of AL/AX by BL/BX (ModRM C3 + /n) or by [BX] (07 + /n), one machine a case; FLAGS start with CF and OF the
   opposite of the case's expected value, so an instruction that left them alone fails too */
static void mul_case(int op, int ext, int mem, uint16_t a, uint16_t b, int expect, i86_regs *out)
{
    machine *m = new_machine(expect ? 0x0002 : (0x0002 | F_OF | 0x0001));
    i86_regs r;
    regs(m, &r); r.ax = a; r.dx = 0x5A5A; r.bx = mem ? 0x0010 : b; i86_regs_set(m->cpu, &r);
    m->mem[0x30010] = (uint8_t)b; m->mem[0x30011] = (uint8_t)(b >> 8);
    db(m, 2, op, mem ? (ext << 3) | 0x07 : 0xC0 | (ext << 3) | 3);
    i86_step(m->cpu);
    regs(m, out);
    free_machine(m);
}

/* IMUL's CF and OF (Intel, printed 2-37/PDF 60): set exactly when the upper half is not the sign extension of the
   lower, i.e. the signed product does not fit the source's width.  Astra's probe (Reply 110) -- -1 x 1, the largest
   positive x 2, the most negative x 1, the most negative x -1, 2 x 3 -- and around the edges: the largest positive x 1,
   the most negative reached exactly from below (fits), one past it (-129 / -32769: negative overflow), a larger
   negative overflow.  Register and memory operands; the product (AX, DX) checked with the flags.  AF, PF, SF and ZF
   are undefined after IMUL (the same page) and not asserted */
static void imul_flags(int word)
{
    static const int16_t a8[9] = { -1, 127, -128, -128, 2, 127, -64, -43, 100 };
    static const int16_t b8[9] = { 1, 2, 1, -1, 3, 1, 2, 3, -2 };
    static const int32_t a16[9] = { -1, 32767, -32768, -32768, 2, 32767, 16384, -10923, 200 };
    static const int32_t b16[9] = { 1, 2, 1, -1, 3, 1, -2, 3, -200 };
    static const int want[9] = { 0, 1, 0, 1, 0, 0, 0, 1, 1 };  /* products: -1, 254, -128, 128, 6, 127, -128, -129, -200
                                                                 (word: ..., 65534, ..., 32768, ..., -32768, -32769,
                                                                 -40000) */
    char d[2000];
    int k, mem, ok = 1, pos = 0;
    for (mem = 0; mem < 2; mem++)
        for (k = 0; k < 9; k++) {
            i86_regs r;
            int32_t a = word ? a16[k] : a8[k], b = word ? b16[k] : b8[k], p = a * b;
            int cf, of, prod_ok;
            mul_case(word ? 0xF7 : 0xF6, 5, mem, (uint16_t)a, (uint16_t)b, want[k], &r);
            cf = r.flags & 0x0001 ? 1 : 0;
            of = r.flags & F_OF ? 1 : 0;
            prod_ok = word ? r.ax == (uint16_t)p && r.dx == (uint16_t)((uint32_t)p >> 16)
                           : r.ax == (uint16_t)p && r.dx == 0x5A5A;
            if (cf != want[k] || of != want[k] || !prod_ok) {
                ok = 0;
                pos += sprintf(d + pos, "%s %ld x %ld: CF %d OF %d (want %d), DX:AX %04X:%04X; ", mem ? "[BX]" : "reg",
                               (long)a, (long)b, cf, of, want[k], r.dx, r.ax);
            }
        }
    sprintf(d + pos, "%d cases x (reg, [BX]): CF = OF = 1 exactly when the signed product does not fit %s",
            9, word ? "16 bits" : "8 bits");
    report(word ? "imul_flags_word" : "imul_flags_byte", ok, d);
}
static void t_imul_flags_byte(void) { imul_flags(0); }
static void t_imul_flags_word(void) { imul_flags(1); }

/* MUL, not changed: CF = OF = 1 exactly when the upper half (AH, DX) is nonzero (printed 2-37/PDF 60) */
static void t_mul_flags(void)
{
    static const uint16_t a[2][5] = { { 0xFF, 0x80, 0x10, 2, 0xFF }, { 0xFFFF, 0x8000, 0x0100, 2, 0xFFFF } };
    static const uint16_t b[2][5] = { { 0x01, 0x02, 0x10, 3, 0xFF }, { 0x0001, 0x0002, 0x0100, 3, 0xFFFF } };
    static const int want[5] = { 0, 1, 1, 0, 1 };
    char d[2000];
    int w, k, mem, ok = 1, pos = 0;
    for (w = 0; w < 2; w++)
        for (mem = 0; mem < 2; mem++)
            for (k = 0; k < 5; k++) {
                i86_regs r;
                uint32_t p = (uint32_t)a[w][k] * b[w][k];
                int cf, of, prod_ok;
                mul_case(w ? 0xF7 : 0xF6, 4, mem, a[w][k], b[w][k], want[k], &r);
                cf = r.flags & 0x0001 ? 1 : 0;
                of = r.flags & F_OF ? 1 : 0;
                prod_ok = w ? r.ax == (uint16_t)p && r.dx == (uint16_t)(p >> 16)
                            : r.ax == (uint16_t)p && r.dx == 0x5A5A;
                if (cf != want[k] || of != want[k] || !prod_ok) {
                    ok = 0;
                    pos += sprintf(d + pos, "MUL %s %s %04X x %04X: CF %d OF %d (want %d), DX:AX %04X:%04X; ",
                                   w ? "r/m16" : "r/m8", mem ? "[BX]" : "reg", a[w][k], b[w][k], cf, of, want[k],
                                   r.dx, r.ax);
                }
            }
    sprintf(d + pos, "5 cases x (byte, word) x (reg, [BX]): CF = OF = the upper half nonzero");
    report("mul_flags", ok, d);
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
    t_wait_interrupt();
    t_wait_prefixed();
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
    t_stack_counts();
    t_port_counts();
    t_return_counts();
    t_word_memory_counts();
    t_test_imm_counts();
    t_loopne_counts();
    t_imul_flags_byte();
    t_imul_flags_word();
    t_mul_flags();
    t_forward_progress();
    t_step_cost_floor();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
