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
    int ack[4];                                /* the interrupt acknowledge's bytes, by index */
    int ack_idx[8], n_ack;                     /* the indices asked for, in order */
    uint32_t wr_addr[16], wr_pc[16];           /* memory writes at 8000h and above: address, z180_pc() then */
    int n_wr;
    int rx_on;                                 /* serial_rx answers 55h on channel 0 */
    int log_reads;                             /* record every read address (fetches included) while set */
    uint32_t rd_addr[32];
    int n_rd;
} machine;

static uint8_t rd(void *ctx, uint32_t a)
{
    machine *m = (machine *)ctx;
    if (m->log_reads && m->n_rd < 32)
        m->rd_addr[m->n_rd++] = a & 0xFFFF;
    return m->mem[a & 0xFFFF];
}
static void wr(void *ctx, uint32_t a, uint8_t v)
{
    machine *m = (machine *)ctx;
    m->mem[a & 0xFFFF] = v;
    if ((a & 0xFFFF) >= 0x8000 && (a & 0xFFFF) < 0x9000 && m->n_wr < 16) {   /* the stack region */
        m->wr_addr[m->n_wr] = a & 0xFFFF;
        m->wr_pc[m->n_wr++] = z180_pc(m->cpu);
    }
}
static int ack(void *ctx, int line, int n)
{
    machine *m = (machine *)ctx;
    (void)line;
    if (m->n_ack < 8)
        m->ack_idx[m->n_ack++] = n;
    return n < 4 ? m->ack[n] : -1;
}
static int serial_rx(void *ctx, int channel) { return channel == 0 && ((machine *)ctx)->rx_on ? 0x55 : -1; }
static int serial_pin(void *ctx, int pin) { (void)ctx; return pin == Z180_PIN_DCD0; }   /* carrier present */
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
    bus.irq_ack = ack;
    bus.serial_rx = serial_rx;
    bus.serial_pin = serial_pin;
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

/* IM0 with INT0 held: LD SP,9000h; IM 0; EI; NOPs from 0007h.  Step 4 is the NOP in EI's shadow; step 5 accepts
   at A and runs the acknowledged instruction at E; step 6 is whatever comes next. */
static machine *im0_program(int b0, int b1, int b2)
{
    machine *m = new_machine();
    int i;
    org(m, 0);
    ld_sp(m, 0x9000);                           /* 0000 */
    db(m, 2, 0xED, 0x46);                       /* 0003 IM 0 */
    db(m, 1, 0xFB);                             /* 0005 EI */
    for (i = 0; i < 8; i++)
        db(m, 1, 0x00);                         /* 0006.. NOPs */
    m->ack[0] = b0;
    m->ack[1] = b1;
    m->ack[2] = b2;
    z180_set_irq(m->cpu, Z180_INT0, 1);
    return m;
}

static void t_im0(void)
{
    static const struct { const char *name; int b0, b1, b2; int acks; uint32_t pc_after, sp_after; int t; } cases[] = {
        {"im0_nop", 0x00, 0, 0, 1, 0x0007, 0x9000, 5},     /* nothing pushed, nothing jumped: on at 0007h */
        {"im0_rst", 0xC7, 0, 0, 1, 0x0000, 0x8FFE, 19},     /* RST 0: pushes 0007h, to 0000h */
        {"im0_call", 0xCD, 0x34, 0x12, 3, 0x1234, 0x8FFE, 24},   /* CALL 1234h: three acknowledge bytes */
    };
    int k;
    for (k = 0; k < 3; k++) {
        machine *m = im0_program(cases[k].b0, cases[k].b1, cases[k].b2);
        z180_regs r;
        char d[260];
        int i, acks_ok = m->n_ack == 0, pushed_ok, t5 = 0, ok;
        for (i = 1; i <= 5; i++) {
            int t = z180_step(m->cpu);
            if (i == 4)
                acks_ok = m->n_ack == 0;        /* nothing acknowledged before the accepting step */
            if (i == 5)
                t5 = t;
        }
        z180_regs_get(m->cpu, &r);
        for (i = 0; i < m->n_ack; i++)
            if (m->ack_idx[i] != i)
                acks_ok = 0;                    /* the index restarts at 0 and counts up */
        acks_ok = acks_ok && m->n_ack == cases[k].acks;
        pushed_ok = cases[k].sp_after == 0x9000 ? m->n_wr == 0
                    : (m->n_wr == 2 && m->mem[0x8FFE] == 0x07 && m->mem[0x8FFF] == 0x00);
        z180_step(m->cpu);                      /* step 6: its boundary is the next instruction */
        ok = acks_ok && pushed_ok && r.pc == cases[k].pc_after && r.sp == cases[k].sp_after && r.iff1 == 0
             && m->pcs[5] == cases[k].pc_after && m->pcs[4] == 0x0007 && t5 == cases[k].t;
        sprintf(d, "acknowledge bytes read %d (indices from 0: %s), PC %04X SP %04X, stack writes %d "
                "(pushed %02X%02X), IFF1 %d, step 5 took %d T; step 5 boundary %04X, step 6 boundary %04X",
                m->n_ack, acks_ok ? "yes" : "no", r.pc, r.sp, m->n_wr, m->mem[0x8FFF], m->mem[0x8FFE], r.iff1, t5,
                m->pcs[4], m->pcs[5]);
        report(cases[k].name, ok, d);
        free_machine(m);
    }
}

