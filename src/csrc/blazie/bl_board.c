/* bl_board.c -- the Blazie Braille Lite 2000 board around a Z180, per instance (see bl_board.h).
 *
 * z180emu/bns.c's --live path, line for line, with the tracing and logging left out.  The board drives its CPU only
 * through ../cpu/cpu.h: every callback gets its bl_unit as ctx, so there is no shared or thread-local state here.
 * Today the Z180 behind it is z180emu's, on the legacy path (../cpu/z180_legacy.c), which also carries what the board
 * used to reach into -- the ASCI's baud ticking and request level, /DCD0 -- so the golden vectors hold bit for bit.
 *
 * Memory: ROM image from file offset 3000h at physical 00000h (256 KB, FFh-padded); 40000h..FFFFFh RAM, except that
 * port E0h bit 3 maps the AMD 29F040-style file flash over 80000h..FFFFFh.  SSI-263 at ports C0h..C4h, its A/R
 * request on /INT1.  Braille keyboard on port 40h, /INT2.  Serial on ASCI0 (9600 bit/s at 6.144 MHz), with the
 * host honouring the unit's XON/XOFF.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../cpu/cpu.h"
#include "bl_board.h"

#define ROM_SIZE 0x40000
#define FILE_ROM_OFFSET 0x3000
#define SLICE_DEFAULT 10000
#define FLASH_BASE 0x80000
#define MAX_KEYS 64
#define FIFO 0x10000

struct bl_unit {
    unsigned char *flash, *ram, *fflash;
    z180 *cpu;
    double clock_hz, phon_ms;
    int irq_line;
    unsigned char ssi_ready_value, ssi[5];
    int ssi_ctl, ssi_ar, ssi_mode, live_on;
    unsigned long long ssi_ready_at;
    unsigned char *lfifo;
    unsigned lhead, ltail;
    int urgent, host_xoff;
    unsigned long long key_at[MAX_KEYS];
    unsigned char key_val[MAX_KEYS], key_latch;
    int n_keys, next_key, key_latched, hold_chord;
    unsigned char live_keys[16];             /* bl_key: chords pressed live, delivered one per boundary when free */
    int n_live_keys;
    int ff_state, ff_autoselect;
    unsigned char port_e0;
    unsigned site_release;
    uint32_t instr_pc;
    bl_event *ev;
    int n_ev, cap_ev;
};

static void event(bl_unit *u, unsigned char type, unsigned char a, unsigned char b)
{
    if (u->n_ev == u->cap_ev) {
        int cap = u->cap_ev ? u->cap_ev * 2 : 256;
        bl_event *e = (bl_event *)realloc(u->ev, (size_t)cap * sizeof(bl_event));
        if (!e)
            return;
        u->ev = e;
        u->cap_ev = cap;
    }
    u->ev[u->n_ev].type = type;
    u->ev[u->n_ev].a = a;
    u->ev[u->n_ev].b = b;
    u->n_ev++;
}

static void ar_line(bl_unit *u)             /* A/R request drives the INT line (level, active = asserted) */
{
    if (u->irq_line >= 0)
        z180_set_irq(u->cpu, u->irq_line, u->ssi_ar && u->ssi_mode ? 1 : 0);
}

static int flash_window(const bl_unit *u)
{
    return (u->port_e0 & 0x08) != 0;
}

static unsigned char fflash_read(const bl_unit *u, uint32_t off)
{
    if (u->ff_autoselect)
        return (off & 3) == 0 ? 0x01 : (off & 3) == 1 ? 0xA4 : 0x00;   /* AMD, Am29F040 */
    return u->fflash[off];
}

static void fflash_write(bl_unit *u, uint32_t off, unsigned char V)
{
    unsigned a = off & 0x7FFF;
    switch (u->ff_state) {
    case 0: if (V == 0xF0) { u->ff_autoselect = 0; return; }
            u->ff_state = (a == 0x5555 && V == 0xAA) ? 1 : 0; break;
    case 1: u->ff_state = (a == 0x2AAA && V == 0x55) ? 2 : 0; break;
    case 2: u->ff_state = 0;
            if (a != 0x5555) break;
            if (V == 0xA0) u->ff_state = 3;
            else if (V == 0x80) u->ff_state = 4;
            else if (V == 0x90) u->ff_autoselect = 1;
            else if (V == 0xF0) u->ff_autoselect = 0;
            break;
    case 3: u->fflash[off] &= V; u->ff_state = 0; break;
    case 4: u->ff_state = (a == 0x5555 && V == 0xAA) ? 5 : 0; break;
    case 5: u->ff_state = (a == 0x2AAA && V == 0x55) ? 6 : 0; break;
    case 6: u->ff_state = 0;
            if (V == 0x10)
                memset(u->fflash, 0xFF, 0x80000);
            else if (V == 0x30)
                memset(u->fflash + (off & 0x70000), 0xFF, 0x10000);
            break;
    }
}

