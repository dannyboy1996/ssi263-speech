/* ssa_engine.c -- see ssa_engine.h.  The Braille Lite's flow is sd_ssi263.c's speak(): blv_set, blv_speak,
 * blv_render until done, a stop between blocks and blv_cancel after it.  The Accent SA's is the NVDA driver's _speakJob
 * (as_voice.h) on a unit of the utterance's own. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blazie/bl_voice.h"
#include "accentsa/as_voice.h"
#include "ssa_engine.h"
#include "ssa_map.h"

#define BLAZIE_VOICES 2
#define ACCENT_PITCH 50                /* the settings' pitch the Accent SA's units keep: the rest goes as a capital's */

int ssa_accent_break = 0;

static const char *voice_files[BLAZIE_VOICES][2] = {
    {"BL2ENG.BNS", "bl2_2003_warm.state"},
    {"BL2SPA.BNS", "bl2spa_fresh.state"},
};
static const size_t ROM_SIZES[3] = {0x10000, 0x8000, 0x8000};

struct ssa_engine {
    char *datadir;
    int sample_rate, inflection, whine;
    bl_voice *voices[BLAZIE_VOICES];
    unsigned char *rom[3];             /* the Accent SA's u2, u3, u4 */
    as_voice *spare;                   /* the Accent SA's next unit: booted, not yet spoken on */
    as_voice *accent;                  /* the unit speaking the current utterance */
    int cur;                           /* the utterance's voice; -1 when none is running */
    const short *block;                /* the last block rendered, valid until the next render */
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
    e->cur = -1;
    e->done = 1;
    return e;
}

static void shut_down(ssa_engine *e)
{
    int i;
    for (i = 0; i < BLAZIE_VOICES; i++) {
        blv_destroy(e->voices[i]);
        e->voices[i] = NULL;
    }
    asv_destroy(e->spare);
    asv_destroy(e->accent);
    e->spare = e->accent = NULL;
    e->cur = -1;                       /* nothing may point into a unit that is gone */
    e->block = NULL;
    e->block_n = e->block_at = 0;
    e->done = 1;
}

void ssa_free(ssa_engine *e)
{
    int i;
    if (!e) return;
    shut_down(e);
    for (i = 0; i < 3; i++) free(e->rom[i]);
    free(e->datadir);
    free(e);
}

int ssa_set_accent_roms(ssa_engine *e, const unsigned char *u2, size_t n2, const unsigned char *u3, size_t n3,
                        const unsigned char *u4, size_t n4)
{
    const unsigned char *src[3] = {u2, u3, u4};
    size_t n[3] = {n2, n3, n4};
    int i;
    for (i = 0; i < 3; i++)
        if (!src[i] || n[i] != ROM_SIZES[i]) return 0;
    asv_destroy(e->spare);             /* a unit made from other ROMs is not this voice any more */
    e->spare = NULL;
    for (i = 0; i < 3; i++) {
        unsigned char *p = (unsigned char *)malloc(n[i]);
        if (!p) return 0;
        memcpy(p, src[i], n[i]);
        free(e->rom[i]);
        e->rom[i] = p;
    }
    return 1;
}

int ssa_has_voice(const ssa_engine *e, int voice)
{
    char p[1200];
    if (voice == SSA_ACCENT_SA) return e->rom[0] && e->rom[1] && e->rom[2];
    if (voice < 0 || voice >= BLAZIE_VOICES) return 0;
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
    if (voice == SSA_ACCENT_SA) {
        if (e->spare) return 0;
        if (!ssa_has_voice(e, voice)) { snprintf(err, errlen, "the Accent SA's ROMs are not set"); return -1; }
        e->spare = asv_create(e->rom[0], ROM_SIZES[0], e->rom[1], ROM_SIZES[1], e->rom[2], ROM_SIZES[2],
                              (double)e->sample_rate, err, errlen);
        return e->spare ? 0 : -1;
    }
    if (voice < 0 || voice >= BLAZIE_VOICES) { snprintf(err, errlen, "no voice %d", voice); return -1; }
    if (e->voices[voice]) return 0;
    path(e, voice_files[voice][0], fw, sizeof fw);
    path(e, voice_files[voice][1], st, sizeof st);
    e->voices[voice] = blv_create(fw, st, voice == SSA_SPANISH ? BLV_CP850 : BLV_LATIN1, e->sample_rate,
                                  e->inflection, e->whine, err, errlen);
    return e->voices[voice] ? 0 : -1;
}

/* The Accent SA's unit is done with: let go, and the next one booted now, so the next utterance need not wait for
   it (under the test's control 3, kept instead: its state carries into the next utterance). */
static void accent_release(ssa_engine *e)
{
    char err[256];
    if (!e->accent) return;
    if (ssa_accent_break == 3) {
        asv_destroy(e->spare);
        e->spare = e->accent;
    } else {
        asv_destroy(e->accent);
    }
    e->accent = NULL;
    if (!e->spare)
        ssa_load(e, SSA_ACCENT_SA, err, sizeof err);
}