/* Cycle-stealing DMA0, memory to memory, 32 bytes waiting; then SLP (or HALT). */
static machine *dma_sleep_program(int use_slp)
{
    machine *m = new_machine();
    int i;
    org(m, 0);
    out0(m, 0x20, 0x00); out0(m, 0x21, 0x10); out0(m, 0x22, 0x00);   /* SAR0 = 01000h */
    out0(m, 0x23, 0x00); out0(m, 0x24, 0x20); out0(m, 0x25, 0x00);   /* DAR0 = 02000h */
    out0(m, 0x26, 32); out0(m, 0x27, 0x00);                          /* BCR0 = 32 */
    out0(m, 0x31, 0x00);                                             /* DMODE: memory+1 -> memory+1, cycle steal */
    out0(m, 0x30, 0x40);                                             /* DSTAT: DE0 -> DME; step 20 */
    if (use_slp)
        db(m, 2, 0xED, 0x76);                                        /* 0032 SLP: step 21 */
    else
        db(m, 2, 0x76, 0x00);                                        /* 0032 HALT */
    for (i = 0; i < 32; i++)
        m->mem[0x1000 + i] = (uint8_t)(0xB0 + i);
    return m;
}

static int dma_copied(const machine *m)
{
    int i, n = 0;
    for (i = 0; i < 32; i++)
        n += m->mem[0x2000 + i] == (uint8_t)(0xB0 + i);
    return n;
}

static void t_sleep_dma(void)
{
    machine *s = dma_sleep_program(1), *h = dma_sleep_program(0), *n = dma_sleep_program(0);
    char d[240];
    int i, s20, s21, h_after, n_before, n_after;
    for (i = 0; i < 20; i++)
        z180_step(s->cpu);
    s20 = dma_copied(s);                        /* the DSTAT write's own step moves one byte */
    z180_step(s->cpu);
    s21 = dma_copied(s);                        /* the SLP step: none */
    for (i = 0; i < 40; i++)
        z180_step(s->cpu);
    for (i = 0; i < 60; i++)
        z180_step(h->cpu);
    h_after = dma_copied(h);                    /* the control: HALT leaves the DMAC running */
    sprintf(d, "SLP: %d byte(s) after the DSTAT write, %d after the SLP step, %d after 40 sleep slots; "
            "HALT control: %d of 32", s20, s21, dma_copied(s), h_after);
    report("sleep_dma", s20 == 1 && s21 == 1 && dma_copied(s) == 1 && h_after == 32, d);

    /* an NMI wakes a HALT and disables DMA (DME cleared): the copy stops there */
    for (i = 0; i < 24; i++)
        z180_step(n->cpu);
    n_before = dma_copied(n);
    z180_set_irq(n->cpu, Z180_NMI, 1);
    for (i = 0; i < 30; i++)
        z180_step(n->cpu);
    n_after = dma_copied(n);
    sprintf(d, "HALT with DMA running: %d bytes when NMI raised, %d after 30 more steps (want one more at most: "
            "the raising step's own transfer)", n_before, n_after);
    report("nmi_stops_dma", n_before > 1 && n_before < 32 && n_after <= n_before + 1, d);
    free_machine(s);
    free_machine(h);
    free_machine(n);
}

static void t_accept_pc(void)
{
    machine *a = new_machine(), *h = new_machine();
    char d[240];
    int i;
    org(a, 0);                                  /* a NOP at 0000h, then the NMI: the interrupted instruction is 0001h */
    db(a, 4, 0x00, 0x00, 0x00, 0x00);
    org(a, 0x66);
    db(a, 1, 0x76);
    z180_step(a->cpu);
    z180_set_irq(a->cpu, Z180_NMI, 1);
    z180_step(a->cpu);
    org(h, 0);                                  /* HALT at 0003h: interrupted there, resuming at 0004h */
    ld_sp(h, 0x9000);
    db(h, 1, 0x76);
    org(h, 0x66);
    db(h, 1, 0x76);
    for (i = 0; i < 4; i++)
        z180_step(h->cpu);
    z180_set_irq(h->cpu, Z180_NMI, 1);
    z180_step(h->cpu);
    /* the reset SP is 0000h: the first NMI pushes to FFFEh-FFFFh, outside the recorded region -- read it back */
    sprintf(d, "after a NOP: pushed %02X%02X; from HALT at 0003h: stack writes %d seeing z180_pc %04X %04X, pushed "
            "%02X%02X", a->mem[0xFFFF], a->mem[0xFFFE], h->n_wr, h->n_wr > 0 ? h->wr_pc[0] : 0,
            h->n_wr > 1 ? h->wr_pc[1] : 0, h->mem[0x8FFF], h->mem[0x8FFE]);
    report("accept_pc", a->mem[0xFFFF] == 0x00 && a->mem[0xFFFE] == 0x01 && h->n_wr == 2 && h->wr_pc[0] == 0x0004
                        && h->wr_pc[1] == 0x0004 && h->mem[0x8FFF] == 0x00 && h->mem[0x8FFE] == 0x04, d);
    free_machine(a);
    free_machine(h);
}

