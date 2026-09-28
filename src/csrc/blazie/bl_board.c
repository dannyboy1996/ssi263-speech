/* bl_board.c -- the Blazie Braille Lite 2000 board around a Z180 core, per instance (see bl_board.h).
 *
 * z180emu/bns.c's --live path, line for line, with the tracing and logging left out.  The Z180 core's memory and
 * port callbacks carry no context, so the instance being run is a thread-local pointer set at every entry point.
 *
 * Memory: ROM image from file offset 3000h at physical 00000h (256 KB, FFh-padded); 40000h..FFFFFh RAM, except that
 * port E0h bit 3 maps the AMD 29F040-style file flash over 80000h..FFFFFh.  SSI-263 at ports C0h..C4h, its A/R
 * request on /INT1.  Braille keyboard on port 40h, /INT2.  Serial on ASCI0 (9600 bit/s at 6.144 MHz), with the
 * host honouring the unit's XON/XOFF.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z180/z180.h"
#include "z180/z180asci.h"
#include "bl_board.h"

#define ROM_SIZE 0x40000
#define FILE_ROM_OFFSET 0x3000
#define SLICE_DEFAULT 10000
#define FLASH_BASE 0x80000
#define MAX_KEYS 64
#define FIFO 0x10000

#if defined(_MSC_VER)
#define BL_TLS __declspec(thread)
#else
#define BL_TLS __thread
#endif

struct bl_unit {
    UINT8 *flash, *ram, *fflash;
    struct z180_device *cpu;
    unsigned long long instrcnt, cyc_base, asci_next;
    int cur_slice;
    double clock_hz, phon_ms;
    int irq_line;
    UINT8 ssi_ready_value, ssi[5];
    int ssi_ctl, ssi_ar, ssi_mode, live_on;
    unsigned long long ssi_ready_at;
    UINT8 *lfifo;
    unsigned lhead, ltail;
    int urgent, host_xoff;
    unsigned long long key_at[MAX_KEYS];
    UINT8 key_val[MAX_KEYS], key_latch;
    int n_keys, next_key, key_latched, hold_chord;
    int ff_state, ff_autoselect;
    UINT8 port_e0;
    unsigned site_release;
    offs_t instr_pc;
    UINT8 *asci_pend;
    bl_event *ev;
    int n_ev, cap_ev;
};

static BL_TLS bl_unit *cur;

static void event(unsigned char type, unsigned char a, unsigned char b)
{
    if (cur->n_ev == cur->cap_ev) {
        int cap = cur->cap_ev ? cur->cap_ev * 2 : 256;
        bl_event *e = (bl_event *)realloc(cur->ev, (size_t)cap * sizeof(bl_event));
        if (!e)
            return;
        cur->ev = e;
        cur->cap_ev = cap;
    }
    cur->ev[cur->n_ev].type = type;
    cur->ev[cur->n_ev].a = a;
    cur->ev[cur->n_ev].b = b;
    cur->n_ev++;
}

static unsigned long long cycles_now(void)
{
    return cur->cyc_base + (unsigned long long)(cur->cur_slice - cpu_icount_z180((device_t *)cur->cpu));
}

static void ar_line(void)                    /* A/R request drives the INT line (level, active = asserted) */
{
    if (cur->irq_line >= 0)
        z180_set_irq_line((device_t *)cur->cpu, cur->irq_line, cur->ssi_ar && cur->ssi_mode ? 1 : 0);
}

static int flash_window(void)
{
    return (cur->port_e0 & 0x08) != 0;
}

static UINT8 fflash_read(offs_t off)
{
    if (cur->ff_autoselect)
        return (off & 3) == 0 ? 0x01 : (off & 3) == 1 ? 0xA4 : 0x00;   /* AMD, Am29F040 */
    return cur->fflash[off];
}

