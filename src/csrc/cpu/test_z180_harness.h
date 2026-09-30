/* test_z180_harness.h -- the machine the cpu.h Z180 tests run on: 64K of flat RAM, a recorder at each boundary,
 * a tiny assembler and the report line.  Shared by test_z180_contract.c (the corrected path) and
 * test_z180_legacy.c (the legacy path); one translation unit each, so everything here is static.
 */
#ifndef TEST_Z180_HARNESS_H
#define TEST_Z180_HARNESS_H

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

#endif