/* ASCI0 error flags: bytes arrive without being read, the FIFO overruns; OVRN shows at the fourth read; a CNTLA
   write with EFR = 1 leaves it, one with EFR = 0 clears it (Zilog UM, printed pages 126 and 128). */
static void t_asci_efr(void)
{
    machine *m = new_machine();
    char d[200];
    int i;
    org(m, 0);
    out0(m, 0x02, 0x00);                        /* CNTLB0: SS = 0, PS = 0, DR = 0: 160 T a bit */
    out0(m, 0x00, 0x64);                        /* CNTLA0: RE, TE, 8 bits */
    for (i = 0; i < 4; i++)
        db(m, 4, 0x06, 0x00, 0x10, 0xFE);       /* LD B,0; DJNZ $ -- four times: well over 5 frames */
    for (i = 0; i < 4; i++)
        db(m, 3, 0xED, 0x38, 0x08);             /* IN0 A,(RDR0) x4 */
    db(m, 3, 0xED, 0x38, 0x04);                 /* IN0 A,(STAT0) */
    st_a(m, 0x9000);
    out0(m, 0x00, 0x2C);                        /* CNTLA0: TE, 8 bits, EFR = 1 (RE off) */
    db(m, 3, 0xED, 0x38, 0x04);
    st_a(m, 0x9001);
    out0(m, 0x00, 0x24);                        /* EFR = 0 */
    db(m, 3, 0xED, 0x38, 0x04);
    st_a(m, 0x9002);
    db(m, 1, 0x76);
    m->rx_on = 1;
    z180_run(m->cpu, 200000);
    sprintf(d, "STAT0 after the fourth read %02X, after EFR = 1 %02X, after EFR = 0 %02X (want OVRN 40h, 40h, clear)",
            m->mem[0x9000], m->mem[0x9001], m->mem[0x9002]);
    report("asci_efr", (m->mem[0x9000] & 0x40) && (m->mem[0x9001] & 0x40) && !(m->mem[0x9002] & 0x40), d);
    free_machine(m);
}

/* ---- MAME's e0deaf3898b, one test per hunk (Astra, Reply 92): each fails with its hunk reverted ------------- */

/* Enabling a timer starts it from RLDR: no overflow within a few ticks (reverted: from 0, TIF at the first tick). */
static void t_timer_start(void)
{
    machine *m = new_machine();
    char d[160];
    int i;
    org(m, 0);
    out0(m, 0x0E, 0x00);                        /* RLDR0 = 0100h */
    out0(m, 0x0F, 0x01);
    out0(m, 0x10, 0x01);                        /* TCR: TDE0 */
    for (i = 0; i < 8; i++)
        db(m, 1, 0x00);                         /* NOPs: a few ticks */
    db(m, 3, 0xED, 0x38, 0x10);                 /* IN0 A,(TCR) */
    st_a(m, 0x9000);
    db(m, 1, 0x76);
    z180_run(m->cpu, 2000);
    sprintf(d, "TCR after ~%d ticks: %02X (want TIF0 40h clear: counting down from RLDR)", 150 / 20, m->mem[0x9000]);
    report("timer_start", !(m->mem[0x9000] & 0x40), d);
    free_machine(m);
}

/* TMDR1H is the counter's high byte (reverted: written into the low half). */
static void t_tmdr1h(void)
{
    machine *m = new_machine();
    char d[160];
    org(m, 0);
    out0(m, 0x15, 0x12);                        /* TMDR1H, timer 1 stopped */
    out0(m, 0x14, 0x34);                        /* TMDR1L */
    db(m, 3, 0xED, 0x38, 0x14);                 /* IN0 A,(TMDR1L) */
    st_a(m, 0x9000);
    db(m, 3, 0xED, 0x38, 0x15);                 /* IN0 A,(TMDR1H) */
    st_a(m, 0x9001);
    db(m, 1, 0x76);
    z180_run(m->cpu, 2000);
    sprintf(d, "TMDR1 reads %02X%02X (want 1234)", m->mem[0x9001], m->mem[0x9000]);
    report("tmdr1h", m->mem[0x9001] == 0x12 && m->mem[0x9000] == 0x34, d);
    free_machine(m);
}

