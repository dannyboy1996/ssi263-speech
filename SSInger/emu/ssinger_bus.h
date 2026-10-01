/* ssinger_bus.h -- the patented system around the chips.
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
#ifndef SSINGER_BUS_H
#define SSINGER_BUS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mc6850.h"
#include "ssinger_firmware.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "ssi263.h"
#ifdef __cplusplus
}
#endif

/* Bus addresses. */
#define SG_BUS_RAM_BASE 0x0000u
#define SG_BUS_RAM_SIZE 0x0800u
#define SG_BUS_ACIA_STAT 0x1000u
#define SG_BUS_ACIA_DATA 0x1001u
#define SG_BUS_SC_BASE 0x1400u
#define SG_BUS_MODE_SW 0x1800u
#define SG_BUS_ROM_BASE 0xF800u
#define SG_BUS_ROM_SIZE 0x0800u
#define SG_BUS_IRQ_VECTOR 0xFFFEu

typedef enum {
    SG_MODE_SEQ = 0,
    SG_MODE_POLY = 1,
    SG_MODE_MONO = 2,
    SG_MODE_SPLIT = 3,
    SG_MODE_FILTER = 4
} ssinger_hw_mode_t;

typedef struct ssinger_bus {
    mc6850_t acia;
    uint8_t ram[SG_BUS_RAM_SIZE];
    uint8_t rom[SG_BUS_ROM_SIZE];
    int rom_loaded;
    ssinger_hw_mode_t mode_switch;
    int midi2_select;              /* second MIDI channel pair selector */
    int int_ext[SG_NVOICES_MAX];   /* 0 internal / 1 external carrier */
    double xck_hz;                 /* clock generator 18 */
    double sample_rate;
    struct ssi263 *chip[SG_NVOICES_MAX];
    ssi263_params *chip_params; /* engine defaults copy, XCK patched per retune */
    unsigned char chip_rom[576];
    int regs_mirror[SG_NVOICES_MAX][5];
    int nvoices;
    int irq_to_cpu;                /* live 6850 IRQ line level */
    ssinger_state_t fw;
    double *render_tmp;
    long render_cap;
    /* Filter-frequency slew (de-zipper): the translator targets R4 at
     * once, the chip follows at SG_FF_SLEW_PER_SEC. R4 is an 8-bit
     * register on a reciprocal curve (fc = XCK/(2(256-FF))), so single
     * mod-wheel clicks would otherwise jump the cutoff by hundreds of
     * Hz mid-sweep. */
    float ff_cur[SG_NVOICES_MAX]; /* continuous, what the chip has */
    int ff_tgt[SG_NVOICES_MAX];   /* what the translator asked for */
    int ff_out[SG_NVOICES_MAX];   /* last R4 value sent to the chip */
    long svc_phase;               /* samples since the last service tick */
} ssinger_bus_t;

#define SG_FF_SLEW_PER_SEC 2550.0 /* full 0..255 sweep in ~100 ms */

/* Service tick: the firmware's A/R service and the filter slew run every
 * SG_SERVICE_SAMPLES output samples, counted from power-on, never at the
 * host's block edges -- so a song renders the same at any block size
 * (64, 512, 4096, or REAPER's offline render). 32 samples is 0.7 ms at
 * 44.1 kHz, 0.33 ms at 96 kHz. */
#define SG_SERVICE_SAMPLES 32

/* Sink: translator SC-02 writes -> 74LS245 buffer -> engine. Amplitude
 * latches exactly as the translator sends it (velocity -> Amplitude):
 * the real unit has no attack/release envelope, so neither does the bus. */
static void ssinger_bus_sc_write(int voice, int addr, int value, void *ctx)
{
    ssinger_bus_t *b = (ssinger_bus_t *)ctx;
    int a = addr & 7;
    if (voice < 0 || voice >= b->nvoices || !b->chip[voice])
        return;
    if (a >= 4)
        a = 4;
    value &= 0xFF;
    if (a == 4) {
        /* Filter target latches; the slew below walks the chip to it so
         * wheel/bend sweeps glide instead of zippering. */
        b->regs_mirror[voice][a] = value;
        b->ff_tgt[voice] = value;
        return;
    }
    b->regs_mirror[voice][a] = value;
    if (a == 1)
        ssi263_set_snap_pitch(b->chip[voice], 1);
    ssi263_write(b->chip[voice], a, value);
}

