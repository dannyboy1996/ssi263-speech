/* pc86.c -- a bare PC for a host that stands in for DOS (see pc86.h).  MIT. */
#include <stdlib.h>
#include <string.h>
#include "pc86.h"

struct pc86 {
    uint8_t *mem;
    i86 *cpu;
    pc86_in_fn in;
    pc86_out_fn out;
    pc86_int_fn intr;
    void *user;
    int vector;
    int stop;
};

static uint8_t rd(void *ctx, uint32_t a) { return ((pc86 *)ctx)->mem[a & 0xFFFFF]; }
static void wr(void *ctx, uint32_t a, uint8_t v) { ((pc86 *)ctx)->mem[a & 0xFFFFF] = v; }
static uint8_t in(void *ctx, uint16_t port)
{
    pc86 *p = (pc86 *)ctx;
    return p->in ? (uint8_t)p->in(p->user, port) : 0xFF;
}
static void out(void *ctx, uint16_t port, uint8_t v)
{
    pc86 *p = (pc86 *)ctx;
    if (p->out)
        p->out(p->user, port, v);
}
static int ack(void *ctx, int line, int n)
{
    (void)line;
    return n == 0 ? ((pc86 *)ctx)->vector : -1;
}
static int intercept(void *ctx, int vector, int kind)
{
    pc86 *p = (pc86 *)ctx;
    return p->intr ? p->intr(p->user, vector, kind) : 0;
}

pc86 *pc86_create(void)
{
    pc86 *p = (pc86 *)calloc(1, sizeof(pc86));
    cpu_bus bus;
    if (!p)
        return NULL;
    p->mem = (uint8_t *)calloc(1, PC86_MEM_SIZE);
    if (!p->mem) {
        free(p);
        return NULL;
    }
    p->vector = 0xFF;
    memset(&bus, 0, sizeof bus);
    bus.ctx = p;
    bus.read = rd;
    bus.write = wr;
    bus.in = in;
    bus.out = out;
    bus.irq_ack = ack;
    bus.intercept = intercept;
    p->cpu = i86_create(&bus, 0.0);
    if (!p->cpu) {
        free(p->mem);
        free(p);
        return NULL;
    }
    return p;
}

void pc86_destroy(pc86 *p)
{
    if (!p)
        return;
    i86_destroy(p->cpu);
    free(p->mem);
    free(p);
}

uint8_t *pc86_mem(pc86 *p) { return p->mem; }

void pc86_set_hooks(pc86 *p, pc86_in_fn in_fn, pc86_out_fn out_fn, pc86_int_fn intr, void *user)
{
    p->in = in_fn;
    p->out = out_fn;
    p->intr = intr;
    p->user = user;
}

uint64_t pc86_run(pc86 *p, uint64_t count, uint32_t until)
{
    uint64_t n = 0;
    i86_regs r;
    p->stop = 0;
    while (n < count && !p->stop) {
        if (until != PC86_NO_UNTIL) {
            i86_regs_get(p->cpu, &r);
            if (((((uint32_t)r.cs << 4) + r.ip) & 0xFFFFF) == until)
                break;
        }
        i86_step(p->cpu);
        n++;
    }
    return n;
}

void pc86_stop(pc86 *p) { p->stop = 1; }
void pc86_regs_get(pc86 *p, i86_regs *out_regs) { i86_regs_get(p->cpu, out_regs); }
void pc86_regs_set(pc86 *p, const i86_regs *in_regs) { i86_regs_set(p->cpu, in_regs); }
void pc86_set_irq(pc86 *p, int line, int asserted) { i86_set_irq(p->cpu, line, asserted); }
void pc86_set_vector(pc86 *p, int vector) { p->vector = vector & 0xFF; }
uint64_t pc86_cycles(pc86 *p) { return i86_cycles(p->cpu); }
uint64_t pc86_steps(pc86 *p) { return i86_steps(p->cpu); }
uint64_t pc86_aliased(pc86 *p, uint32_t *last_addr, uint8_t *last_op) { return i86_aliased(p->cpu, last_addr, last_op); }