/* A DMA0 terminal-count interrupt raised while IFF1 = 0 is kept, and taken only after EI and its shadow (reverted
   DMA hunk: dropped; reverted gating hunk: taken while interrupts are disabled). */
static void t_dma_done_di(void)
{
    machine *m = new_machine();
    char d[240];
    int i, taken, ei_nop;
    org(m, 0);
    ld_sp(m, 0x8000);
    db(m, 1, 0xF3);                             /* DI */
    out0(m, 0x33, 0x40);                        /* IL: vectors at 0040h (I = 0); DMA0's at 0048h */
    out0(m, 0x20, 0x00); out0(m, 0x21, 0x10); out0(m, 0x22, 0x00);   /* SAR0 = 01000h */
    out0(m, 0x23, 0x00); out0(m, 0x24, 0x20); out0(m, 0x25, 0x00);   /* DAR0 = 02000h */
    out0(m, 0x26, 4); out0(m, 0x27, 0x00);                           /* BCR0 = 4 */
    out0(m, 0x31, 0x02);                                             /* DMODE: burst */
    out0(m, 0x30, 0x44);                                             /* DSTAT: DE0 | DIE0 */
    for (i = 0; i < 6; i++)
        db(m, 1, 0x00);                         /* the DMA is done here, interrupts still disabled */
    db(m, 2, 0x3E, 0x11);
    st_a(m, 0x9001);                            /* reached with the request pending but not taken */
    db(m, 1, 0xFB);                             /* EI */
    ei_nop = m->pos;
    db(m, 3, 0x00, 0x00, 0x76);                 /* NOP (the shadow), NOP, HALT */
    org(m, 0x48);
    db(m, 2, 0x00, 0x02);                       /* DMA0 vector -> 0200h */
    org(m, 0x200);
    db(m, 2, 0x3E, 0x77);
    st_a(m, 0x9000);
    db(m, 1, 0x76);
    z180_run(m->cpu, 20000);
    taken = first_step_at(m, 0x200);
    sprintf(d, "before EI: (9001h) = %02X; handler (9000h) = %02X; the NOP after EI at step %d, handler at step %d",
            m->mem[0x9001], m->mem[0x9000], first_step_at(m, (uint32_t)ei_nop), taken);
    report("dma_done_di", m->mem[0x9001] == 0x11 && m->mem[0x9000] == 0x77
                          && first_step_at(m, (uint32_t)ei_nop) > 0 && taken == first_step_at(m, (uint32_t)ei_nop) + 1, d);
    free_machine(m);
}

/* ---- TRAP (Zilog UM, printed pages 70-71; the op code maps, pages 247-251) ------------------------------------
   0000h tells a reset from a TRAP by ITC.TRAP: on a reset it jumps to 0100h (LD SP,9000h; EI; the instruction
   under test at 0104h; then (9001h) = 55h and HALT).  On a TRAP it stores ITC at 9000h, writes ITC = 01h (clear
   TRAP) and stores it at 9002h, writes 81h (try to set TRAP) and stores it at 9003h, then HALTs. */
static machine *trap_program(const int *ins, int n)
{
    machine *m = new_machine();
    int i;
    org(m, 0);
    db(m, 3, 0xED, 0x38, 0x34);                 /* 0000 IN0 A,(ITC) */
    db(m, 2, 0xE6, 0x80);                       /* AND 80h */
    db(m, 3, 0xCA, 0x00, 0x01);                 /* JP Z,0100h: a reset */
    db(m, 3, 0xED, 0x38, 0x34);
    st_a(m, 0x9000);
    out0(m, 0x34, 0x01);                        /* clear TRAP (ITE0 kept) */
    db(m, 3, 0xED, 0x38, 0x34);
    st_a(m, 0x9002);
    out0(m, 0x34, 0x81);                        /* try to set TRAP */
    db(m, 3, 0xED, 0x38, 0x34);
    st_a(m, 0x9003);
    db(m, 1, 0x76);
    org(m, 0x100);
    ld_sp(m, 0x9000);
    db(m, 1, 0xFB);                             /* EI: IEF1 = 1, to see that TRAP leaves it */
    for (i = 0; i < n; i++)
        db(m, 1, ins[i]);                       /* 0104h: the instruction under test */
    db(m, 2, 0x3E, 0x55);
    st_a(m, 0x9001);
    db(m, 1, 0x76);
    return m;
}

