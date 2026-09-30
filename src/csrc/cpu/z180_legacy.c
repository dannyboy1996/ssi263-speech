/* z180_legacy.c -- cpu.h's Z180 on z180emu's core: the LEGACY compatibility path (CONTRACT.md 3), and only that.
 *
 * z180emu (GPL-2.0-or-later) is compiled into this one translation unit, so a build containing it is GPL.  Its
 * callbacks carry no context: the instance being run is a thread-local pointer set at every entry point, and each
 * callback is forwarded to that instance's cpu_bus with its ctx.  Everything z180emu-specific that the Braille Lite
 * board used to reach into -- the ASCI baud ticking, the ASCI request level, /DCD0, the cycle count within a slice --
 * lives here now, unchanged, so the board sees only cpu.h.
 *
 * Provided: z180_create, z180_destroy, z180_reset, z180_run_legacy, z180_set_irq, z180_cycles, z180_steps, z180_pc,
 * z180_regs_get.  NOT provided: z180_step and z180_run (the corrected path), which z180emu cannot give faithfully
 * (NMI is sampled at slice entry; a burst DMA chunk takes the budget; SLP ends the slice).  They come with MAME's
 * core.  z180emu has no destructor: z180_destroy frees this adapter, not the core's own allocation (a known limit).
 */
#include "z180/z80common.h"
#undef logerror
#define logerror(...) ((void)0)
#include "z180/z180dasm.c"
#include "z180/z80daisy.c"
#include "z180/z80scc.c"
#include "z180/z180asci.c"
#include "cpu.h"

#if defined(_MSC_VER)
#define Z180_TLS __declspec(thread)
#else
#define Z180_TLS __thread
#endif

struct z180 {
    cpu_bus bus;
    struct z180_device *dev;
    unsigned long long cyc_base;      /* T-states before the current legacy slice */
    int cur_slice;                    /* the current slice's budget (its cycle count is cur_slice - icount) */
    unsigned long long steps, asci_next;
    uint32_t pc;
    UINT8 *asci_pend;
};

static Z180_TLS z180 *zcur;

/* ---- z180emu's context-free callbacks, forwarded to the running instance ---------------------------------------- */
static UINT8 zmem_read(offs_t a) { return zcur->bus.read(zcur->bus.ctx, (uint32_t)a); }
static UINT8 zmem_fetch(offs_t a)
{
    return zcur->bus.fetch ? zcur->bus.fetch(zcur->bus.ctx, (uint32_t)a) : zcur->bus.read(zcur->bus.ctx, (uint32_t)a);
}
static void zmem_write(offs_t a, UINT8 v) { zcur->bus.write(zcur->bus.ctx, (uint32_t)a, v); }
static UINT8 zio_read(offs_t p) { return zcur->bus.in(zcur->bus.ctx, (uint16_t)p); }
static void zio_write(offs_t p, UINT8 v) { zcur->bus.out(zcur->bus.ctx, (uint16_t)p, v); }

static int zirqack(device_t *device, int irqnum)
{
    int v;
    (void)device;
    if (!zcur->bus.irq_ack)
        return 0xFF;
    v = zcur->bus.irq_ack(zcur->bus.ctx, irqnum, 0);
    return v < 0 ? 0xFF : v;
}

static int zasci_rx(device_t *device, int channel)
{
    (void)device;
    return zcur->bus.serial_rx ? zcur->bus.serial_rx(zcur->bus.ctx, channel) : -1;
}

static void zasci_tx(device_t *device, int channel, UINT8 data)
{
    (void)device;
    if (zcur->bus.serial_tx)
        zcur->bus.serial_tx(zcur->bus.ctx, channel, data);
}

static unsigned long long zcycles_now(void)
{
    return zcur->cyc_base + (unsigned long long)(zcur->cur_slice - cpu_icount_z180((device_t *)zcur->dev));
}

/* The step boundary (CONTRACT.md 1, phase D): steps, then the ASCI's catch-up and its request level, then the
   board.  Moved here from bl_board.c unchanged. */
