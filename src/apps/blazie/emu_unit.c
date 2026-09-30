/* emu_unit.c -- see emu_unit.h.  The chip is made as the NVDA driver makes it (bl_voice.c's blv_create), the board
 * through bl_host, but booted with no chords, so the unit comes up in its own main menu.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../csrc/ssi263.h"
#include "../../csrc/blazie/bl_host.h"
#include "emu_unit.h"

#define BOARD_LOWPASS_HZ 5000.0     /* as bl_voice.c */
#define CLOSURE_NOISE_LEAD_MS 10.0
#define MAKEUP 2.0
#define BOOT_INSTR 100000ULL      /* the stand-in boot before the chip takes over A/R: short, so the greeting is heard */
#define STEP_S 0.0005               /* the host's lockstep */

struct emu_unit {
    ssi263 *chip;
    bl_host *host;
    double out_rate, gain;
    const double *pend;             /* rendered but not yet handed out */
    int n_pend;
};

emu_unit *emu_create(const char *firmware, const char *state, double out_rate, int whine, char *err, int errlen)
{
    emu_unit *u = (emu_unit *)calloc(1, sizeof(emu_unit));
    ssi263_params p;
    if (!u) { snprintf(err, errlen, "out of memory"); return NULL; }
    ssi263_default_params(&p);
    p.closure_noise_lead_s = CLOSURE_NOISE_LEAD_MS / 1000.0;
    if (whine)
        p.carrier_rel_db = -300.0;
    u->chip = ssi263_new(&p, ssi263_default_rom(), out_rate);
    if (!u->chip) { snprintf(err, errlen, "could not create the chip"); free(u); return NULL; }
    u->host = bh_create(firmware, state, u->chip, out_rate, BOARD_LOWPASS_HZ, NULL, NULL, 0, BOOT_INSTR, 0, err,
                        errlen);
    if (!u->host) { ssi263_free(u->chip); free(u); return NULL; }
    bh_set_whine(u->host, whine);
    u->out_rate = out_rate;
    u->gain = MAKEUP;
    return u;
}

void emu_destroy(emu_unit *u)
{
    if (!u) return;
    bh_destroy(u->host);
    ssi263_free(u->chip);
    free(u);
}

void emu_render(emu_unit *u, short *out, int n)
{
    while (n > 0) {
        int k;
        if (!u->n_pend) {
            u->n_pend = bh_run(u->host, (double)n / u->out_rate, STEP_S, &u->pend);
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

int emu_key(emu_unit *u, int chord)
{
    return bh_key(u->host, chord);
}

int emu_save(const emu_unit *u, const char *path)
{
    return bh_save_state(u->host, path);
}

void emu_set_whine(emu_unit *u, int whine)
{
    bh_set_whine(u->host, whine);
}

void emu_set_volume(emu_unit *u, int volume)
{
    u->gain = MAKEUP * volume / 100.0;
}