static void t_trap(void)
{
    static const struct { const char *name; int n, b[4]; int traps, ufo; } cases[] = {
        {"trap_dd_nop", 2, {0xDD, 0x00}, 1, 0},            /* DD before a non-HL instruction */
        {"trap_ixh", 2, {0xDD, 0x24}, 1, 0},               /* INC IXH: H as a plain register */
        {"trap_fd_exdehl", 2, {0xFD, 0xEB}, 1, 0},         /* EX DE,HL is named illegal after DD/FD */
        {"trap_ed_dup", 2, {0xED, 0x54}, 1, 0},            /* the Z80's NEG duplicate */
        {"trap_ed_in_c", 2, {0xED, 0x70}, 1, 0},           /* the Z80's IN (C) */
        {"trap_cb_sll", 2, {0xCB, 0x30}, 1, 0},            /* SLL B */
        {"trap_ddcb_reg", 4, {0xDD, 0xCB, 0x7F, 0x00}, 1, 1},   /* DDCB d 00: not an (IX+d) form: 3rd op code */
        {"trap_ddcb_sll", 4, {0xFD, 0xCB, 0x7F, 0x36}, 1, 1},   /* SLL (IY+d) */
        {"legal_ld_ix", 4, {0xDD, 0x21, 0x34, 0x12}, 0, 0},     /* the negative controls */
        {"legal_ddcb", 4, {0xDD, 0xCB, 0x7F, 0x06}, 0, 0},      /* RLC (IX+7Fh) */
        {"legal_ed_neg", 2, {0xED, 0x44}, 0, 0},
        {"legal_ed_mlt", 2, {0xED, 0x4C}, 0, 0},
        {"legal_cb_srl", 2, {0xCB, 0x38}, 0, 0},
        {"legal_fd_jp", 2, {0xFD, 0xE9}, 0, 0},            /* JP (IY): IY = FFFFh after reset -> not taken far */
    };
    int k;
    for (k = 0; k < (int)(sizeof cases / sizeof cases[0]); k++) {
        machine *m = trap_program(cases[k].b, cases[k].n);
        z180_regs r;
        char d[240];
        unsigned stacked;
        int ok;
        if (cases[k].b[0] == 0xFD && cases[k].b[1] == 0xE9)
            m->mem[0xFFFF] = 0x76;              /* JP (IY) lands on a HALT at FFFFh */
        z180_run(m->cpu, 4000);
        z180_regs_get(m->cpu, &r);
        stacked = (unsigned)(m->mem[0x8FFF] << 8 | m->mem[0x8FFE]);
        if (cases[k].traps) {
            unsigned want = 0x0104 + 1 + cases[k].ufo;          /* start + 1 (UFO 0) or + 2 (UFO 1) */
            ok = (m->mem[0x9000] & 0xC0) == (0x80 | (cases[k].ufo ? 0x40 : 0)) && stacked == want && r.sp == 0x8FFE
                 && r.iff1 == 1 && m->mem[0x9001] == 0 && !(m->mem[0x9002] & 0x80) && !(m->mem[0x9003] & 0x80);
            sprintf(d, "ITC %02X (TRAP, UFO %d wanted), stacked %04X (want %04X), IFF1 %d, after clearing %02X, "
                    "after writing 1 %02X", m->mem[0x9000], cases[k].ufo, stacked, want, r.iff1, m->mem[0x9002],
                    m->mem[0x9003]);
        } else {
            ok = m->mem[0x9000] == 0 && (m->mem[0x9001] == 0x55 || cases[k].b[1] == 0xE9);
            sprintf(d, "no TRAP: ITC record %02X, marker %02X, halted at %04X", m->mem[0x9000], m->mem[0x9001], r.pc);
            if (cases[k].b[1] == 0xE9)
                ok = ok && r.pc == 0xFFFF;
        }
        report(cases[k].name, ok, d);
        free_machine(m);
    }
}

/* ---- after Astra, Reply 93/94 -------------------------------------------------------------------------------- */

/* The injected instruction's timing and R (printed pp. 76 and 177): with programmed waits off an injected RST is
   13 T (T1 T2 TW* TW* T3, Ti Ti, two push cycles); each acknowledge is one M1 (R + 1); a prefixed injected
   instruction keeps the PC and counts its second op code fetch too; an undefined one TRAPs, stacking the PC the
   interrupt found. */