void debugger_instruction_hook(device_t *device, offs_t curpc)
{
    z180 *c = zcur;
    (void)device;
    c->steps++;
    c->pc = (uint32_t)curpc;
    {                                        /* ASCI baud clock: one tick per 16 CPU cycles (DR = 16; legacy) */
        unsigned long long now = zcycles_now();
        if (c->asci_next <= now) {
            struct z180asci_channel *c0 = c->dev->z180asci->m_chan0, *c1 = c->dev->z180asci->m_chan1;
            unsigned long long ticks = (now - c->asci_next) / 16 + 1;
            if (ticks < c0->m_brg_timer && ticks < c1->m_brg_timer) {
                c0->m_brg_timer -= (uint16_t)ticks;
                c1->m_brg_timer -= (uint16_t)ticks;
                c->asci_next += ticks * 16;
            } else {
                while (c->asci_next <= now) {
                    z180asci_channel_device_timer(c0);
                    z180asci_channel_device_timer(c1);
                    c->asci_next += 16;
                }
            }
        }
        {                                    /* the ASCI interrupt request is a level (Astra, Reply 15) */
            int k;
            if (!c->asci_pend)
                c->asci_pend = z180_asci_irq_pending((device_t *)c->dev);
            for (k = 0; k < 2; k++) {
                struct z180asci_channel *ch = k ? c->dev->z180asci->m_chan1 : c->dev->z180asci->m_chan0;
                int rx = (ch->m_stat & 0x08) && (ch->m_stat & (0x80 | 0x40 | 0x20 | 0x10));
                int tx = (ch->m_stat & 0x01) && (ch->m_stat & 0x02);
                c->asci_pend[k] = (UINT8)(rx || tx);
            }
        }
    }
    if (c->bus.boundary)
        c->bus.boundary(c->bus.ctx, (uint32_t)curpc);
}

#include "z180/z180.c"

/* ---- cpu.h ---------------------------------------------------------------------------------------------------- */
static struct address_space zmemspace = {zmem_read, zmem_write, zmem_fetch};
static struct address_space ziospace = {zio_read, zio_write, NULL};

static void zpins(z180 *c)
{
    c->dev->z180asci->m_chan0->m_dcd = c->bus.serial_pin ? (c->bus.serial_pin(c->bus.ctx, Z180_PIN_DCD0) ? 1 : 0) : 1;
}

z180 *z180_create(const cpu_bus *bus, double clock_hz)
{
    z180 *c = (z180 *)calloc(1, sizeof(z180));
    if (!c)
        return NULL;
    c->bus = *bus;
    zcur = c;
    c->dev = cpu_create_z180((char *)"Z180", Z180_TYPE_Z180, (UINT32)clock_hz, &zmemspace, NULL, &ziospace, zirqack,
                             NULL, zasci_rx, zasci_tx, NULL, NULL, NULL, NULL);
    if (!c->dev) {
        free(c);
        return NULL;
    }
    cpu_reset_z180((device_t *)c->dev);
    zpins(c);
    return c;
}

void z180_destroy(z180 *c)
{
    if (zcur == c)
        zcur = NULL;
    free(c);                                 /* the core's own allocation stays: z180emu has no destructor */
}

void z180_reset(z180 *c)
{
    zcur = c;
    cpu_reset_z180((device_t *)c->dev);
    zpins(c);
    c->cyc_base = c->steps = c->asci_next = 0;
    c->cur_slice = 0;
}

uint64_t z180_run_legacy(z180 *c, uint64_t budget)
{
    unsigned long long done;
    zcur = c;
    c->cur_slice = (int)budget;
    cpu_execute_z180((device_t *)c->dev, c->cur_slice);
    done = (unsigned long long)(c->cur_slice - cpu_icount_z180((device_t *)c->dev));
    c->cyc_base += done;
    c->cur_slice = cpu_icount_z180((device_t *)c->dev);   /* so z180_cycles() is cyc_base between slices */
    return done;
}

void z180_set_irq(z180 *c, int line, int asserted)
{
    zcur = c;
    if (line < Z180_INT0 || line > Z180_NMI)
        return;
    /* cpu.h's lines are not z180emu's input numbers: its NMI is INPUT_LINE_NMI, and 3 would be an IRQ state slot
       (test_z180_legacy.c's legacy_nmi_entry; the Braille Lite board raises no NMI, so its goldens never saw it) */
    z180_set_irq_line((device_t *)c->dev, line == Z180_NMI ? INPUT_LINE_NMI : Z180_IRQ0 + line, asserted ? 1 : 0);
}

uint64_t z180_cycles(const z180 *c)
{
    return c->cyc_base + (unsigned long long)(c->cur_slice - cpu_icount_z180((device_t *)c->dev));
}

uint64_t z180_steps(const z180 *c) { return c->steps; }
uint32_t z180_pc(const z180 *c) { return c->pc; }

void z180_regs_get(const z180 *c, z180_regs *out)
{
    struct z180_state *cs = get_safe_token((device_t *)c->dev);
    out->af = cs->_AF; out->bc = cs->_BC; out->de = cs->_DE; out->hl = cs->_HL;
    out->af2 = cs->AF2.w.l; out->bc2 = cs->BC2.w.l; out->de2 = cs->DE2.w.l; out->hl2 = cs->HL2.w.l;
    out->ix = cs->_IX; out->iy = cs->_IY; out->sp = cs->_SP; out->pc = (uint16_t)cs->_PCD;
    out->i = cs->I; out->r = (uint8_t)((cs->R & 0x7F) | (cs->R2 & 0x80)); out->im = cs->IM;
    out->iff1 = cs->IFF1; out->iff2 = cs->IFF2; out->halted = cs->HALT ? 1 : 0; out->sleeping = cs->HALT == 2;
}
