/* ssa_engine.c -- see ssa_engine.h.  The flow is sd_ssi263.c's speak(): blv_set, blv_speak, blv_render until done,
 * a stop between blocks and blv_cancel after it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blazie/bl_voice.h"
#include "ssa_engine.h"
#include "ssa_map.h"

static const char *voice_files[SSA_VOICES][2] = {
    {"BL2ENG.BNS", "bl2_2003_warm.state"},
    {"BL2SPA.BNS", "bl2spa_fresh.state"},
};

struct ssa_engine {
    char *datadir;
    int sample_rate, inflection, whine;
    bl_voice *voices[SSA_VOICES];
    bl_voice *cur;                     /* the utterance's voice; NULL when none is running */
    const short *block;                /* the last block blv_render gave, valid until the next render */
    int block_n, block_at, done, blocks;
    int stop;                          /* ssa_stop's flag: set from any thread, read with __atomic */
};

static void path(const ssa_engine *e, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", e->datadir, name);
}

static int readable(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

ssa_engine *ssa_new(const char *datadir)
{
    ssa_engine *e = (ssa_engine *)calloc(1, sizeof(ssa_engine));
    if (!e) return NULL;
    e->datadir = (char *)malloc(strlen(datadir) + 1);
    if (!e->datadir) { free(e); return NULL; }
    strcpy(e->datadir, datadir);
    e->sample_rate = 22050;
    e->inflection = 1;
    e->whine = 0;
    return e;
}

static void shut_down(ssa_engine *e)
{
    int i;
    for (i = 0; i < SSA_VOICES; i++) {
        blv_destroy(e->voices[i]);
        e->voices[i] = NULL;
    }
    e->cur = NULL;
}

void ssa_free(ssa_engine *e)
{
    if (!e) return;
    shut_down(e);
    free(e->datadir);
    free(e);
}

int ssa_has_voice(const ssa_engine *e, int voice)
{
    char p[1200];
    if (voice < 0 || voice >= SSA_VOICES) return 0;
    path(e, voice_files[voice][0], p, sizeof p);
    if (!readable(p)) return 0;
    path(e, voice_files[voice][1], p, sizeof p);
    return readable(p);
}

void ssa_configure(ssa_engine *e, int sample_rate, int inflection, int whine)
{
    if (sample_rate != 11025 && sample_rate != 22050 && sample_rate != 44100)
        sample_rate = 22050;
    inflection = inflection != 0;
    whine = whine < 0 || whine > 2 ? 0 : whine;
    if (sample_rate == e->sample_rate && inflection == e->inflection && whine == e->whine)
        return;
    shut_down(e);
    e->sample_rate = sample_rate;
    e->inflection = inflection;
    e->whine = whine;
}

int ssa_sample_rate(const ssa_engine *e) { return e->sample_rate; }

int ssa_load(ssa_engine *e, int voice, char *err, int errlen)
{
    char fw[1200], st[1200];
    if (voice < 0 || voice >= SSA_VOICES) { snprintf(err, errlen, "no voice %d", voice); return -1; }
    if (e->voices[voice]) return 0;
    path(e, voice_files[voice][0], fw, sizeof fw);
    path(e, voice_files[voice][1], st, sizeof st);
    e->voices[voice] = blv_create(fw, st, voice == SSA_SPANISH ? BLV_CP850 : BLV_LATIN1, e->sample_rate,
                                  e->inflection, e->whine, err, errlen);
    return e->voices[voice] ? 0 : -1;
}

void ssa_cancel(ssa_engine *e)
{
    if (e->cur && !e->done)
        blv_cancel(e->cur);
    e->cur = NULL;
    e->done = 1;
    e->block_n = e->block_at = 0;
}

int ssa_start(ssa_engine *e, int voice, const char *utf8, const ssa_settings *s, int request_rate,
              int request_pitch)
{
    char err[256];
    ssa_cancel(e);                     /* an utterance still running is abandoned: its leftovers must not follow */
    __atomic_store_n(&e->stop, 0, __ATOMIC_SEQ_CST);
    if (ssa_load(e, voice, err, sizeof err) != 0)
        return -1;
    blv_set(e->voices[voice], ssa_rate(s->rate, request_rate), ssa_pitch(s->pitch, request_pitch), s->tone,
            s->volume, s->pack);
    e->blocks = 0;
    if (!blv_speak(e->voices[voice], utf8))
        return 1;
    e->cur = e->voices[voice];
    e->done = 0;
    return 0;
}

int ssa_pull(ssa_engine *e, short *out, int cap)
{
    int n = 0;
    while (n < cap) {
        if (e->block_at < e->block_n) {
            int k = e->block_n - e->block_at;
            if (k > cap - n) k = cap - n;
            memcpy(out + n, e->block + e->block_at, sizeof(short) * (size_t)k);
            e->block_at += k;
            n += k;
            continue;
        }
        if (n)                         /* hand over what there is; the next block comes on the next pull */
            break;
        if (!e->cur || e->done)
            return 0;
        if (__atomic_load_n(&e->stop, __ATOMIC_SEQ_CST))
            return -2;
        e->block_n = blv_render(e->cur, &e->block, &e->done);
        e->block_at = 0;
        if (e->block_n > 0)
            e->blocks++;
        if (e->done && e->block_n <= 0)
            e->cur = NULL;
    }
    return n;
}

void ssa_stop(ssa_engine *e)
{
    __atomic_store_n(&e->stop, 1, __ATOMIC_SEQ_CST);
}

int ssa_blocks(const ssa_engine *e) { return e->blocks; }