/* ---- the bus the CPU sees (cpu.h) -------------------------------------------------------------------------------- */
static uint8_t mem_read(void *ctx, uint32_t A)
{
    bl_unit *u = (bl_unit *)ctx;
    A &= 0xFFFFF;
    if (A < ROM_SIZE) return u->flash[A];
    if (A >= FLASH_BASE && flash_window(u)) return fflash_read(u, A - FLASH_BASE);
    return u->ram[A];
}

static void mem_write(void *ctx, uint32_t A, uint8_t V)
{
    bl_unit *u = (bl_unit *)ctx;
    A &= 0xFFFFF;
    if (A < ROM_SIZE)
        return;                              /* ROM: bns.c counts and logs these; after a hard reset there are none */
    if (A >= FLASH_BASE && flash_window(u)) {
        fflash_write(u, A - FLASH_BASE, V);
        return;
    }
    u->ram[A] = V;
}

static uint8_t io_read(void *ctx, uint16_t Port)
{
    bl_unit *u = (bl_unit *)ctx;
    int p = Port & 0xFF;
    unsigned char v;
    if (p >= 0xC0 && p <= 0xC4)
        return u->ssi_ar ? u->ssi_ready_value : (unsigned char)(u->ssi_ready_value ^ 0x80);
    v = p == 0x40 ? 0x00 : 0xFF;
    if (p == 0x40 && u->hold_chord >= 0) {
        if (u->instr_pc == u->site_release || u->instr_pc == u->site_release + 8)
            u->hold_chord = -1;              /* the firmware now waits for release */
        else
            v = (unsigned char)u->hold_chord;
    }
    if (p == 0x40 && u->key_latched) {
        v = u->key_latch;
        u->key_latched = 0;
        z180_set_irq(u->cpu, Z180_INT2, 0);
    }
    return v;
}

static void io_write(void *ctx, uint16_t Port, uint8_t V)
{
    bl_unit *u = (bl_unit *)ctx;
    int p = Port & 0xFF;
    if (p == 0xE0)
        u->port_e0 = V;
    if (p >= 0xC0 && p <= 0xC4) {
        int reg = p - 0xC0;
        unsigned long long cyc = z180_cycles(u->cpu);
        u->ssi[reg] = V;
        if (reg == 3)
            u->ssi_ctl = V >> 7;
        event(u, 'W', (unsigned char)reg, V);
        if (reg == 0 && u->ssi_ctl) {
            u->ssi_mode = V >> 6;
            ar_line(u);
        } else if (reg == 0) {
            u->ssi_ar = 0;
            u->ssi_ready_at = u->live_on ? ~0ULL
                            : cyc + (unsigned long long)(u->phon_ms * u->clock_hz / 1000.0);
            ar_line(u);
        }
    }
}

static int asci_rx(void *ctx, int channel)
{
    bl_unit *u = (bl_unit *)ctx;
    int b;
    if (u->live_on) {
        if (channel == 0 && u->urgent >= 0) {
            b = u->urgent;
            u->urgent = -1;
            return b;
        }
        if (channel != 0 || u->lhead == u->ltail || u->host_xoff)
            return -1;
        return u->lfifo[u->ltail++ & (FIFO - 1)];
    }
    return -1;                               /* before live mode nothing is queued (bns: no --serial in live runs) */
}

static void asci_tx(void *ctx, int channel, uint8_t data)
{
    bl_unit *u = (bl_unit *)ctx;
    if (channel != 0)
        return;
    event(u, 'T', data, 0);
    if (data == 0x13)
        u->host_xoff = 1;
    else if (data == 0x11)
        u->host_xoff = 0;
}

static int serial_pin(void *ctx, int pin)
{
    (void)ctx;
    return pin == Z180_PIN_DCD0 ? 1 : 0;     /* /DCD0 = carrier present (see bns.c) */
}