static void t_im0_more(void)
{
    static const struct { const char *name; int n, b[4]; int t, r, pc, sp, trap; } cases[] = {
        {"im0_rst_nowait", 1, {0xC7}, 13, 1, 0x0000, 0x8FFE, 0},
        {"im0_nop_nowait", 1, {0x00}, 5, 1, 0x000C, 0x9000, 0},
        {"im0_prefixed", 4, {0xDD, 0x21, 0x34, 0x12}, -1, 2, 0x000C, 0x9000, 0},   /* LD IX,1234h */
        {"im0_undefined", 2, {0xDD, 0x00}, -1, 2, 0x0000, 0x8FFE, 1},
    };
    int k;
    for (k = 0; k < (int)(sizeof cases / sizeof cases[0]); k++) {
        machine *m = new_machine();
        z180_regs r0, r1;
        char d[220];
        int i, t = 0, ok;
        org(m, 0);
        out0(m, 0x32, 0x00);                    /* 0000 DCNTL: no programmed wait states */
        ld_sp(m, 0x9000);                       /* 0005 */
        db(m, 2, 0xED, 0x46);                   /* 0008 IM 0 */
        db(m, 1, 0xFB);                         /* 000A EI */
        for (i = 0; i < 6; i++)
            db(m, 1, 0x00);                     /* 000B.. NOPs; the interrupted instruction is at 000Ch */
        for (i = 0; i < 4; i++)
            m->ack[i] = cases[k].b[i];
        z180_set_irq(m->cpu, Z180_INT0, 1);
        for (i = 1; i <= 5; i++)                /* LD A; OUT0; LD SP; IM 0; EI */
            z180_step(m->cpu);
        z180_step(m->cpu);                      /* the NOP in EI's shadow (at 000B) */
        z180_regs_get(m->cpu, &r0);
        t = z180_step(m->cpu);                  /* accepts, runs the injected instruction */
        z180_regs_get(m->cpu, &r1);
        ok = ((r1.r - r0.r) & 0x7F) == cases[k].r && r1.pc == cases[k].pc && r1.sp == cases[k].sp
             && (cases[k].t < 0 || t == cases[k].t);
        if (cases[k].trap)
            ok = ok && m->mem[0x8FFF] == 0x00 && m->mem[0x8FFE] == 0x0C;   /* the PC the interrupt found */
        if (cases[k].b[0] == 0xDD && cases[k].b[1] == 0x21)
            ok = ok && r1.ix == 0x1234;
        sprintf(d, "%d T, R +%d, PC %04X, SP %04X, IX %04X, stacked %02X%02X", t, (r1.r - r0.r) & 0x7F, r1.pc, r1.sp,
                r1.ix, m->mem[0x8FFF], m->mem[0x8FFE]);
        report(cases[k].name, ok, d);
        free_machine(m);
    }
}

/* Injected prefixed instructions that DO set the PC (Astra, Reply 95: a jump to the interrupted PC + 1 was taken
   for the fetches' own advance and undone).  The interrupted instruction is at 0010h; IX and the stack are set
   first.  JP (IX) and RETN to 0011h (the collision) and 0012h (ordinary); LD IX,nn at a wrapped PC (the interrupt
   lands at 0000h after the shadow NOP at FFFFh: the PC stays 0000h); LDIR from the acknowledge runs once (model)
   with the PC put back: one byte moved, BC 3 -> 2 (its set-up runs at 0100h, so it is interrupted at 010Dh). */
