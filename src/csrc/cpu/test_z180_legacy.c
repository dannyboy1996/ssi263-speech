/* test_z180_legacy.c -- the LEGACY path's exceptions (CONTRACT.md 3), on z180_legacy.c (z180emu, GPL build).
 *
 * z180_run_legacy(budget) keeps today's z180emu slice, exceptions included, so the Braille Lite goldens hold.  The
 * goldens may never exercise the exceptions, so each has a synthetic test here.  These are regression facts about
 * the legacy core, not the definition of correct (the corrected path, test_z180_contract.c, differs in each):
 *   legacy_nmi_entry   an NMI raised at a slice's first boundary stays pending for the rest of that slice (ten NOPs
 *                      in one 30-cycle call) and is taken at the next call's entry; one-cycle calls take it at the
 *                      next call -- the control (Astra, Reply 78)
 *   legacy_burst       a burst DMA takes the rest of the slice's budget in one chunk with a single boundary
 *   legacy_slp_slice   SLP ends the slice: the call reports its whole budget while the PRT counts only that step,
 *                      so a PRT wake from SLP takes far more reported cycles than from HALT -- the control
 *
 *   build: gcc -std=gnu89 -fcommon -I<z180emu> -I<z180emu>/z180 test_z180_legacy.c z180_legacy.c  (build_board.py)
 * Exit status 0 when every test passes.
 */
#include "test_z180_harness.h"

/* ten NOPs after the stack; the NMI vector HALTs.  The NMI is raised in the boundary of step 1 (the LD SP). */
static machine *nmi_program(void)
{
    machine *m = new_machine();
    int i;
    org(m, 0);
    ld_sp(m, 0x8000);
    for (i = 0; i < 40; i++)
        db(m, 1, 0x00);
    org(m, 0x66);
    db(m, 1, 0x76);
    m->raise_at_step = 1;
    m->raise_line = Z180_NMI;
    return m;
}

static void t_nmi_entry(void)
{
    machine *a = nmi_program(), *b = nmi_program();
    char d[240];
    int i, taken_a_first, taken_a, n_first, taken_b;
    uint64_t done = z180_run_legacy(a->cpu, 30);
    taken_a_first = first_step_at(a, 0x66);          /* within the one 30-cycle call: must not be taken */
    n_first = a->n_pcs;
    z180_run_legacy(a->cpu, 30);
    taken_a = first_step_at(a, 0x66);
    for (i = 0; i < 40; i++)                          /* the control: one-cycle calls */
        z180_run_legacy(b->cpu, 1);
    taken_b = first_step_at(b, 0x66);
    /* raised in boundary 1: the 30-cycle call runs LD SP and seven NOPs (8 boundaries) with it pending; the next
       call's first boundary is the vector's.  One-cycle calls: boundary 2, the next call. */
    sprintf(d, "one 30-cycle call: %llu cycles, %d boundaries, vector %s; next call: vector at boundary %d; "
            "one-cycle calls: vector at boundary %d", (unsigned long long)done, n_first,
            taken_a_first < 0 ? "not taken" : "TAKEN", taken_a, taken_b);
    report("legacy_nmi_entry", taken_a_first < 0 && n_first == 8 && taken_a == n_first + 1 && taken_b == 2, d);
    free_machine(a);
    free_machine(b);
}