/* Every step's boundary (cpu.h phase D), after the core has counted the step and caught its ASCI up. */
static void boundary(void *ctx, uint32_t pc)
{
    bl_unit *u = (bl_unit *)ctx;
    u->instr_pc = pc & 0xFFFF;               /* read by the key-release check (port reads) */
    if (!u->live_on && !u->ssi_ar && z180_cycles(u->cpu) >= u->ssi_ready_at) {
        u->ssi_ar = 1;
        ar_line(u);
    }
    if (u->next_key < u->n_keys && z180_steps(u->cpu) >= u->key_at[u->next_key]) {
        if (u->hold_chord >= 0)
            u->hold_chord = -1;              /* a hand pressing a key has let go of the power-on chord */
        u->key_latch = u->key_val[u->next_key++];
        u->key_latched = 1;
        z180_set_irq(u->cpu, Z180_INT2, 1);
    } else if (u->n_live_keys && !u->key_latched) {  /* a live chord, once the last one has been read */
        u->hold_chord = -1;
        u->key_latch = u->live_keys[0];
        memmove(u->live_keys, u->live_keys + 1, (size_t)--u->n_live_keys);
        u->key_latched = 1;
        z180_set_irq(u->cpu, Z180_INT2, 1);
    }
}

/* a byte signature ("F6 40 D3 C0", "??" = any byte) in the ROM image: the offset of the unique match plus `off`,
   or 0 if absent or ambiguous (bns.c's detect_sites) */
static unsigned find_sig(const bl_unit *u, const char *sig, int off, long img_len)
{
    int pat[32], n = 0, hits = 0;
    unsigned at = 0;
    long i;
    const char *s = sig;
    while (*s && n < 32) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') pat[n++] = -1;
        else { unsigned v; sscanf(s, "%2x", &v); pat[n++] = (int)v; }
        s += 2;
    }
    for (i = 0; i + n <= img_len; i++) {
        int k;
        for (k = 0; k < n; k++)
            if (pat[k] >= 0 && u->flash[i + k] != pat[k]) break;
        if (k == n) { hits++; at = (unsigned)i + off; }
    }
    return hits == 1 ? at : 0;
}