static void t_im0_transfer(void)
{
    static const struct { const char *name; int n, b[4]; unsigned ix, stack, want_pc; int wrap; } cases[] = {
        {"im0_jpix_collision", 2, {0xDD, 0xE9}, 0x0011, 0, 0x0011, 0},
        {"im0_jpix_ordinary", 2, {0xDD, 0xE9}, 0x0012, 0, 0x0012, 0},
        {"im0_retn_collision", 2, {0xED, 0x45}, 0x3000, 0x0011, 0x0011, 0},
        {"im0_retn_ordinary", 2, {0xED, 0x45}, 0x3000, 0x0012, 0x0012, 0},
        {"im0_wrap", 4, {0xDD, 0x21, 0x34, 0x12}, 0x3000, 0, 0x0000, 1},
        {"im0_ldir_once", 2, {0xED, 0xB0}, 0x3000, 0, 0x010D, 0},
    };
    int k;
    for (k = 0; k < (int)(sizeof cases / sizeof cases[0]); k++) {
        machine *m = new_machine();
        z180_regs r0, r1;
        char d[200];
        int i, steps, ok;
        org(m, 0);
        out0(m, 0x32, 0x00);                            /* 0000 no programmed waits */
        ld_sp(m, 0x9000);                               /* 0005 */
        db(m, 4, 0xDD, 0x21, cases[k].ix & 0xFF, cases[k].ix >> 8);   /* 0008 LD IX,nn */
        if (cases[k].wrap) {
            db(m, 3, 0xC3, 0xFC, 0xFF);                 /* 000C JP FFFCh */
            org(m, 0xFFFC);
            db(m, 2, 0xED, 0x46);                       /* FFFC IM 0 */
            db(m, 1, 0xFB);                             /* FFFE EI */
            db(m, 1, 0x00);                             /* FFFF NOP (the shadow); the interrupt then finds 0000h */
            steps = 8;                                  /* LD A, OUT0, LD SP, LD IX, JP, IM 0, EI, NOP */
        } else if (cases[k].b[1] == 0xB0) {
            db(m, 3, 0xC3, 0x00, 0x01);                 /* 000C JP 0100h */
            org(m, 0x0100);
            db(m, 3, 0x21, 0x00, 0x40);                 /* 0100 LD HL,4000h */
            db(m, 3, 0x11, 0x00, 0x50);                 /* 0103 LD DE,5000h */
            db(m, 3, 0x01, 0x03, 0x00);                 /* 0106 LD BC,3 */
            db(m, 2, 0xED, 0x46);                       /* 0109 IM 0 */
            db(m, 1, 0xFB);                             /* 010B EI */
            db(m, 3, 0x00, 0x00, 0x00);                 /* 010C NOP (the shadow); 010D the interrupted one */
            steps = 11;                                 /* LD A, OUT0, LD SP, LD IX, JP, 3 loads, IM 0, EI, NOP */
        } else {
            db(m, 2, 0xED, 0x46);                       /* 000C IM 0 */
            db(m, 1, 0xFB);                             /* 000E EI */
            db(m, 3, 0x00, 0x00, 0x00);                 /* 000F NOP (the shadow); 0010 the interrupted one */
            steps = 7;
        }
        m->mem[0x9000] = cases[k].stack & 0xFF;          /* RETN's return address */
        m->mem[0x9001] = cases[k].stack >> 8;
        for (i = 0; i < 4; i++)
            m->ack[i] = cases[k].b[i];
        if (cases[k].b[1] == 0xB0) {                    /* LDIR: HL = 4000h -> DE = 5000h, BC = 3 */
            m->mem[0x4000] = 0xAB;
            m->mem[0x4001] = 0xCD;
        }
        z180_set_irq(m->cpu, Z180_INT0, 1);
        for (i = 0; i < steps; i++)
            z180_step(m->cpu);
        z180_regs_get(m->cpu, &r0);
        z180_step(m->cpu);                              /* accepts, runs the injected instruction */
        z180_regs_get(m->cpu, &r1);
        ok = r1.pc == cases[k].want_pc;
        if (cases[k].b[0] == 0xDD && cases[k].b[1] == 0x21)
            ok = ok && r1.ix == 0x1234;
        if (cases[k].b[1] == 0x45)
            ok = ok && r1.sp == 0x9002;
        if (cases[k].b[1] == 0xB0)                      /* one iteration: one byte, the counts stepped once */
            ok = ok && m->mem[0x5000] == 0xAB && m->mem[0x5001] == 0x00 && r1.bc == 2 && r1.hl == 0x4001
                 && r1.de == 0x5001;
        sprintf(d, "interrupted at %04X: PC %04X (want %04X), SP %04X, IX %04X, BC %04X, [5000h] %02X %02X", r0.pc,
                r1.pc, cases[k].want_pc, r1.sp, r1.ix, r1.bc, m->mem[0x5000], m->mem[0x5001]);
        report(cases[k].name, ok, d);
        free_machine(m);
    }
}

/* TRAP on the bus (printed pp. 71-72, Figures 32 and 33), programmed waits off, IX = 3000h: the cycles, the R
   count, the reads, and the stack written PCH to SP-1 first. */
static void t_trap_bus(void)
{
    static const struct { const char *name; int n, b[4]; int t, r, read3005, trap; } cases[] = {
        {"trapbus_2nd", 2, {0xDD, 0x24}, 18, 2, 0, 1},
        {"trapbus_3rd", 4, {0xDD, 0xCB, 0x05, 0x00}, 26, 3, 1, 1},
        {"trapbus_legal_ddcb", 4, {0xDD, 0xCB, 0x05, 0x06}, -1, 3, 1, 0},   /* RLC (IX+5): the control */
    };
    int k;
    for (k = 0; k < (int)(sizeof cases / sizeof cases[0]); k++) {
        machine *m = new_machine();
        z180_regs r0, r1;
        char d[240];
        int i, t, reads_ixd = 0, ok;
        org(m, 0x0100);
        for (i = 0; i < cases[k].n; i++)
            db(m, 1, cases[k].b[i]);            /* the instruction under test at 0100h */
        db(m, 1, 0x76);
        org(m, 0);
        out0(m, 0x32, 0x00);                    /* DCNTL: no programmed waits */
        ld_sp(m, 0x9000);
        db(m, 4, 0xDD, 0x21, 0x00, 0x30);       /* LD IX,3000h */
        db(m, 3, 0xC3, 0x00, 0x01);             /* JP 0100h */
        for (i = 0; i < 5; i++)
            z180_step(m->cpu);
        z180_regs_get(m->cpu, &r0);
        m->log_reads = 1;
        m->n_wr = 0;
        t = z180_step(m->cpu);
        m->log_reads = 0;
        z180_regs_get(m->cpu, &r1);
        for (i = 0; i < m->n_rd; i++)
            reads_ixd += m->rd_addr[i] == 0x3005;
        ok = r0.pc == 0x0100 && ((r1.r - r0.r) & 0x7F) == cases[k].r && reads_ixd == cases[k].read3005
             && (cases[k].t < 0 || t == cases[k].t);
        if (cases[k].trap)
            ok = ok && r1.pc == 0x0000 && m->n_wr == 2 && m->wr_addr[0] == 0x8FFF && m->wr_addr[1] == 0x8FFE;
        sprintf(d, "%d T (want %d), R +%d, %d read(s) of IX+d, %d reads in all; stack writes %d: %04X then %04X",
                t, cases[k].t, (r1.r - r0.r) & 0x7F, reads_ixd, m->n_rd, m->n_wr, m->n_wr > 0 ? m->wr_addr[0] : 0,
                m->n_wr > 1 ? m->wr_addr[1] : 0);
        report(cases[k].name, ok, d);
        free_machine(m);
    }
}