static void fflash_write(offs_t off, UINT8 V)
{
    unsigned a = off & 0x7FFF;
    switch (cur->ff_state) {
    case 0: if (V == 0xF0) { cur->ff_autoselect = 0; return; }
            cur->ff_state = (a == 0x5555 && V == 0xAA) ? 1 : 0; break;
    case 1: cur->ff_state = (a == 0x2AAA && V == 0x55) ? 2 : 0; break;
    case 2: cur->ff_state = 0;
            if (a != 0x5555) break;
            if (V == 0xA0) cur->ff_state = 3;
            else if (V == 0x80) cur->ff_state = 4;
            else if (V == 0x90) cur->ff_autoselect = 1;
            else if (V == 0xF0) cur->ff_autoselect = 0;
            break;
    case 3: cur->fflash[off] &= V; cur->ff_state = 0; break;
    case 4: cur->ff_state = (a == 0x5555 && V == 0xAA) ? 5 : 0; break;
    case 5: cur->ff_state = (a == 0x2AAA && V == 0x55) ? 6 : 0; break;
    case 6: cur->ff_state = 0;
            if (V == 0x10)
                memset(cur->fflash, 0xFF, 0x80000);
            else if (V == 0x30)
                memset(cur->fflash + (off & 0x70000), 0xFF, 0x10000);
            break;
    }
}

static UINT8 mem_read(offs_t A)
{
    A &= 0xFFFFF;
    if (A < ROM_SIZE) return cur->flash[A];
    if (A >= FLASH_BASE && flash_window()) return fflash_read(A - FLASH_BASE);
    return cur->ram[A];
}

static void mem_write(offs_t A, UINT8 V)
{
    A &= 0xFFFFF;
    if (A < ROM_SIZE)
        return;                              /* ROM: bns.c counts and logs these; after a hard reset there are none */
    if (A >= FLASH_BASE && flash_window()) {
        fflash_write(A - FLASH_BASE, V);
        return;
    }
    cur->ram[A] = V;
}

static UINT8 io_read(offs_t Port)
{
    int p = Port & 0xFF;
    UINT8 v;
    if (p >= 0xC0 && p <= 0xC4)
        return cur->ssi_ar ? cur->ssi_ready_value : (UINT8)(cur->ssi_ready_value ^ 0x80);
    v = p == 0x40 ? 0x00 : 0xFF;
    if (p == 0x40 && cur->hold_chord >= 0) {
        if (cur->instr_pc == cur->site_release || cur->instr_pc == cur->site_release + 8)
            cur->hold_chord = -1;            /* the firmware now waits for release */
        else
            v = (UINT8)cur->hold_chord;
    }
    if (p == 0x40 && cur->key_latched) {
        v = cur->key_latch;
        cur->key_latched = 0;
        z180_set_irq_line((device_t *)cur->cpu, 2, 0);
    }
    return v;
}

static void io_write(offs_t Port, UINT8 V)
{
    int p = Port & 0xFF;
    if (p == 0xE0)
        cur->port_e0 = V;
    if (p >= 0xC0 && p <= 0xC4) {
        int reg = p - 0xC0;
        unsigned long long cyc = cycles_now();
        cur->ssi[reg] = V;
        if (reg == 3)
            cur->ssi_ctl = V >> 7;
        event('W', (unsigned char)reg, V);
        if (reg == 0 && cur->ssi_ctl) {
            cur->ssi_mode = V >> 6;
            ar_line();
        } else if (reg == 0) {
            cur->ssi_ar = 0;
            cur->ssi_ready_at = cur->live_on ? ~0ULL
                              : cyc + (unsigned long long)(cur->phon_ms * cur->clock_hz / 1000.0);
            ar_line();
        }
    }
}

static int irqack(device_t *device, int irqnum)
{
    (void)device; (void)irqnum;
    return 0xFF;
}

