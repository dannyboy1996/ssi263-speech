/* am_voice.c -- see am_voice.h.  Each part names the driver function it ports (nvda/accent/synthDrivers/accentmini.py,
 * the "mini" voice); its text rules are ../accent_text.c, shared with the Accent SA's as_voice.c.  Change them
 * together.  nvda/tools/am_voice_equiv.py compares the two byte for byte.  MIT. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "am_voice.h"
#include "../accent_text.h"

/* the driver's constants */
#define BLOCK_S 0.03
#define STEP_S 0.0005
#define LEAD_THRESHOLD 0.003
#define LEAD_PREROLL 220
#define BOOT_LIMIT 4.0
#define CANCEL_LIMIT 0.6
#define QUIET 0.03
#define PATIENCE 1.5
#define STALL_S 4.0                     /* the driver's watchdog: busy, speaking, and silent this long */

struct am_voice {
    am_host *h;
    ssi263 *chip;
    unsigned char *dvc;                                /* kept to restart the card after a fault, as _run does */
    size_t n_dvc;
    double out_rate;
    int rate, pitch, inflection, volume, numbers, voice;      /* NVDA's scales; voice = the variant, 0-9 */
    char sent_rate;                                    /* _sent */
    int sent_pitch, sent_voice, sent_infl;
    int base_pitch, cur_pitch;                         /* settings[1], _cur_pitch */
    int pitch_dirty, snap_until_speech;
    int lead, active, fault;
    double t_start, gain;
    short *pcm;
    int pcm_cap;
};

/* ---- _boot ---------------------------------------------------------------------------------------------------- */
static am_host *boot(const unsigned char *dvc, size_t n, double out_rate, char *err, int errlen)
{
    am_host *h = amh_create(dvc, n, NULL, out_rate, err, errlen);
    if (!h)
        return NULL;
    if (amh_boot(h, BOOT_LIMIT) < 0) {
        if (err && errlen > 0)
            snprintf(err, (size_t)errlen, "boot: %s", amh_error(h));
        amh_destroy(h);
        return NULL;
    }
    return h;
}

static void defaults_sent(am_voice *v)                 /* self._sent = DEFAULTS */
{
    v->sent_rate = AT_DEF_RATE;
    v->sent_pitch = AT_DEF_PITCH;
    v->sent_voice = AT_DEF_VOICE;
    v->sent_infl = AT_DEF_INFL;
}

AM_API am_voice *amv_create_mem(const unsigned char *dvc, size_t n, double out_rate, char *err, int errlen)
{
    am_voice *v = (am_voice *)calloc(1, sizeof(am_voice));
    if (!v || !(v->dvc = (unsigned char *)malloc(n ? n : 1))) {
        free(v);
        if (err && errlen > 0)
            snprintf(err, (size_t)errlen, "out of memory");
        return NULL;
    }
    memcpy(v->dvc, dvc, n);
    v->n_dvc = n;
    v->out_rate = out_rate;
    v->h = boot(dvc, n, out_rate, err, errlen);
    if (!v->h) {
        free(v->dvc);
        free(v);
        return NULL;
    }
    v->chip = amh_chip(v->h);
    defaults_sent(v);
    v->rate = 50; v->pitch = 50; v->inflection = 100; v->volume = 100; v->numbers = 1; v->voice = AT_DEF_VOICE;
    v->base_pitch = v->cur_pitch = AT_DEF_PITCH;
    return v;
}

AM_API am_voice *amv_create(const char *dvc_path, double out_rate, char *err, int errlen)
{
    FILE *f = fopen(dvc_path, "rb");
    long len;
    unsigned char *d;
    am_voice *v;
    if (!f) {
        if (err && errlen > 0)
            snprintf(err, (size_t)errlen, "cannot open the driver (SPKEMS.DVC)");
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) || (len = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        if (err && errlen > 0)
            snprintf(err, (size_t)errlen, "cannot read the driver (SPKEMS.DVC)");
        return NULL;
    }
    d = (unsigned char *)malloc((size_t)len);
    if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) {
        free(d);
        fclose(f);
        if (err && errlen > 0)
            snprintf(err, (size_t)errlen, "cannot read the driver (SPKEMS.DVC)");
        return NULL;
    }
    fclose(f);
    v = amv_create_mem(d, (size_t)len, out_rate, err, errlen);
    free(d);
    return v;
}

