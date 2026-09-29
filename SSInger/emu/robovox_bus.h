/* robovox_bus.h -- the patented system around the chips.
 *
 * Fig. 1: 6502 CPU + ROM/RAM on bus 14, 6850 ACIA MIDI interface 15,
 * 2x74LS245 buffer 16 fanning out to 1-4 SC-02 chips 17, mode switch 21,
 * clock generator 18 (variable XCK = coarse pitch, per the tour rig).
 *
 * Memory map (Phase 1; the 6502 core drops onto bus_cpu_read/write in
 * Phase 2, see ../third_party/README.md):
 *   0x0000-0x07FF  RAM (6116 footprint; the patent's "ROM 12 may be a
 *                  6116" is a part-number slip -- modeled as ROM socket
 *                  + RAM, RESEARCH.md section 1)
 *   0x1000         ACIA status/control, 0x1001 ACIA data
 *   0x1400-0x1413  SC-02 buffer: voice*4 + {R0,R1,R2,R3} at +0..3, R4 at +4
 *                  (writes land in ssi263_write, addrs 0-3 -> R0-R3, 4+ -> R4)
 *   0x1800         mode switch 21 (SEQ/POLY/MONO/SPLIT/FILTER)
 *   0xF800-0xFFFF  ROM socket (clean-room image slot for the Phase-2
 *                  6502-resident translator; IRQ vector at 0xFFFE)
 *
 * The MIDI path is live now: wire bytes enter the emulated 6850,
 * its Rx IRQ drains through the translator, and SC-02 register writes
 * hit the ssi263 engine. Include "ssi263.h" via the engine include path.
 */
#ifndef ROBOVOX_ROBOVOX_BUS_H
#define ROBOVOX_ROBOVOX_BUS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mc6850.h"
#include "robovox_firmware.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "ssi263.h"
#ifdef __cplusplus
}
#endif

/* Bus addresses. */
#define RV_BUS_RAM_BASE 0x0000u
#define RV_BUS_RAM_SIZE 0x0800u
#define RV_BUS_ACIA_STAT 0x1000u
#define RV_BUS_ACIA_DATA 0x1001u
#define RV_BUS_SC_BASE 0x1400u
#define RV_BUS_MODE_SW 0x1800u
#define RV_BUS_ROM_BASE 0xF800u
#define RV_BUS_ROM_SIZE 0x0800u
#define RV_BUS_IRQ_VECTOR 0xFFFEu

typedef enum {
    RV_MODE_SEQ = 0,
    RV_MODE_POLY = 1,
    RV_MODE_MONO = 2,
    RV_MODE_SPLIT = 3,
    RV_MODE_FILTER = 4
} robovox_hw_mode_t;

typedef struct robovox_bus {
    mc6850_t acia;
    uint8_t ram[RV_BUS_RAM_SIZE];
    uint8_t rom[RV_BUS_ROM_SIZE];
    int rom_loaded;
    robovox_hw_mode_t mode_switch;
    int midi2_select;              /* second MIDI channel pair selector */
    int int_ext[RV_NVOICES_MAX];   /* 0 internal / 1 external carrier */
    double xck_hz;                 /* clock generator 18 */
    double sample_rate;
    struct ssi263 *chip[RV_NVOICES_MAX];
    ssi263_params *chip_params; /* engine defaults copy, XCK patched per retune */
    unsigned char chip_rom[576];
    int regs_mirror[RV_NVOICES_MAX][5];
    int nvoices;
    int irq_to_cpu;                /* live 6850 IRQ line level */
    robovox_state_t fw;
    double *render_tmp;
    long render_cap;
} robovox_bus_t;

/* Sink: translator SC-02 writes -> 74LS245 buffer -> engine. Amplitude
 * latches exactly as the translator sends it (velocity -> Amplitude):
 * the real unit has no attack/release envelope, so neither does the bus. */
static void robovox_bus_sc_write(int voice, int addr, int value, void *ctx)
{
    robovox_bus_t *b = (robovox_bus_t *)ctx;
    int a = addr & 7;
    if (voice < 0 || voice >= b->nvoices || !b->chip[voice])
        return;
    if (a >= 4)
        a = 4;
    value &= 0xFF;
    b->regs_mirror[voice][a] = value;
    if (a == 1)
        ssi263_set_snap_pitch(b->chip[voice], 1);
    ssi263_write(b->chip[voice], a, value);
}

/* Sink: master-clock (coarse pitch) changes retune the voices in place.
 * Fresh/idle retune is exact; a phoneme in flight keeps its started length
 * (ssi263.h). */