static int asci_rx(device_t *device, int channel)
{
    int b;
    (void)device;
    if (cur->live_on) {
        if (channel == 0 && cur->urgent >= 0) {
            b = cur->urgent;
            cur->urgent = -1;
            return b;
        }
        if (channel != 0 || cur->lhead == cur->ltail || cur->host_xoff)
            return -1;
        return cur->lfifo[cur->ltail++ & (FIFO - 1)];
    }
    return -1;                               /* before live mode nothing is queued (bns: no --serial in live runs) */
}

static void asci_tx(device_t *device, int channel, UINT8 data)
{
    (void)device;
    if (channel != 0)
        return;
    event('T', data, 0);
    if (data == 0x13)
        cur->host_xoff = 1;
    else if (data == 0x11)
        cur->host_xoff = 0;
}

void debugger_instruction_hook(device_t *device, offs_t curpc)
{
    bl_unit *u = cur;
    u->instrcnt++;
    {                                        /* ASCI baud clock: one tick per 16 CPU cycles (DR = 16) */
        unsigned long long now = cycles_now();
        if (u->asci_next <= now) {
            struct z180asci_channel *c0 = u->cpu->z180asci->m_chan0, *c1 = u->cpu->z180asci->m_chan1;
            unsigned long long ticks = (now - u->asci_next) / 16 + 1;
            if (ticks < c0->m_brg_timer && ticks < c1->m_brg_timer) {
                c0->m_brg_timer -= (uint16_t)ticks;
                c1->m_brg_timer -= (uint16_t)ticks;
                u->asci_next += ticks * 16;
            } else {
                while (u->asci_next <= now) {
                    z180asci_channel_device_timer(c0);
                    z180asci_channel_device_timer(c1);
                    u->asci_next += 16;
                }
            }
        }
        {                                    /* the ASCI interrupt request is a level (Astra, Reply 15) */
            int k;
            if (!u->asci_pend)
                u->asci_pend = z180_asci_irq_pending((device_t *)u->cpu);
            for (k = 0; k < 2; k++) {
                struct z180asci_channel *c = k ? u->cpu->z180asci->m_chan1 : u->cpu->z180asci->m_chan0;
                int rx = (c->m_stat & 0x08) && (c->m_stat & (0x80 | 0x40 | 0x20 | 0x10));
                int tx = (c->m_stat & 0x01) && (c->m_stat & 0x02);
                u->asci_pend[k] = (UINT8)(rx || tx);
            }
        }
    }
    u->instr_pc = curpc & 0xFFFF;            /* read by the key-release check (port reads) */
    if (!u->live_on && !u->ssi_ar && cycles_now() >= u->ssi_ready_at) {
        u->ssi_ar = 1;
        ar_line();
    }
    if (u->next_key < u->n_keys && u->instrcnt >= u->key_at[u->next_key]) {
        if (u->hold_chord >= 0)
            u->hold_chord = -1;              /* a hand pressing a key has let go of the power-on chord */
        u->key_latch = u->key_val[u->next_key++];
        u->key_latched = 1;
        z180_set_irq_line(device, 2, 1);
    }
}

/* a byte signature ("F6 40 D3 C0", "??" = any byte) in the ROM image: the offset of the unique match plus `off`,
   or 0 if absent or ambiguous (bns.c's detect_sites) */
static unsigned find_sig(const char *sig, int off, long img_len)
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
            if (pat[k] >= 0 && cur->flash[i + k] != pat[k]) break;
        if (k == n) { hits++; at = (unsigned)i + off; }
    }
    return hits == 1 ? at : 0;
}

static struct address_space memspace = {mem_read, mem_write, mem_read};
static struct address_space iospace = {io_read, io_write, NULL};