void ssa_cancel(ssa_engine *e)
{
    if (e->cur == SSA_ACCENT_SA) {
        if (ssa_accent_break == 3 && e->accent && !e->done)
            asv_cancel(e->accent);     /* a kept unit must drop what it has not spoken */
        accent_release(e);
    } else if (e->cur >= 0 && !e->done) {
        blv_cancel(e->voices[e->cur]);
    }
    e->cur = -1;
    e->done = 1;
    e->block_n = e->block_at = 0;
}

static int start_accent(ssa_engine *e, const char *utf8, const ssa_settings *s, int request_rate, int request_pitch)
{
    char err[256];
    int pitch = ssa_pitch(s->pitch, ssa_accent_break == 1 ? 100 : request_pitch);
    if (ssa_load(e, SSA_ACCENT_SA, err, sizeof err) != 0)
        return -1;
    e->accent = e->spare;
    e->spare = NULL;
    if (ssa_accent_break == 2)         /* the control: the pitch as a setting, glided to */
        asv_set(e->accent, ssa_rate(s->rate, request_rate), pitch, e->inflection ? 100 : 0,
                s->volume * SSA_ACCENT_LEVEL / 100, 1);
    else
        asv_set(e->accent, ssa_rate(s->rate, request_rate), ACCENT_PITCH, e->inflection ? 100 : 0,
                s->volume * SSA_ACCENT_LEVEL / 100, 1);
    e->blocks = 0;
    if (!asv_speak(e->accent, utf8, ssa_accent_break == 2 ? 0 : pitch - ACCENT_PITCH)) {
        e->cur = SSA_ACCENT_SA;        /* released at once: nothing to say */
        e->done = 1;
        accent_release(e);
        e->cur = -1;
        return 1;
    }
    e->cur = SSA_ACCENT_SA;
    e->done = 0;
    return 0;
}

int ssa_start(ssa_engine *e, int voice, const char *utf8, const ssa_settings *s, int request_rate,
              int request_pitch)
{
    char err[256];
    ssa_cancel(e);                     /* an utterance still running is abandoned: its leftovers must not follow */
    __atomic_store_n(&e->stop, 0, __ATOMIC_SEQ_CST);
    if (voice == SSA_ACCENT_SA)
        return start_accent(e, utf8, s, request_rate, request_pitch);
    if (ssa_load(e, voice, err, sizeof err) != 0)
        return -1;
    blv_set(e->voices[voice], ssa_rate(s->rate, request_rate), ssa_pitch(s->pitch, request_pitch), s->tone,
            s->volume, s->pack);
    e->blocks = 0;
    if (!blv_speak(e->voices[voice], utf8))
        return 1;
    e->cur = voice;
    e->done = 0;
    return 0;
}

static int render(ssa_engine *e)
{
    if (e->cur == SSA_ACCENT_SA)
        return asv_render(e->accent, &e->block, &e->done);
    return blv_render(e->voices[e->cur], &e->block, &e->done);
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
        if (e->cur < 0)
            return 0;
        if (e->done) {                 /* finished, and all of it handed over */
            if (e->cur == SSA_ACCENT_SA)
                accent_release(e);
            e->cur = -1;
            return 0;
        }
        if (__atomic_load_n(&e->stop, __ATOMIC_SEQ_CST))
            return -2;
        e->block_n = render(e);
        e->block_at = 0;
        if (e->block_n > 0)
            e->blocks++;
    }
    return n;
}

void ssa_stop(ssa_engine *e)
{
    __atomic_store_n(&e->stop, 1, __ATOMIC_SEQ_CST);
}

int ssa_blocks(const ssa_engine *e) { return e->blocks; }

long ssa_probe(const char *datadir, int voice, const char *utf8, unsigned long long *fnv, char *err, int errlen)
{
    ssa_settings s = {50, 50, 7, 100, 1};
    ssa_engine *e = ssa_new(datadir);
    short buf[4096];
    unsigned long long h = 1469598103934665603ULL;
    long samples = 0;
    int n, i;
    if (!e) { snprintf(err, errlen, "out of memory"); return -1; }
    if (ssa_load(e, voice, err, errlen) != 0) { ssa_free(e); return -1; }
    if (ssa_start(e, voice, utf8, &s, 100, 100) == 0)
        while ((n = ssa_pull(e, buf, 4096)) > 0) {
            for (i = 0; i < n; i++) {
                unsigned v = (unsigned short)buf[i];
                h = (h ^ (v & 0xFF)) * 1099511628211ULL;
                h = (h ^ (v >> 8)) * 1099511628211ULL;
            }
            samples += n;
        }
    ssa_free(e);
    if (fnv) *fnv = h;
    return samples;
}
