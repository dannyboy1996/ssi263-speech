/* emu_unit.c -- see emu_unit.h.  The chip is made as the NVDA driver makes it (bl_voice.c's blv_create).  The
 * Braille Lite runs through bl_host (its lockstep, board low-pass and idle-channel noise), booted with no chords, so
 * the unit comes up in its own main menu.  The Type 'n Speak runs through the same lockstep written out here:
 * bl_host carries the Braille Lite driver's bookkeeping, which the Type 'n Speak has no use for.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../csrc/ssi263.h"
#include "../../csrc/blazie/bl_host.h"
#include "../../csrc/blazie/tns_board.h"
#include "emu_unit.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BOARD_LOWPASS_HZ 5000.0     /* as bl_voice.c */
#define CLOSURE_NOISE_LEAD_MS 10.0
#define MAKEUP 2.0
#define BOOT_INSTR 100000ULL        /* the stand-in boot before the chip takes over A/R: short, so the greeting is heard */
#define STEP_S 0.0005               /* the host's lockstep */
#define CLOCK_HZ 6144000.0
#define BATTERY_LEVEL 128           /* the gauge's raw reading: "100 percent", not charging (255: charging) */

void ssi_onepole(double *x, int n, double b0, double b1, double a1, double *state);   /* ssi263dsp.c */

struct emu_unit {
    int kind;
    ssi263 *chip;
    bl_host *host;                  /* the Braille Lite */
    tns_unit *tns;                  /* the Type 'n Speak */
    int ar;
    double b0, b1, a1, lp[2];
    double *buf;
    int cap;
    double out_rate, gain;
    const double *pend;             /* rendered but not yet handed out */
    int n_pend;
};

emu_unit *emu_create(int kind, const char *firmware, const char *state, double out_rate, int whine, char *err,
                     int errlen)
{
    emu_unit *u = (emu_unit *)calloc(1, sizeof(emu_unit));
    ssi263_params p;
    if (!u) { snprintf(err, errlen, "out of memory"); return NULL; }
    u->kind = kind;
    ssi263_default_params(&p);
    p.closure_noise_lead_s = CLOSURE_NOISE_LEAD_MS / 1000.0;
    if (whine && kind == EMU_BRAILLE_LITE)
        p.carrier_rel_db = -300.0;
    u->chip = ssi263_new(&p, ssi263_default_rom(), out_rate);
    if (!u->chip) { snprintf(err, errlen, "could not create the chip"); free(u); return NULL; }
    if (kind == EMU_TYPE_N_SPEAK) {
        double k = tan(M_PI * BOARD_LOWPASS_HZ / out_rate);   /* dsp.onepole_coeffs, as bl_host */
        u->tns = tns_create(firmware, state, err, errlen);
        if (!u->tns) { ssi263_free(u->chip); free(u); return NULL; }
        u->b0 = u->b1 = k / (1.0 + k);
        u->a1 = (k - 1.0) / (k + 1.0);
        u->ar = -1;
    } else {
        u->host = bh_create(firmware, state, u->chip, out_rate, BOARD_LOWPASS_HZ, NULL, NULL, 0, BOOT_INSTR, 0, err,
                            errlen);
        if (!u->host) { ssi263_free(u->chip); free(u); return NULL; }
        bh_set_whine(u->host, whine);
        bh_battery(u->host, BATTERY_LEVEL);   /* the status menu's % reads the gauge (without it: frozen) */
    }
    u->out_rate = out_rate;
    u->gain = MAKEUP;
    return u;
}

void emu_destroy(emu_unit *u)
{
    if (!u) return;
    if (u->host) bh_destroy(u->host);
    if (u->tns) tns_destroy(u->tns);
    ssi263_free(u->chip);
    free(u->buf);
    free(u);
}

int emu_kind(const emu_unit *u)
{
    return u->kind;
}

/* ---- the Type 'n Speak's lockstep (bl_host.c's bh_run, without the driver's turbo and whine) ------------------ */
static void tns_events_to_chip(emu_unit *u)
{
    const bl_event *ev;
    int n = tns_events(u->tns, &ev), i;
    for (i = 0; i < n; i++)
        if (ev[i].type == 'W')
            ssi263_write(u->chip, ev[i].a, ev[i].b);
    tns_clear_events(u->tns);
}

static int tns_render(emu_unit *u, double seconds, const double **out)
{
    double t = 0.0;
    int n_buf = 0;
    while (t < seconds) {
        double before, dt;
        unsigned long long cyc;
        long n, got;
        int r = ssi263_request(u->chip) ? 1 : 0;
        if (r != u->ar) {
            u->ar = r;
            tns_set_ar(u->tns, r);
            tns_events_to_chip(u);
        }
        before = ssi263_time(u->chip);
        n = r ? (long)nearbyint(STEP_S / 4 * u->out_rate) : (long)(STEP_S * u->out_rate);
        if (n_buf + n + 1 > u->cap) {
            int cap = (n_buf + (int)n + 1) * 2;
            double *b = (double *)realloc(u->buf, sizeof(double) * (size_t)cap);
            if (!b) break;
            u->buf = b;
            u->cap = cap;
        }
        got = r ? ssi263_run(u->chip, n, u->buf + n_buf) : ssi263_run_until_request(u->chip, n, u->buf + n_buf);
        n_buf += (int)got;
        dt = ssi263_time(u->chip) - before;
        if (dt < 1e-5)
            dt = 1e-5;
        cyc = (unsigned long long)(CLOCK_HZ * dt);
        tns_run(u->tns, cyc ? cyc : 1);
        tns_events_to_chip(u);
        t += dt;
    }
    if (n_buf)
        ssi_onepole(u->buf, n_buf, u->b0, u->b1, u->a1, u->lp);
    *out = u->buf;
    return n_buf;
}

void emu_render(emu_unit *u, short *out, int n)
{
    while (n > 0) {
        int k;
        if (!u->n_pend) {
            double sec = (double)n / u->out_rate;
            u->n_pend = u->tns ? tns_render(u, sec, &u->pend) : bh_run(u->host, sec, STEP_S, &u->pend);
            if (u->n_pend <= 0) {   /* nothing came back: give silence rather than spin */
                memset(out, 0, sizeof(short) * (size_t)n);
                u->n_pend = 0;
                return;
            }
        }
        k = n < u->n_pend ? n : u->n_pend;
        ssi_pcm16(u->pend, k, u->gain, out);
        u->pend += k;
        u->n_pend -= k;
        out += k;
        n -= k;
    }
}

int emu_key(emu_unit *u, int key)
{
    return u->tns ? tns_key(u->tns, key) : bh_key(u->host, key);
}

int emu_save(const emu_unit *u, const char *path)
{
    return u->tns ? tns_save_state(u->tns, path) : bh_save_state(u->host, path);
}

void emu_set_whine(emu_unit *u, int whine)
{
    if (u->host)
        bh_set_whine(u->host, whine);
}

void emu_set_volume(emu_unit *u, int volume)
{
    u->gain = MAKEUP * volume / 100.0;
}