bl_unit *bl_create(const char *firmware, const char *state, double phon_ms,
                   const unsigned long long *key_at, const unsigned char *key_val, int n_keys,
                   char *err, int errlen)
{
    bl_unit *u = (bl_unit *)calloc(1, sizeof(bl_unit));
    FILE *f;
    long n;
    int i;
    if (!u) { snprintf(err, errlen, "out of memory"); return NULL; }
    u->flash = (UINT8 *)malloc(ROM_SIZE);
    u->ram = (UINT8 *)calloc(1, 0x100000);
    u->fflash = (UINT8 *)malloc(0x80000);
    u->lfifo = (UINT8 *)malloc(FIFO);
    if (!u->flash || !u->ram || !u->fflash || !u->lfifo) { snprintf(err, errlen, "out of memory"); bl_destroy(u); return NULL; }
    u->cur_slice = SLICE_DEFAULT;
    u->clock_hz = 6144000.0;
    u->phon_ms = phon_ms;
    u->irq_line = 1;
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
    cur = u;
    {                                        /* bns.c refuses a firmware without these sites; so does this */
        unsigned send = find_sig("F6 40 D3 C0", 2, n);
        unsigned prime = find_sig("F6 E0 D3 C4 CD ?? ?? 3E C0 D3 C0", 9, n);
        unsigned fetch = find_sig("2A 17 D6 7E 23 22 17 D6", 3, n);
        unsigned halt = find_sig("21 ?? ?? 7E B7 20 06 76 CD", 7, n);
        u->site_release = find_sig("DB 40 D3 20 E6 7F 20 F8", 0, n);
        if (!(send && prime && fetch && halt && u->site_release)) {
            snprintf(err, errlen, "could not locate the firmware sites in %s", firmware);
            bl_destroy(u);
            return NULL;
        }
    }
    u->cpu = cpu_create_z180("Z180", Z180_TYPE_Z180, (int)u->clock_hz, &memspace, NULL, &iospace, irqack, NULL,
                             asci_rx, asci_tx, NULL, NULL, NULL, NULL);
    if (!u->cpu) { snprintf(err, errlen, "cannot create the Z180"); bl_destroy(u); return NULL; }
    cpu_reset_z180((device_t *)u->cpu);
    u->cpu->z180asci->m_chan0->m_dcd = 1;    /* /DCD0 = carrier present (see bns.c) */
    return u;
}

void bl_destroy(bl_unit *u)
{
    if (!u)
        return;
    if (cur == u)
        cur = NULL;
    /* the Z180 core has no destructor; its allocation is left to the process (one per unit, a few KB) */
    free(u->flash);
    free(u->ram);
    free(u->fflash);
    free(u->lfifo);
    free(u->ev);
    free(u);
}

static void run_cycles(unsigned long long n)
{
    while (n > 0) {
        cur->cur_slice = n > SLICE_DEFAULT ? SLICE_DEFAULT : (int)n;
        cpu_execute_z180((device_t *)cur->cpu, cur->cur_slice);
        {
            unsigned long long done = (unsigned long long)(cur->cur_slice - cpu_icount_z180((device_t *)cur->cpu));
            cur->cyc_base += done;
            n = done >= n ? 0 : n - done;
        }
    }
    cur->cur_slice = SLICE_DEFAULT;
}

void bl_boot(bl_unit *u, unsigned long long target_instr)
{
    cur = u;
    while (u->instrcnt < target_instr)
        run_cycles(SLICE_DEFAULT);
}

void bl_live(bl_unit *u)
{
    cur = u;
    u->live_on = 1;
    u->ssi_ar = 0;
    u->ssi_ready_at = ~0ULL;
    ar_line();
}

void bl_run(bl_unit *u, unsigned long long cycles)
{
    cur = u;
    run_cycles(cycles);
}

void bl_set_ar(bl_unit *u, int requesting)
{
    cur = u;
    u->ssi_ar = requesting ? 1 : 0;
    ar_line();
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

unsigned long long bl_cycles(const bl_unit *u)
{
    return u->cyc_base + (unsigned long long)(u->cur_slice - cpu_icount_z180((device_t *)u->cpu));
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