/* Walk each chip's R4 toward its target (called per render chunk). */
static void ssinger_bus_slew_filters(ssinger_bus_t *b, double seconds)
{
    int v;
    double step;
    if (!(seconds > 0.0))
        return;
    step = SG_FF_SLEW_PER_SEC * seconds;
    for (v = 0; v < b->nvoices; v++) {
        float cur = b->ff_cur[v], tgt = (float)b->ff_tgt[v];
        int out;
        if (cur == tgt)
            continue;
        if (tgt > cur) {
            cur += (float)step;
            if (cur > tgt)
                cur = tgt;
        } else {
            cur -= (float)step;
            if (cur < tgt)
                cur = tgt;
        }
        b->ff_cur[v] = cur;
        out = (int)(cur + 0.5f);
        if (out != b->ff_out[v]) {
            /* Mirror keeps the translator's target; ff_out tracks the chip. */
            b->ff_out[v] = out;
            if (b->chip[v])
                ssi263_write(b->chip[v], 4, out);
        }
    }
}

/* Sink: master-clock (coarse pitch) changes retune the voices in place.
 * Fresh/idle retune is exact; a phoneme in flight keeps its started length
 * (ssi263.h). */
static void ssinger_bus_xck_write(double hz, void *ctx)
{
    ssinger_bus_t *b = (ssinger_bus_t *)ctx;
    int i;
    if (hz < 100000.0)
        hz = 100000.0;
    if (hz > 4000000.0)
        hz = 4000000.0;
    if (hz == b->xck_hz)
        return;
    b->xck_hz = hz;
    /* Only the chips' clock moves.  The translator keeps tuning new notes
     * against its fixed reference clock (fw.cfg.xck_hz), so they transpose
     * with the bend or the Master clock exactly as held notes do. */
    for (i = 0; i < b->nvoices; i++)
        if (b->chip[i])
            ssi263_set_xck(b->chip[i], hz);
}

/* Master-knob base clock in: the effective clock (base through the current
 * Polaxis bend, if any) hits the chips. Tracked against the nominal base
 * (not the effective clock) so the VST's per-block param poll never fights
 * a held bend. */
static void ssinger_bus_master_write(double hz, void *ctx)
{
    ssinger_bus_t *b = (ssinger_bus_t *)ctx;
    if (hz < 100000.0)
        hz = 100000.0;
    if (hz > 4000000.0)
        hz = 4000000.0;
    if (hz == b->fw.nominal_xck)
        return;
    ssinger_set_nominal_xck(&b->fw, hz);
}

static void ssinger_bus_on_irq(int level, void *ctx)
{
    ssinger_bus_t *b = (ssinger_bus_t *)ctx;
    b->irq_to_cpu = level;
    /* Phase-1 firmware services the ACIA synchronously (the 6502 ISR
     * equivalent): drain every pending byte through the translator. */
    while (mc6850_read_status(&b->acia) & M6850_SR_RDRF)
        ssinger_midi_byte(&b->fw, mc6850_read_data(&b->acia));
}

static int ssinger_bus_build_chips(ssinger_bus_t *b)
{
    int i, a;
    int saved[SG_NVOICES_MAX][5];
    for (i = 0; i < SG_NVOICES_MAX; i++)
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
        /* Chip rebuilt at the translator's latched filter target. */
        b->ff_cur[i] = (float)b->regs_mirror[i][4];
        b->ff_tgt[i] = b->regs_mirror[i][4];
        b->ff_out[i] = b->regs_mirror[i][4];
    }
    return 1;
}

