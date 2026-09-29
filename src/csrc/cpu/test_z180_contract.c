/* test_z180_contract.c -- CONTRACT.md's behaviour of a cpu.h Z180 core on the CORRECTED path (z180_step / z180_run).
 *
 * Each test is a few instructions in a flat 64K (after reset the MMU maps logical to physical one to one), driven
 * through cpu.h only, and checks one clause:
 *   zero_budget   z180_run(0) returns 0 and changes nothing (2)
 *   nmi_per_step  an NMI raised at a boundary is taken at the next step's acceptance; IFF1 goes to IFF2 (1, 4)
 *   ei_shadow     after EI the next instruction runs before a pending maskable interrupt (5)
 *   halt_edge     an interrupt raised during a HALT slot's boundary is accepted at the next step; a slot is 3 T (6)
 *   slp_wake      SLP with IEF1 = 0 ends on an enabled PRT request and continues after SLP without vectoring;
 *                 the same program with HALT stays halted -- the rule's own control (6)
 *   iostop        with ICR.IOSTOP the PRT does not count; without it, it does -- the control (6)
 *   burst_dma     a 40-byte burst is three steps of 16, 16 and 8 bytes, each with its boundary and no
 *                 instruction, then the program goes on (1)
 *   reset         reset zeroes cycles and steps, drops a pending NMI edge, and re-samples a held INT0 (9)
 *   two_cores     two instances interleaved step by step behave as each alone (9)
 *
 *   build (MAME): gcc -c test_z180_contract.c; g++ ... z180_mame.cpp z180_asci.cpp   (build_board.py does it)
 * Exit status 0 when every test passes.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

/* ---- a machine: 64K of RAM, a recorder at each boundary ------------------------------------------------------ */
typedef struct {
    uint8_t mem[65536];
    int pos;                                   /* the assembler's cursor */
    uint32_t pcs[512];
    int n_pcs;
    z180 *cpu;
    int raise_at_step, raise_line;             /* raise this line in the boundary of that step (0 = never) */
    long raised_step;
} machine;

static uint8_t rd(void *ctx, uint32_t a) { return ((machine *)ctx)->mem[a & 0xFFFF]; }
static void wr(void *ctx, uint32_t a, uint8_t v) { ((machine *)ctx)->mem[a & 0xFFFF] = v; }
static uint8_t in(void *ctx, uint16_t p) { (void)ctx; (void)p; return 0xFF; }
static void out(void *ctx, uint16_t p, uint8_t v) { (void)ctx; (void)p; (void)v; }

static void boundary(void *ctx, uint32_t pc)
{
    machine *m = (machine *)ctx;
    if (m->n_pcs < (int)(sizeof m->pcs / sizeof m->pcs[0]))
        m->pcs[m->n_pcs++] = pc;
    if (m->raise_at_step && (long)z180_steps(m->cpu) == m->raise_at_step) {
        z180_set_irq(m->cpu, m->raise_line, 1);
        m->raised_step = (long)z180_steps(m->cpu);
    }
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
    m->cpu = z180_create(&bus, 6144000.0);
    return m;
}