AM_API void amv_destroy(am_voice *v)
{
    if (!v)
        return;
    amh_destroy(v->h);
    free(v->dvc);
    free(v->pcm);
    free(v);
}

AM_API am_host *amv_host(am_voice *v) { return v->h; }
AM_API int amv_fault(const am_voice *v) { return v->fault; }
AM_API int amv_pitch_step(int pitch) { return at_pitch_step(pitch); }

AM_API void amv_set(am_voice *v, int rate, int pitch, int inflection, int volume, int numbers, int voice)
{
    v->rate = rate; v->pitch = pitch; v->inflection = inflection; v->volume = volume; v->numbers = numbers != 0;
    if (voice >= 0 && voice <= 9)                      /* _set_variant: "0" to "9" only */
        v->voice = voice;
}

AM_API int amv_say_bytes(const char *utf8, int with_numbers, char *out, int cap)
{
    char *s = at_say_text(utf8, with_numbers);
    int n;
    if (!s) return -1;
    n = (int)strlen(s);
    if (out && n < cap) memcpy(out, s, (size_t)n + 1);
    free(s);
    return n;
}

/* ---- faults: _run's "Accent speech failed; restarting the emulated card" ---------------------------------------- */
static void failed(am_voice *v)
{
    v->fault = 1;
    v->active = 0;
    /* _speakJob's finally would have restored the pitch on the old card: what is left of it is the flag */
    if (v->cur_pitch != v->base_pitch) {
        v->cur_pitch = v->base_pitch;
        v->pitch_dirty = 1;
    }
}

static int restart(am_voice *v)                        /* self._box = self._boot(); self._sent = DEFAULTS */
{
    am_host *h = boot(v->dvc, v->n_dvc, v->out_rate, NULL, 0);
    if (!h)
        return 0;
    amh_destroy(v->h);
    v->h = h;
    v->chip = amh_chip(h);
    defaults_sent(v);
    v->fault = 0;
    return 1;
}

/* ---- _speakJob / _speakItems ----------------------------------------------------------------------------------- */
static int say_str(am_voice *v, const char *s)          /* box.say(s, speech=False) */
{
    return amh_say(v->h, (const unsigned char *)s, (int)strlen(s), 0, 0);
}

static int say_pitch(am_voice *v, int p)                /* box.chip.snap_pitch = True; box.say("\x1bP%d") */
{
    char cmd[16];
    ssi263_set_snap_pitch(v->chip, 1);
    snprintf(cmd, sizeof cmd, "\x1bP%d", p);
    return say_str(v, cmd);
}

/* _speakJob's finally: the user's pitch again, only after the capital's audio exists */
static int restore_pitch(am_voice *v)
{
    if (v->cur_pitch != v->base_pitch) {
        int r = say_pitch(v, v->base_pitch);
        v->cur_pitch = v->base_pitch;
        v->pitch_dirty = 1;
        return r;
    }
    return 0;
}