static void t_burst(void)
{
    machine *m = new_machine();
    char d[240];
    int i, n_before, copied = 0;
    uint64_t done;
    org(m, 0);
    out0(m, 0x20, 0x00); out0(m, 0x21, 0x10); out0(m, 0x22, 0x00);   /* SAR0 = 01000h */
    out0(m, 0x23, 0x00); out0(m, 0x24, 0x20); out0(m, 0x25, 0x00);   /* DAR0 = 02000h */
    out0(m, 0x26, 40); out0(m, 0x27, 0x00);                          /* BCR0 = 40 */
    out0(m, 0x31, 0x02);                                             /* DMODE: memory+1 -> memory+1, burst */
    out0(m, 0x30, 0x40);                                             /* DSTAT: DE0 -> DME */
    db(m, 2, 0x00, 0x76);                                            /* 0032 NOP; HALT */
    for (i = 0; i < 40; i++)
        m->mem[0x1000 + i] = (uint8_t)(0xA0 + i);
    while (first_step_at(m, 0x2F) < 0)                               /* up to the OUT0 that starts it */
        z180_run_legacy(m->cpu, 1);
    n_before = m->n_pcs;
    done = z180_run_legacy(m->cpu, 100);
    for (i = 0; i < 40; i++)
        copied += m->mem[0x2000 + i] == (uint8_t)(0xA0 + i);
    /* z180emu charges 6 T a byte here (no wait states): the chunk runs until the budget is spent -- 17 bytes, 102
       cycles -- behind one boundary; the corrected path moves 16 bytes a step, each with its boundary */
    sprintf(d, "a 100-cycle call from the DSTAT write: %llu cycles, %d boundary, %d of 40 bytes copied (want 102, "
            "1, 17)", (unsigned long long)done, m->n_pcs - n_before, copied);
    report("legacy_burst", done == 102 && m->n_pcs - n_before == 1 && copied == 17, d);
    free_machine(m);
}

static void t_slp_slice(void)
{
    int k, wakes[2];
    uint64_t cyc[2];
    char d[240];
    for (k = 0; k < 2; k++) {                        /* 0: SLP, 1: HALT (the control) */
        machine *m = new_machine();
        org(m, 0);
        ld_sp(m, 0x8000);
        out0(m, 0x33, 0xE0);                        /* IL = E0h: PRT0's vector at 00E4h (I = 0) */
        out0(m, 0x0C, 0x40); out0(m, 0x0D, 0x00);   /* TMDR0 = 0040h: 65 counts of 20 clocks to the overflow */
        out0(m, 0x0E, 0x40); out0(m, 0x0F, 0x00);   /* RLDR0 = 0040h */
        out0(m, 0x10, 0x11);                        /* TCR: TIE0 | TDE0 */
        db(m, 1, 0xFB);                             /* EI */
        if (k == 0)
            db(m, 2, 0xED, 0x76);                   /* loop: SLP */
        else
            db(m, 2, 0x76, 0x00);                   /* loop: HALT; NOP */
        db(m, 2, 0x18, 0xFC);                       /* JR loop */
        m->mem[0xE4] = 0x00;                        /* the vector: 0100h */
        m->mem[0xE5] = 0x01;
        org(m, 0x100);                              /* the handler: clear TIF0 (read TCR, then TMDR0L), count */
        db(m, 3, 0xED, 0x38, 0x10);                 /* IN0 A,(TCR) */
        db(m, 3, 0xED, 0x38, 0x0C);                 /* IN0 A,(TMDR0L) */
        db(m, 3, 0x3A, 0x00, 0x90);                 /* LD A,(9000h) */
        db(m, 1, 0x3C);                             /* INC A */
        st_a(m, 0x9000);
        db(m, 1, 0xFB);                             /* EI */
        db(m, 2, 0xED, 0x4D);                       /* RETI */
        /* the legacy timer restarts from 0 when enabled, so the first wake comes at once; then one every reload
           period (65 counts of 20 clocks = 1300 clocked T-states).  One 5000-cycle call. */
        cyc[k] = z180_run_legacy(m->cpu, 5000);
        wakes[k] = m->mem[0x9000];
        free_machine(m);
    }
    /* SLP ends the slice: the call reports its whole budget, but the PRT counted only up to the SLP (no wake yet: the
       immediate one comes after it); the HALT control is clocked throughout (four wakes in the 5000 cycles) */
    sprintf(d, "one 5000-cycle call: SLP %llu cycles reported, %d PRT0 wakes; HALT (the control) %llu, %d wakes",
            (unsigned long long)cyc[0], wakes[0], (unsigned long long)cyc[1], wakes[1]);
    report("legacy_slp_slice", cyc[0] >= 5000 && wakes[0] == 0 && cyc[1] >= 5000 && wakes[1] == 4, d);
}

int main(void)
{
    t_nmi_entry();
    t_burst();
    t_slp_slice();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