static void robovox_bus_xck_write(double hz, void *ctx)
{
    robovox_bus_t *b = (robovox_bus_t *)ctx;
    int i;
    if (hz < 100000.0)
        hz = 100000.0;
    if (hz > 4000000.0)
        hz = 4000000.0;
    if (hz == b->xck_hz)
        return;
    b->xck_hz = hz;
    b->fw.cfg.xck_hz = hz;
    for (i = 0; i < b->nvoices; i++)
        if (b->chip[i])
            ssi263_set_xck(b->chip[i], hz);
}

/* Master-knob base clock in: the effective clock (base through the current
 * Polaxis bend, if any) hits the chips. Tracked against the nominal base
 * (not the effective clock) so the VST's per-block param poll never fights
 * a held bend. */
static void robovox_bus_master_write(double hz, void *ctx)
{
    robovox_bus_t *b = (robovox_bus_t *)ctx;
    if (hz < 100000.0)
        hz = 100000.0;
    if (hz > 4000000.0)
        hz = 4000000.0;
    if (hz == b->fw.nominal_xck)
        return;
    robovox_set_nominal_xck(&b->fw, hz);
}

static void robovox_bus_on_irq(int level, void *ctx)
{
    robovox_bus_t *b = (robovox_bus_t *)ctx;
    b->irq_to_cpu = level;
    /* Phase-1 firmware services the ACIA synchronously (the 6502 ISR
     * equivalent): drain every pending byte through the translator. */
    while (mc6850_read_status(&b->acia) & M6850_SR_RDRF)
        robovox_midi_byte(&b->fw, mc6850_read_data(&b->acia));
}

static int robovox_bus_build_chips(robovox_bus_t *b)
{
    int i, a;
    int saved[RV_NVOICES_MAX][5];
    for (i = 0; i < RV_NVOICES_MAX; i++)
        for (a = 0; a < 5; a++)
            saved[i][a] = b->regs_mirror[i][a];
    for (i = 0; i < b->nvoices; i++) {
        ssi263_params *p;
        if (b->chip[i])
            ssi263_free(b->chip[i]);
        b->chip[i] = 0;
        p = b->chip_params;
        p->xck_hz = b->xck_hz; /* xck_hz is the first params field (ssi263.h) */
        b->chip[i] = ssi263_new(p, b->chip_rom, b->sample_rate);
        if (!b->chip[i])
            return 0;
        /* Power up: CTL 1->0 latches mode (ssi263.c:461-467). */
        ssi263_write(b->chip[i], 0, saved[i][0]);
        ssi263_write(b->chip[i], 1, saved[i][1]);
        ssi263_write(b->chip[i], 2, saved[i][2]);
        ssi263_write(b->chip[i], 4, saved[i][4]);
        ssi263_write(b->chip[i], 3, saved[i][3] & 0x7F);
        b->regs_mirror[i][3] &= 0x7F;
    }
    return 1;
}

static int robovox_bus_init(robovox_bus_t *b, double sample_rate, int nvoices,
                            const ssi263_params *defaults,
                            const unsigned char *rom576)
{
    int i, a;
    memset(b, 0, sizeof(*b));
    if (nvoices < 1)
        nvoices = 1;
    if (nvoices > RV_NVOICES_MAX)
        nvoices = RV_NVOICES_MAX;
    b->nvoices = nvoices;
    b->sample_rate = sample_rate;
    b->xck_hz = 1000000.0;
    b->mode_switch = RV_MODE_SEQ;
    mc6850_init(&b->acia);
    b->acia.irq_cb = robovox_bus_on_irq;
    b->acia.irq_ctx = b;
    /* Firmware power-on: 6850 master reset + 8N1 /16 + Rx IRQ, the way
     * any 6502 MIDI firmware starts. */
    mc6850_write_ctrl(&b->acia, 0x03);
    mc6850_write_ctrl(&b->acia, 0x95);
    robovox_default_cfg(&b->fw.cfg);
    b->fw.cfg.nvoices = nvoices;
    b->fw.cfg.xck_hz = b->xck_hz;
    b->fw.sc_write = robovox_bus_sc_write;
    b->fw.xck_write = robovox_bus_xck_write;
    b->fw.sink_ctx = b;
    robovox_default_table(&b->fw);
    robovox_reset(&b->fw);
    /* Engine params/ROM (caller passes ssi263_default_params/rom). */
    b->chip_params = (ssi263_params *)malloc(defaults ? sizeof(*defaults) : 0);
    if (defaults && !b->chip_params)
        return 0;
    if (defaults)
        memcpy(b->chip_params, defaults, sizeof(*defaults));
    if (rom576)
        memcpy(b->chip_rom, rom576, sizeof(b->chip_rom));
    for (i = 0; i < RV_NVOICES_MAX; i++)
        for (a = 0; a < 5; a++)
            b->regs_mirror[i][a] = 0;
    /* Sane power-on register set: DUR from cfg, tone FF, mid rate/art. */
    for (i = 0; i < nvoices; i++) {
        b->regs_mirror[i][0] = (b->fw.cfg.dur & 3) << 6;
        b->regs_mirror[i][2] = (b->fw.cfg.rate & 15) << 4;
        b->regs_mirror[i][3] = (b->fw.cfg.articulation & 7) << 4;
        b->regs_mirror[i][4] = b->fw.cfg.filter_ff;
    }
    if (!robovox_bus_build_chips(b))
        return 0;
    b->render_cap = 4096;
    b->render_tmp = (double *)malloc(sizeof(double) * (size_t)(b->render_cap + 1));
    if (!b->render_tmp)
        return 0;
    return 1;
}