static void free_machine(machine *m)
{
    z180_destroy(m->cpu);
    free(m);
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
static void out0(machine *m, int port, int value) { db(m, 5, 0x3E, value, 0xED, 0x39, port); }   /* LD A,n; OUT0 (p),A */
static void ld_sp(machine *m, int a) { db(m, 3, 0x31, a & 0xFF, a >> 8); }
static void st_a(machine *m, int a) { db(m, 3, 0x32, a & 0xFF, a >> 8); }                        /* LD (nn),A */

static int first_step_at(const machine *m, uint32_t pc)
{
    int i;
    for (i = 0; i < m->n_pcs; i++)
        if (m->pcs[i] == pc)
            return i + 1;                       /* steps count from 1 */
    return -1;
}

static int failures;
static void report(const char *name, int ok, const char *detail)
{
    printf("%-4s %-13s %s\n", ok ? "ok" : "FAIL", name, detail);
    if (!ok)
        failures++;
}

/* ---- the tests ------------------------------------------------------------------------------------------------ */
static void t_zero_budget(void)
{
    machine *m = new_machine();
    char d[160];
    uint64_t done, c0 = z180_cycles(m->cpu), s0 = z180_steps(m->cpu);
    done = z180_run(m->cpu, 0);
    sprintf(d, "run(0) = %llu, cycles %llu -> %llu, steps %llu -> %llu", (unsigned long long)done,
            (unsigned long long)c0, (unsigned long long)z180_cycles(m->cpu), (unsigned long long)s0,
            (unsigned long long)z180_steps(m->cpu));
    report("zero_budget", done == 0 && z180_cycles(m->cpu) == c0 && z180_steps(m->cpu) == s0 && m->n_pcs == 0, d);
    free_machine(m);
}

static void t_nmi_per_step(void)
{
    machine *m = new_machine();
    z180_regs r;
    char d[160];
    int taken, i;
    org(m, 0);
    ld_sp(m, 0x8000);
    db(m, 1, 0xFB);                             /* EI: IFF1 = IFF2 = 1 */
    for (i = 0; i < 20; i++)
        db(m, 1, 0x00);
    org(m, 0x66);
    db(m, 1, 0x76);                             /* HALT */
    m->raise_at_step = 6;
    m->raise_line = Z180_NMI;
    for (i = 0; i < 12; i++)
        z180_step(m->cpu);
    z180_regs_get(m->cpu, &r);
    taken = first_step_at(m, 0x66);
    sprintf(d, "raised at step %ld's boundary, vector's first boundary at step %d; IFF1 %d IFF2 %d", m->raised_step,
            taken, r.iff1, r.iff2);
    report("nmi_per_step", taken == m->raised_step + 1 && r.iff1 == 0 && r.iff2 == 1, d);
    free_machine(m);
}

static void t_ei_shadow(void)
{
    machine *m = new_machine();
    char d[160];
    int i;
    org(m, 0);
    ld_sp(m, 0x8000);                           /* 0000 */
    db(m, 2, 0xED, 0x56);                       /* 0003 IM 1 */
    db(m, 1, 0xFB);                             /* 0005 EI */
    db(m, 4, 0x00, 0x00, 0x00, 0x00);           /* 0006 NOPs */
    org(m, 0x38);
    db(m, 1, 0x76);
    z180_set_irq(m->cpu, Z180_INT0, 1);         /* held from the start (ITC.ITE0 is set at reset) */
    for (i = 0; i < 8; i++)
        z180_step(m->cpu);
    sprintf(d, "boundaries %04X %04X %04X %04X %04X (want 0000 0003 0005 0006 0038)", m->pcs[0], m->pcs[1], m->pcs[2],
            m->pcs[3], m->pcs[4]);
    report("ei_shadow", m->pcs[2] == 0x05 && m->pcs[3] == 0x06 && m->pcs[4] == 0x38, d);
    free_machine(m);
}

static void t_halt_edge(void)
{
    machine *m = new_machine();
    char d[200];
    int i, t_slot = -1, taken;
    org(m, 0);
    ld_sp(m, 0x8000);
    db(m, 2, 0xED, 0x56);                       /* IM 1 */
    db(m, 1, 0xFB);                             /* EI */
    db(m, 1, 0x76);                             /* 0006 HALT */
    org(m, 0x38);
    db(m, 1, 0x76);
    m->raise_at_step = 8;                       /* a HALT slot's boundary (the HALT itself is step 5) */
    m->raise_line = Z180_INT0;
    for (i = 1; i <= 12; i++) {
        int t = z180_step(m->cpu);
        if (i == 8)
            t_slot = t;
    }
    taken = first_step_at(m, 0x38);
    sprintf(d, "boundary pc at step 8: %04X, that slot %d T; INT0 raised there, vector's first boundary at step %d",
            m->pcs[7], t_slot, taken);
    report("halt_edge", m->pcs[7] == 0x06 && t_slot == 3 && taken == 9, d);
    free_machine(m);
}

/* DI; PRT0 enabled with its interrupt (TIE0) and reload 0010h; then SLP (or HALT); then A = 42h stored at 9000h */
static machine *slp_program(int use_slp, int reload)
{
    machine *m = new_machine();
    org(m, 0);
    ld_sp(m, 0x8000);
    db(m, 1, 0xF3);                             /* DI */
    out0(m, 0x0E, reload & 0xFF);               /* RLDR0 */
    out0(m, 0x0F, reload >> 8);
    out0(m, 0x10, 0x11);                        /* TCR: TIE0 | TDE0 */
    if (use_slp)
        db(m, 2, 0xED, 0x76);                   /* SLP */
    else
        db(m, 2, 0x76, 0x00);                   /* HALT (NOP after, so the addresses match) */
    db(m, 2, 0x3E, 0x42);                       /* LD A,42h */
    st_a(m, 0x9000);
    db(m, 1, 0x76);                             /* HALT */
    return m;
}

static void t_slp_wake(void)
{
    machine *s = slp_program(1, 0x0010), *h = slp_program(0, 0x0010);
    z180_regs rs, rh;
    char d[200];
    z180_run(s->cpu, 20000);
    z180_run(h->cpu, 20000);
    z180_regs_get(s->cpu, &rs);
    z180_regs_get(h->cpu, &rh);
    sprintf(d, "SLP: (9000h) = %02X, SP %04X, IFF1 %d; HALT control: (9000h) = %02X, halted at %04X", s->mem[0x9000],
            rs.sp, rs.iff1, h->mem[0x9000], rh.pc);
    report("slp_wake", s->mem[0x9000] == 0x42 && rs.sp == 0x8000 && rs.iff1 == 0 && h->mem[0x9000] == 0 && rh.halted, d);
    free_machine(s);
    free_machine(h);
}

static machine *iostop_program(int iostop)
{
    machine *m = new_machine();
    org(m, 0);
    out0(m, 0x3F, iostop ? 0x20 : 0x00);        /* ICR */
    out0(m, 0x0E, 0x00);                        /* RLDR0 = 1000h */
    out0(m, 0x0F, 0x10);
    out0(m, 0x10, 0x01);                        /* TCR: TDE0, no interrupt */
    db(m, 4, 0x06, 0x00, 0x10, 0xFE);           /* LD B,0; DJNZ $ (256 times) */
    db(m, 3, 0xED, 0x38, 0x0C);                 /* IN0 A,(TMDR0L) */
    st_a(m, 0x9000);
    db(m, 3, 0xED, 0x38, 0x0D);                 /* IN0 A,(TMDR0H) */
    st_a(m, 0x9001);
    db(m, 1, 0x76);
    return m;
}

static void t_iostop(void)
{
    machine *s = iostop_program(1), *c = iostop_program(0);
    char d[160];
    unsigned vs, vc;
    z180_run(s->cpu, 40000);
    z180_run(c->cpu, 40000);
    vs = (unsigned)(s->mem[0x9001] << 8 | s->mem[0x9000]);
    vc = (unsigned)(c->mem[0x9001] << 8 | c->mem[0x9000]);
    sprintf(d, "TMDR0 after the loop: %04X with IOSTOP (want 1000), %04X without (the control: below 1000)", vs, vc);
    report("iostop", vs == 0x1000 && vc < 0x1000, d);
    free_machine(s);
    free_machine(c);
}

static void t_burst_dma(void)
{
    machine *m = new_machine();
    char d[240];
    int i, n = 0, t[26], ok_copy = 1;
    org(m, 0);
    out0(m, 0x20, 0x00); out0(m, 0x21, 0x10); out0(m, 0x22, 0x00);   /* SAR0 = 01000h */
    out0(m, 0x23, 0x00); out0(m, 0x24, 0x20); out0(m, 0x25, 0x00);   /* DAR0 = 02000h */
    out0(m, 0x26, 40); out0(m, 0x27, 0x00);                          /* BCR0 = 40 */
    out0(m, 0x31, 0x02);                                             /* DMODE: memory+1 -> memory+1, burst */
    out0(m, 0x30, 0x40);                                             /* DSTAT: DE0 (DWE0 = 0) -> DME */
    db(m, 2, 0x00, 0x76);                                            /* 0032 NOP; HALT */
    for (i = 0; i < 40; i++)
        m->mem[0x1000 + i] = (uint8_t)(0xA0 + i);
    for (i = 0; i < 26; i++)                                         /* each out0 is two steps: the DSTAT write */
        t[n++] = z180_step(m->cpu);                                  /* is step 20 (OUT0 at 002Fh) */
    for (i = 0; i < 40; i++)
        if (m->mem[0x2000 + i] != (uint8_t)(0xA0 + i))
            ok_copy = 0;
    /* DCNTL resets to F0h: 3 memory wait states, so a memory-to-memory byte is 6 + 2 x 3 = 12 T; steps 21-23 are
       the chunks (16, 16, 8 bytes), step 24 the NOP -- all four with the boundary at 0032h */
    sprintf(d, "step 20 pc %04X; steps 21-24: pc %04X %04X %04X %04X, T %d %d %d %d (want 002F; 0032 x4, "
            "192 192 96 then the NOP's); 40 bytes copied: %s", m->pcs[19], m->pcs[20], m->pcs[21], m->pcs[22],
            m->pcs[23], t[20], t[21], t[22], t[23], ok_copy ? "yes" : "no");
    report("burst_dma", m->pcs[19] == 0x2F && m->pcs[20] == 0x32 && m->pcs[21] == 0x32 && m->pcs[22] == 0x32
                        && m->pcs[23] == 0x32 && t[20] == 192 && t[21] == 192 && t[22] == 96 && ok_copy, d);
    free_machine(m);
}

static void t_reset(void)
{
    machine *m = new_machine();
    char d[200];
    int i, nmi_seen, int_seen;
    org(m, 0);
    ld_sp(m, 0x8000);
    db(m, 2, 0xED, 0x56);                       /* IM 1 */
    db(m, 1, 0xFB);                             /* EI */
    db(m, 4, 0x00, 0x00, 0x00, 0x00);
    org(m, 0x38);
    db(m, 1, 0x76);
    org(m, 0x66);
    db(m, 1, 0x76);
    for (i = 0; i < 3; i++)
        z180_step(m->cpu);
    z180_set_irq(m->cpu, Z180_NMI, 1);          /* an edge, pending ... */
    z180_set_irq(m->cpu, Z180_INT0, 1);         /* ... and a level, both held over the reset */
    z180_reset(m->cpu);
    {
        int zeroed = z180_cycles(m->cpu) == 0 && z180_steps(m->cpu) == 0;
        m->n_pcs = 0;
        for (i = 0; i < 10; i++)
            z180_step(m->cpu);
        nmi_seen = first_step_at(m, 0x66);
        int_seen = first_step_at(m, 0x38);
        sprintf(d, "counts zeroed: %s; after reset the NMI vector %s, the held INT0 taken at step %d (want 5)",
                zeroed ? "yes" : "no", nmi_seen < 0 ? "not entered" : "ENTERED", int_seen);
        report("reset", zeroed && nmi_seen < 0 && int_seen == 5, d);
    }
    free_machine(m);
}

typedef struct { uint32_t pc; uint64_t cycles; int t; } rec;

static void t_two_cores(void)
{
    enum { N = 3000 };
    static rec alone[2][N], mixed[2][N];
    machine *a, *b;
    char d[160];
    int i, k, same = 1;
    for (k = 0; k < 2; k++) {                   /* each alone (two different programs: PRT + SLP, a delay loop) */
        machine *m = k ? iostop_program(0) : slp_program(1, 0x0010);
        for (i = 0; i < N; i++) {
            alone[k][i].t = z180_step(m->cpu);
            alone[k][i].pc = z180_pc(m->cpu);
            alone[k][i].cycles = z180_cycles(m->cpu);
        }
        free_machine(m);
    }
    a = slp_program(1, 0x0010);                 /* both at once, interleaved step by step */
    b = iostop_program(0);
    for (i = 0; i < N; i++) {
        mixed[0][i].t = z180_step(a->cpu);
        mixed[0][i].pc = z180_pc(a->cpu);
        mixed[0][i].cycles = z180_cycles(a->cpu);
        mixed[1][i].t = z180_step(b->cpu);
        mixed[1][i].pc = z180_pc(b->cpu);
        mixed[1][i].cycles = z180_cycles(b->cpu);
    }
    for (k = 0; k < 2; k++)
        for (i = 0; i < N; i++)
            if (alone[k][i].t != mixed[k][i].t || alone[k][i].pc != mixed[k][i].pc
                || alone[k][i].cycles != mixed[k][i].cycles)
                same = 0;
    sprintf(d, "%d steps each: interleaved %s alone; the two programs %s", N, same ? "equals" : "DIFFERS FROM",
            memcmp(alone[0], alone[1], sizeof alone[0]) ? "differ (as they should)" : "are the same (bad test)");
    report("two_cores", same && memcmp(alone[0], alone[1], sizeof alone[0]) != 0, d);
    free_machine(a);
    free_machine(b);
}

int main(void)
{
    t_zero_budget();
    t_nmi_per_step();
    t_ei_shadow();
    t_halt_edge();
    t_slp_wake();
    t_iostop();
    t_burst_dma();
    t_reset();
    t_two_cores();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
