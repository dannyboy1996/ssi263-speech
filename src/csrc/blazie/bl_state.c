/* bl_state.c -- see bl_state.h.  The recipes are bns.c command lines turned into data; each run is a power-on of a
 * fresh unit (new CPU, the RAM and file flash the last run left), as each bns.exe invocation was. */
#include <stdio.h>
#include <string.h>
#include "bl_board.h"
#include "bl_state.h"

BL_API int blv_state_break = 0;

#define M 1000000ULL
#define MAX_RUN_KEYS 12
#define CHUNK (10 * M)                     /* instructions between progress reports: bl_boot stops on the same slice
                                              boundaries however it is called, so chunking changes nothing */

typedef struct {
    unsigned long long at;                 /* bns --key INSTR=CHORD: instructions since this power-on */
    unsigned char chord;                   /* port 40h: dot 1 = bit 0 .. dot 6 = bit 5, space = bit 6 */
} key;

typedef struct {
    int hold;                              /* bns --hold: the chord held at power-on, -1 none */
    double phon_ms;                        /* bns --phon-ms: the stand-in A/R time before live mode */
    unsigned long long run;                /* bns --max: instructions */
    int n_keys;
    key keys[MAX_RUN_KEYS];
} run;

/* English, the June 2003 image: three power-ons, as make_states.sh and then one more bns run made the shipped
   bl2_2003_warm.state.  ONCE's September 2000 English takes the same keys at the same points: its phoneme stream
   through these runs is June 2003's but for one sentence spoken at power-on (a wording change), and the third run's
   is the same, write-protected answer included.
   1. Hard reset: the i-chord (4Ah = dots 2 4 + space) held at power-on; then y (3Dh) five times -- "delete all data
      in file area?", "are you sure?", the flash and its "are you sure?", the folder system.
   2. o-chord (55h), f (0Bh), c (09h): "file to create?"; a (01h); e-chord (51h): RAM file "a", open.
   3. Warm reset: all seven keys (7Fh) held at power-on -- "ready", "help is open" -- and n (1Dh), which the help
      file answers with "file is write protected". */
static const run ENGLISH[] = {
    {0x4A, 20.0, 60 * M, 5, {{12 * M, 0x3D}, {20 * M, 0x3D}, {28 * M, 0x3D}, {36 * M, 0x3D}, {44 * M, 0x3D}}},
    {-1, 20.0, 60 * M, 5, {{8 * M, 0x55}, {16 * M, 0x0B}, {24 * M, 0x09}, {32 * M, 0x01}, {40 * M, 0x51}}},
    {0x7F, 20.0, 30 * M, 1, {{12 * M, 0x1D}}},
};

/* Spanish, ONCE's BL2SPA.BNS: one power-on, as the Spanish unit's reset made bl2spa_fresh.state: the i-chord held,
   then s (0Eh, "si") twelve times, 80 million instructions apart, from 100 million on. */
static const run SPANISH[] = {
    {0x4A, 5.0, 1150 * M, 12, {{100 * M, 0x0E}, {180 * M, 0x0E}, {260 * M, 0x0E}, {340 * M, 0x0E}, {420 * M, 0x0E},
                               {500 * M, 0x0E}, {580 * M, 0x0E}, {660 * M, 0x0E}, {740 * M, 0x0E}, {820 * M, 0x0E},
                               {900 * M, 0x0E}, {980 * M, 0x0E}}},
};

BL_API int blv_make_state(const char *firmware, int language, const char *out_path, blv_state_progress progress,
                          void *ctx, char *err, int errlen)
{
    const run *runs = language == BLV_STATE_SPANISH ? SPANISH : ENGLISH;
    int n_runs = language == BLV_STATE_SPANISH ? (int)(sizeof SPANISH / sizeof *SPANISH)
                                               : (int)(sizeof ENGLISH / sizeof *ENGLISH);
    unsigned long long total = 0, before = 0;
    int r, i;
    if (language != BLV_STATE_ENGLISH && language != BLV_STATE_SPANISH) {
        snprintf(err, errlen, "no state recipe for language %d", language);
        return 0;
    }
    for (r = 0; r < n_runs; r++)
        total += runs[r].run;
    for (r = 0; r < n_runs; r++) {
        const run *s = &runs[r];
        unsigned long long at[MAX_RUN_KEYS], t;
        unsigned char val[MAX_RUN_KEYS];
        bl_unit *u;
        for (i = 0; i < s->n_keys; i++) {
            at[i] = s->keys[i].at;
            val[i] = s->keys[i].chord;
        }
        /* the first run powers on with no state (RAM clear, file flash erased); the others resume the last one's */
        u = bl_create(firmware, r ? out_path : NULL, s->phon_ms, at, val, s->n_keys, err, errlen);
        if (!u) {
            remove(out_path);
            return 0;
        }
        if (s->hold >= 0)
            bl_hold(u, blv_state_break && s->hold == 0x7F ? 0x7E : s->hold);
        for (t = CHUNK < s->run ? CHUNK : s->run;; t = t + CHUNK < s->run ? t + CHUNK : s->run) {
            bl_boot(u, t);
            if (progress && progress(ctx, (double)(before + t) / (double)total)) {
                bl_destroy(u);
                remove(out_path);
                snprintf(err, errlen, "cancelled");
                return 0;
            }
            if (t == s->run)
                break;
        }
        before += s->run;
        if (!bl_save_state(u, out_path)) {
            bl_destroy(u);
            remove(out_path);
            snprintf(err, errlen, "cannot write %s", out_path);
            return 0;
        }
        bl_destroy(u);
    }
    return 1;
}