static void robovox_bus_free(robovox_bus_t *b)
{
    int i;
    for (i = 0; i < RV_NVOICES_MAX; i++) {
        if (b->chip[i])
            ssi263_free(b->chip[i]);
        b->chip[i] = 0;
    }
    free(b->chip_params);
    b->chip_params = 0;
    free(b->render_tmp);
    b->render_tmp = 0;
}

/* MIDI wire byte in (from the plugin's MIDI input). */
static void robovox_bus_midi_byte(robovox_bus_t *b, uint8_t byte)
{
    mc6850_receive_byte(&b->acia, byte, 0);
}

/* Service a voice's A/R request (sustain sung notes, firmware-style). */
static void robovox_bus_service(robovox_bus_t *b, int voice)
{
    if (voice < 0 || voice >= b->nvoices || !b->chip[voice])
        return;
    if (ssi263_request(b->chip[voice]))
        robovox_service_request(&b->fw, voice);
}

static void robovox_bus_service_all(robovox_bus_t *b)
{
    int i;
    for (i = 0; i < b->nvoices; i++)
        robovox_bus_service(b, i);
}

/* Render n samples, mixed mono. Returns samples written. */
static long robovox_bus_render(robovox_bus_t *b, long n, double *out)
{
    long i, v, got;
    if (!b->render_tmp)
        return 0;
    if (n > b->render_cap) {
        double *nt = (double *)realloc(b->render_tmp,
                                       sizeof(double) * (size_t)(n + 1));
        if (!nt)
            return 0;
        b->render_tmp = nt;
        b->render_cap = n;
    }
    for (i = 0; i < n; i++)
        out[i] = 0.0;
    for (v = 0; v < b->nvoices; v++) {
        if (!b->chip[v])
            continue;
        got = ssi263_run(b->chip[v], n, b->render_tmp);
        for (i = 0; i < got; i++)
            out[i] += b->render_tmp[i] / b->nvoices;
    }
    return n;
}

/* Phase-2 CPU socket: byte access for the 6502 core. Exercised by the
 * C tests through a test double; floooh m6502.h hooks these directly. */
static uint8_t robovox_bus_cpu_read(robovox_bus_t *b, uint16_t addr)
{
    if (addr < RV_BUS_RAM_SIZE)
        return b->ram[addr];
    if (addr == RV_BUS_ACIA_STAT)
        return mc6850_read_status(&b->acia);
    if (addr == RV_BUS_ACIA_DATA)
        return mc6850_read_data(&b->acia);
    if (addr >= RV_BUS_SC_BASE && addr < RV_BUS_SC_BASE + 16) {
        int voice = (addr - RV_BUS_SC_BASE) / 4;
        int reg = (addr - RV_BUS_SC_BASE) % 4;
        if (voice < b->nvoices && reg < 5)
            return (uint8_t)b->regs_mirror[voice][reg];
        return 0xFF;
    }
    if (addr == RV_BUS_MODE_SW)
        return (uint8_t)b->mode_switch;
    if (addr >= RV_BUS_ROM_BASE)
        return b->rom[addr - RV_BUS_ROM_BASE];
    return 0xFF;
}

static void robovox_bus_cpu_write(robovox_bus_t *b, uint16_t addr, uint8_t v)
{
    if (addr < RV_BUS_RAM_SIZE) {
        b->ram[addr] = v;
        return;
    }
    if (addr == RV_BUS_ACIA_STAT) {
        mc6850_write_ctrl(&b->acia, v);
        return;
    }
    if (addr == RV_BUS_ACIA_DATA) {
        mc6850_write_data(&b->acia, v);
        return;
    }
    if (addr >= RV_BUS_SC_BASE && addr < RV_BUS_SC_BASE + 16) {
        int voice = (addr - RV_BUS_SC_BASE) / 4;
        int reg = (addr - RV_BUS_SC_BASE) % 4;
        robovox_bus_sc_write(voice, reg, v, b);
        return;
    }
}

#endif /* ROBOVOX_ROBOVOX_BUS_H */