/* Fixed priority, PRT0 above DMA0 (Figure 31), when both wait under DI: a PRT0 overflow (TIF0, TIE0; the timer
   then stopped) and a finished DMA0 (DIE0).  After EI and its shadow PRT0 must be taken, in every one of 20
   timer-clock phases (Astra's fixture: it won in 3 of 20).  With TIF0 cleared by software first (read TCR, then
   TMDR0L), DMA0 must be taken: no stale request survives its flag. */
static int priority_run(int phase, int clear_tif)
{
    machine *m = new_machine();
    int i, got;
    org(m, 0);
    out0(m, 0x32, 0x00);                        /* no programmed waits */
    ld_sp(m, 0x8000);
    db(m, 1, 0xF3);                             /* DI */
    out0(m, 0x33, 0xE0);                        /* IL: PRT0 at 00E4h, DMA0 at 00E8h (past the code) */
    out0(m, 0x0E, 0x02); out0(m, 0x0F, 0x00);   /* RLDR0 = 2 */
    out0(m, 0x10, 0x11);                        /* TCR: TIE0 | TDE0 */
    db(m, 4, 0x06, 0x10, 0x10, 0xFE);           /* LD B,16; DJNZ $: well past the first overflow */
    out0(m, 0x10, 0x10);                        /* TCR: TIE0 only -- the timer stops, TIF0 stays */
    out0(m, 0x20, 0x00); out0(m, 0x21, 0x10); out0(m, 0x22, 0x00);   /* DMA0 1000h -> 2000h, 2 bytes, burst */
    out0(m, 0x23, 0x00); out0(m, 0x24, 0x20); out0(m, 0x25, 0x00);
    out0(m, 0x26, 2); out0(m, 0x27, 0x00);
    out0(m, 0x31, 0x02);
    out0(m, 0x30, 0x44);                        /* DE0 | DIE0 */
    for (i = 0; i < phase; i++)
        db(m, 1, 0x00);                         /* the phase: 0-19 NOPs */
    if (clear_tif) {
        db(m, 3, 0xED, 0x38, 0x10);             /* IN0 A,(TCR) */
        db(m, 3, 0xED, 0x38, 0x0C);             /* IN0 A,(TMDR0L): TIF0 cleared */
    }
    db(m, 1, 0xFB);                             /* EI */
    db(m, 3, 0x00, 0x00, 0x76);
    org(m, 0xE4); db(m, 2, 0x00, 0x06);
    org(m, 0xE8); db(m, 2, 0x00, 0x07);
    org(m, 0x600); db(m, 2, 0x3E, 0x01); st_a(m, 0x9000); db(m, 1, 0x76);
    org(m, 0x700); db(m, 2, 0x3E, 0x02); st_a(m, 0x9000); db(m, 1, 0x76);
    z180_run(m->cpu, 20000);
    got = m->mem[0x9000];
    if (getenv("PRIORITY_DEBUG") && phase == 0) {
        z180_regs r;
        z180_regs_get(m->cpu, &r);
        printf("  debug: pc %04X iff1 %d halted %d; copied %02X %02X; sp %04X top %02X%02X\n", r.pc, r.iff1, r.halted,
               m->mem[0x2000], m->mem[0x2001], r.sp, m->mem[(r.sp + 1) & 0xFFFF], m->mem[r.sp]);
    }
    free_machine(m);
    return got;
}

static void t_priority(void)
{
    char d[200];
    int ph, prt = 0, dma = 0, other = 0, cleared_dma = 0;
    for (ph = 0; ph < 20; ph++) {
        int g = priority_run(ph, 0);
        prt += g == 1;
        dma += g == 2;
        other += g != 1 && g != 2;
        cleared_dma += priority_run(ph, 1) == 2;
    }
    sprintf(d, "both waiting: PRT0 taken in %d of 20 phases (DMA0 %d, other %d); TIF0 cleared first: DMA0 in %d of 20",
            prt, dma, other, cleared_dma);
    report("prt_priority", prt == 20, d);
    report("prt_stale", cleared_dma == 20, d);
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
    t_im0();
    t_sleep_dma();
    t_accept_pc();
    t_asci_efr();
    t_timer_start();
    t_tmdr1h();
    t_dma_done_di();
    t_trap();
    t_im0_more();
    t_im0_transfer();
    t_trap_bus();
    t_priority();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