static int ssinger_bus_init(ssinger_bus_t *b, double sample_rate, int nvoices,
                            const ssi263_params *defaults,
                            const unsigned char *rom576)
{
    int i, a;
    memset(b, 0, sizeof(*b));
    if (nvoices < 1)
        nvoices = 1;
    if (nvoices > SG_NVOICES_MAX)
        nvoices = SG_NVOICES_MAX;
    b->nvoices = nvoices;
    b->sample_rate = sample_rate;
    b->xck_hz = 1000000.0;
    b->mode_switch = SG_MODE_SEQ;
    mc6850_init(&b->acia);
    b->acia.irq_cb = ssinger_bus_on_irq;
    b->acia.irq_ctx = b;
    /* Firmware power-on: 6850 master reset + 8N1 /16 + Rx IRQ, the way
     * any 6502 MIDI firmware starts. */
    mc6850_write_ctrl(&b->acia, 0x03);
    mc6850_write_ctrl(&b->acia, 0x95);
    ssinger_default_cfg(&b->fw.cfg);
    b->fw.cfg.nvoices = nvoices;
    b->fw.cfg.xck_hz = b->xck_hz;
    b->fw.sc_write = ssinger_bus_sc_write;
    b->fw.xck_write = ssinger_bus_xck_write;
    b->fw.sink_ctx = b;
    ssinger_default_table(&b->fw);
    ssinger_reset(&b->fw);
    /* Engine params/ROM (caller passes ssi263_default_params/rom). */
    b->chip_params = (ssi263_params *)malloc(defaults ? sizeof(*defaults) : 0);
    if (defaults && !b->chip_params)
        return 0;
    if (defaults)
        memcpy(b->chip_params, defaults, sizeof(*defaults));
    if (rom576)
        memcpy(b->chip_rom, rom576, sizeof(b->chip_rom));
    for (i = 0; i < SG_NVOICES_MAX; i++)
        for (a = 0; a < 5; a++)
            b->regs_mirror[i][a] = 0;
    /* Sane power-on register set: DUR from cfg, tone FF, mid rate/art. */
    for (i = 0; i < nvoices; i++) {
        b->regs_mirror[i][0] = (b->fw.cfg.dur & 3) << 6;
        b->regs_mirror[i][2] = (b->fw.cfg.rate & 15) << 4;
        b->regs_mirror[i][3] = (b->fw.cfg.articulation & 7) << 4;
        b->regs_mirror[i][4] = b->fw.v[i].filter_ff; /* FF + the chip's tour-rig offset */
    }
    for (i = 0; i < SG_NVOICES_MAX; i++) {
        b->ff_cur[i] = (float)b->regs_mirror[i][4];
        b->ff_tgt[i] = b->regs_mirror[i][4];
        b->ff_out[i] = b->regs_mirror[i][4];
    }
    if (!ssinger_bus_build_chips(b))
        return 0;
    b->render_cap = 4096;
    b->render_tmp = (double *)malloc(sizeof(double) * (size_t)(b->render_cap + 1));
    if (!b->render_tmp)
        return 0;
    return 1;
}

static void ssinger_bus_free(ssinger_bus_t *b)
{
    int i;
    for (i = 0; i < SG_NVOICES_MAX; i++) {
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
static void ssinger_bus_midi_byte(ssinger_bus_t *b, uint8_t byte)
{
    mc6850_receive_byte(&b->acia, byte, 0);
}

/* Service a voice's A/R request (sustain sung notes, firmware-style). */
static void ssinger_bus_service(ssinger_bus_t *b, int voice)
{
    if (voice < 0 || voice >= b->nvoices || !b->chip[voice])
        return;
    if (ssi263_request(b->chip[voice]))
        ssinger_service_request(&b->fw, voice);
}

static void ssinger_bus_service_all(ssinger_bus_t *b)
{
    int i;
    for (i = 0; i < b->nvoices; i++)
        ssinger_bus_service(b, i);
}

/* Tour-rig mix: the chips add at full level (one chip sings exactly as
 * loud as SEQ), and a fixed soft knee keeps four chips in unison under
 * 0 dBFS. Memoryless (no envelope, so nothing pumps): linear up to
 * SG_MIX_KNEE, which one chip at full velocity stays under (-4.2 dBFS
 * peak measured), then a tanh shoulder that never reaches SG_MIX_CEIL.
 * 0.7.0 divided by the chip count instead: one chip of four sang 12 dB
 * down (spacepup: "much quieter than solo"). SEQ (one chip) is never
 * touched. */
#define SG_MIX_KNEE 0.70  /* -3.1 dBFS */
#define SG_MIX_CEIL 0.95  /* -0.45 dBFS */
static double ssinger_mix_knee(double x)
{
    const double a = fabs(x), room = SG_MIX_CEIL - SG_MIX_KNEE;
    double y;
    if (a <= SG_MIX_KNEE)
        return x;
    y = SG_MIX_KNEE + room * tanh((a - SG_MIX_KNEE) / room);
    return x < 0.0 ? -y : y;
}

/* Render n samples, mixed mono. Returns samples written. */
static long ssinger_bus_render(ssinger_bus_t *b, long n, double *out)
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
            out[i] += b->render_tmp[i];
    }
    if (b->nvoices > 1)
        for (i = 0; i < n; i++)
            out[i] = ssinger_mix_knee(out[i]);
    return n;
}