AM_API int amv_speak(am_voice *v, const char *utf8, int pitch_offset)
{
    char r = at_rate_char(v->rate), cmd[64], *text, *line;
    int p = at_pitch_step(v->pitch), infl = at_inflection(v->inflection), k = 0;
    size_t n;
    v->active = 0;
    if (v->fault && !restart(v))
        return -1;
    /* only what changed: repeated option commands emit preparation records of their own */
    if (r != v->sent_rate) k += snprintf(cmd + k, sizeof cmd - (size_t)k, "\x1bR%c", r);
    if (p != v->sent_pitch) k += snprintf(cmd + k, sizeof cmd - (size_t)k, "\x1bP%d", p);
    if (v->voice != v->sent_voice) k += snprintf(cmd + k, sizeof cmd - (size_t)k, "\x1bV%d", v->voice);
    if (infl != v->sent_infl) k += snprintf(cmd + k, sizeof cmd - (size_t)k, "\x1bM%d", infl);
    if (k) {
        if (say_str(v, cmd) < 0) { failed(v); return -1; }
        v->sent_rate = r; v->sent_pitch = p; v->sent_voice = v->voice; v->sent_infl = infl;
    }
    v->base_pitch = v->cur_pitch = p;
    v->gain = v->volume / 100.0;
    v->lead = 1;
    /* the items: PitchCommand(offset), then the text */
    if (pitch_offset) {
        int want = at_pitch_step(v->pitch + pitch_offset);
        if (want != v->cur_pitch) {
            if (say_pitch(v, want) < 0) { failed(v); return -1; }
            v->cur_pitch = want;
            v->pitch_dirty = 1;
        }
    }
    text = at_say_text(utf8, v->numbers);
    if (!text || !*text) {
        free(text);
        if (restore_pitch(v) < 0) { failed(v); return -1; }
        return 0;
    }
    n = strlen(text);
    line = (char *)malloc(n + 2);
    if (!line) { free(text); restore_pitch(v); return 0; }
    memcpy(line, text, n);
    line[n] = '\r';                                           /* ESC =F: the carriage return starts speech */
    free(text);
    v->t_start = amh_get_double(v->h, "time");
    /* in the background: audio starts while the driver is still taking a long text */
    k = amh_say(v->h, (const unsigned char *)line, (int)n + 1, -1, 1);
    free(line);
    if (k < 0) { failed(v); return -1; }
    v->active = 1;
    return 1;
}

static int finish(am_voice *v)
{
    v->active = 0;
    return restore_pitch(v);
}

AM_API int amv_render(am_voice *v, const short **pcm, int *done)
{
    const double *y;
    int n, start = 0, count;
    *pcm = v->pcm;
    *done = 0;
    if (!v->active) { *done = 1; return 0; }
    n = amh_run(v->h, BLOCK_S, STEP_S, &y);
    if (n < 0) { failed(v); *done = 1; return 0; }
    if (v->lead) {                                            /* _trim_lead */
        int k;
        start = n;
        for (k = 0; k < n; k++)
            if (y[k] > LEAD_THRESHOLD || y[k] < -LEAD_THRESHOLD) {
                start = k > LEAD_PREROLL ? k - LEAD_PREROLL : 0;
                v->lead = 0;
                break;
            }
    }
    count = n - start;
    if (count > v->pcm_cap) {
        short *q = (short *)realloc(v->pcm, (size_t)count * sizeof(short));
        if (!q) { failed(v); *done = 1; return 0; }
        v->pcm = q;
        v->pcm_cap = count;
    }
    *pcm = v->pcm;
    if (count > 0)
        ssi_pcm16(y + start, count, v->gain, v->pcm);
    if (v->snap_until_speech && amh_get_double(v->h, "last_speech") >= v->t_start) {
        ssi263_set_snap_pitch(v->chip, 0);
        v->snap_until_speech = 0;
    }
    if (!amh_busy(v->h, QUIET, PATIENCE)) {
        v->pitch_dirty = 0;                                   /* the driver has taken everything sent so far */
        if (finish(v) < 0)
            failed(v);
        *done = 1;
    } else {
        double last = amh_get_double(v->h, "last_speech");
        double quiet = amh_get_double(v->h, "time") - (last > v->t_start ? last : v->t_start);
        if (amh_speaking(v->h) && quiet > STALL_S) {          /* "card stalled": the driver restarts it */
            failed(v);
            *done = 1;
        }
    }
    return count > 0 ? count : 0;
}

/* cancel() during an utterance, then _run's flush: box.cancel(), and _resend_pitch when a pitch command may have been
   dropped.  After the utterance has finished it does nothing, as the driver's cancel between jobs. */
AM_API void amv_cancel(am_voice *v)
{
    if (!v->active)
        return;
    if (finish(v) < 0 || amh_cancel(v->h, CANCEL_LIMIT) < 0) {
        failed(v);
        return;
    }
    if (v->pitch_dirty) {                                     /* _resend_pitch */
        ssi263_set_snap_pitch(v->chip, 1);
        v->snap_until_speech = 1;
        if (say_pitch(v, v->sent_pitch) < 0)
            failed(v);
        v->cur_pitch = v->sent_pitch;
    }
}