bl_unit *bl_create(const char *firmware, const char *state, double phon_ms,
                   const unsigned long long *key_at, const unsigned char *key_val, int n_keys,
                   char *err, int errlen)
{
    bl_unit *u = (bl_unit *)calloc(1, sizeof(bl_unit));
    FILE *f;
    long n;
    int i;
    cpu_bus bus;
    if (!u) { snprintf(err, errlen, "out of memory"); return NULL; }
    u->flash = (unsigned char *)malloc(ROM_SIZE);
    u->ram = (unsigned char *)calloc(1, 0x100000);
    u->fflash = (unsigned char *)malloc(0x80000);
    u->lfifo = (unsigned char *)malloc(FIFO);
    if (!u->flash || !u->ram || !u->fflash || !u->lfifo) { snprintf(err, errlen, "out of memory"); bl_destroy(u); return NULL; }
    u->clock_hz = 6144000.0;
    u->phon_ms = phon_ms;
    u->irq_line = Z180_INT1;
    u->ssi_ready_value = 0x80;
    u->ssi_ar = 1;
    u->urgent = -1;
    u->hold_chord = -1;
    for (i = 0; i < n_keys && i < MAX_KEYS; i++) {
        u->key_at[i] = key_at[i];
        u->key_val[i] = key_val[i];
    }
    u->n_keys = n_keys < MAX_KEYS ? n_keys : MAX_KEYS;
    memset(u->flash, 0xFF, ROM_SIZE);
    memset(u->fflash, 0xFF, 0x80000);
    if (state) {                             /* battery-backed RAM + file flash, as a real unit keeps them */
        FILE *s = fopen(state, "rb");
        if (!s || fread(u->ram + 0x40000, 1, 0x40000, s) != 0x40000 || fread(u->fflash, 1, 0x80000, s) != 0x80000) {
            if (s) fclose(s);
            snprintf(err, errlen, "cannot read state %s", state);
            bl_destroy(u);
            return NULL;
        }
        fclose(s);
    }
    f = fopen(firmware, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", firmware); bl_destroy(u); return NULL; }
    fseek(f, FILE_ROM_OFFSET, SEEK_SET);
    n = (long)fread(u->flash, 1, ROM_SIZE, f);
    fclose(f);
    {                                        /* bns.c refuses a firmware without these sites; so does this */
        unsigned send = find_sig(u, "F6 40 D3 C0", 2, n);
        unsigned prime = find_sig(u, "F6 E0 D3 C4 CD ?? ?? 3E C0 D3 C0", 9, n);
        unsigned fetch = find_sig(u, "2A 17 D6 7E 23 22 17 D6", 3, n);
        unsigned halt = find_sig(u, "21 ?? ?? 7E B7 20 06 76 CD", 7, n);
        u->site_release = find_sig(u, "DB 40 D3 20 E6 7F 20 F8", 0, n);
        if (!(send && prime && fetch && halt && u->site_release)) {
            snprintf(err, errlen, "could not locate the firmware sites in %s", firmware);
            bl_destroy(u);
            return NULL;
        }
    }
    memset(&bus, 0, sizeof bus);
    bus.ctx = u;
    bus.read = mem_read;
    bus.write = mem_write;
    bus.in = io_read;
    bus.out = io_write;
    bus.serial_rx = asci_rx;
    bus.serial_tx = asci_tx;
    bus.serial_pin = serial_pin;
    bus.boundary = boundary;
    u->cpu = z180_create(&bus, u->clock_hz);
    if (!u->cpu) { snprintf(err, errlen, "cannot create the Z180"); bl_destroy(u); return NULL; }
    return u;
}

void bl_destroy(bl_unit *u)
{
    if (!u)
        return;
    if (u->cpu)
        z180_destroy(u->cpu);
    free(u->flash);
    free(u->ram);
    free(u->fflash);
    free(u->lfifo);
    free(u->ev);
    free(u);
}

/* the legacy path's own slicing (bns.c's): the golden vectors depend on it (CONTRACT.md 3).  Built with
   BL_Z180_MAME (the MAME core, ../cpu/z180_mame.cpp), the corrected path runs whole steps instead: the same
   slices, but a slice boundary changes nothing there (CONTRACT.md 2). */
#ifdef BL_Z180_MAME
#define BL_RUN_SLICE z180_run
#else
#define BL_RUN_SLICE z180_run_legacy
#endif
static void run_cycles(bl_unit *u, unsigned long long n)
{
    while (n > 0) {
        unsigned long long done = BL_RUN_SLICE(u->cpu, n > SLICE_DEFAULT ? SLICE_DEFAULT : n);
        n = done >= n ? 0 : n - done;
    }
}

void bl_boot(bl_unit *u, unsigned long long target_instr)
{
    while (z180_steps(u->cpu) < target_instr)
        run_cycles(u, SLICE_DEFAULT);
}

void bl_live(bl_unit *u)
{
    u->live_on = 1;
    u->ssi_ar = 0;
    u->ssi_ready_at = ~0ULL;
    ar_line(u);
}

void bl_run(bl_unit *u, unsigned long long cycles)
{
    run_cycles(u, cycles);
}

void bl_set_ar(bl_unit *u, int requesting)
{
    u->ssi_ar = requesting ? 1 : 0;
    ar_line(u);
}

void bl_queue(bl_unit *u, const unsigned char *bytes, int n)
{
    int i;
    for (i = 0; i < n; i++)
        u->lfifo[u->lhead++ & (FIFO - 1)] = bytes[i];
}

void bl_urgent(bl_unit *u, int byte)
{
    u->urgent = byte & 0xFF;
}

int bl_drop(bl_unit *u)
{
    int n6 = 0;
    while (u->ltail != u->lhead) {
        if (u->lfifo[u->ltail & (FIFO - 1)] == 0x06)
            n6++;
        u->ltail++;
    }
    return n6;
}

int bl_save_state(const bl_unit *u, const char *path)
{
    FILE *s = fopen(path, "wb");
    int ok;
    if (!s)
        return 0;
    ok = fwrite(u->ram + 0x40000, 1, 0x40000, s) == 0x40000 && fwrite(u->fflash, 1, 0x80000, s) == 0x80000;
    return fclose(s) == 0 && ok;
}

int bl_key(bl_unit *u, int chord)
{
    if (u->n_live_keys == (int)sizeof u->live_keys)
        return 0;
    u->live_keys[u->n_live_keys++] = (unsigned char)chord;
    return 1;
}

unsigned long long bl_cycles(const bl_unit *u)
{
    return z180_cycles(u->cpu);
}

int bl_events(const bl_unit *u, const bl_event **events)
{
    *events = u->ev;
    return u->n_ev;
}

void bl_clear_events(bl_unit *u)
{
    u->n_ev = 0;
}