/* Render n samples (mixed mono) with the firmware loop running on the
 * service tick (SG_SERVICE_SAMPLES): A/R sustain and filter slew happen at
 * the same sample positions whatever n is, so splitting a span into any
 * pieces gives the same samples. Feed MIDI bytes between calls at their
 * sample positions. Never allocates (each chip pass is <= 32 samples, far
 * under render_cap). This is the plugin's render loop; the tests run it. */
static long ssinger_bus_run(ssinger_bus_t *b, long n, double *out)
{
    long done = 0;
    while (done < n) {
        long k = SG_SERVICE_SAMPLES - b->svc_phase;
        if (k > n - done)
            k = n - done;
        ssinger_bus_render(b, k, out + done);
        done += k;
        b->svc_phase += k;
        if (b->svc_phase >= SG_SERVICE_SAMPLES) {
            b->svc_phase = 0;
            ssinger_bus_slew_filters(b, SG_SERVICE_SAMPLES / b->sample_rate);
            ssinger_bus_service_all(b);
        }
    }
    return n;
}

/* Phase-2 CPU socket: byte access for the 6502 core. Exercised by the
 * C tests through a test double; floooh m6502.h hooks these directly. */
static uint8_t ssinger_bus_cpu_read(ssinger_bus_t *b, uint16_t addr)
{
    if (addr < SG_BUS_RAM_SIZE)
        return b->ram[addr];
    if (addr == SG_BUS_ACIA_STAT)
        return mc6850_read_status(&b->acia);
    if (addr == SG_BUS_ACIA_DATA)
        return mc6850_read_data(&b->acia);
    if (addr >= SG_BUS_SC_BASE && addr < SG_BUS_SC_BASE + 16) {
        int voice = (addr - SG_BUS_SC_BASE) / 4;
        int reg = (addr - SG_BUS_SC_BASE) % 4;
        if (voice < b->nvoices && reg < 5)
            return (uint8_t)b->regs_mirror[voice][reg];
        return 0xFF;
    }
    if (addr == SG_BUS_MODE_SW)
        return (uint8_t)b->mode_switch;
    if (addr >= SG_BUS_ROM_BASE)
        return b->rom[addr - SG_BUS_ROM_BASE];
    return 0xFF;
}

static void ssinger_bus_cpu_write(ssinger_bus_t *b, uint16_t addr, uint8_t v)
{
    if (addr < SG_BUS_RAM_SIZE) {
        b->ram[addr] = v;
        return;
    }
    if (addr == SG_BUS_ACIA_STAT) {
        mc6850_write_ctrl(&b->acia, v);
        return;
    }
    if (addr == SG_BUS_ACIA_DATA) {
        mc6850_write_data(&b->acia, v);
        return;
    }
    if (addr >= SG_BUS_SC_BASE && addr < SG_BUS_SC_BASE + 16) {
        int voice = (addr - SG_BUS_SC_BASE) / 4;
        int reg = (addr - SG_BUS_SC_BASE) % 4;
        ssinger_bus_sc_write(voice, reg, v, b);
        return;
    }
}

#endif /* SSINGER_BUS_H */
