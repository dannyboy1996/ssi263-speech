/* as_voice.c -- see as_voice.h.  Each part names the driver function it ports (nvda/accent/synthDrivers/accentmini.py,
 * the "sa" voice); change them together.  test_android_native.py compares the two byte for byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "as_voice.h"
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
/* DEFAULTS = ("5", 5, 5, 0): rate, pitch, voice, intonation after power-up */
#define DEF_RATE AT_DEF_RATE
#define DEF_PITCH AT_DEF_PITCH
#define DEF_VOICE AT_DEF_VOICE
#define DEF_INFL AT_DEF_INFL

struct as_voice {
    as_host *h;
    ssi263 *chip;
    int rate, pitch, inflection, volume, numbers;      /* NVDA's scales */
    char sent_rate;                                    /* _sent */
    int sent_pitch, sent_voice, sent_infl;
    int base_pitch, cur_pitch;                         /* settings[1], _cur_pitch */
    int pitch_dirty, snap_until_speech;
    int lead, active;
    double t_start, gain;
    short *pcm;
    int pcm_cap;
};

/* ---- _boot ---------------------------------------------------------------------------------------------------- */
AS_API as_voice *asv_create(const unsigned char *u2, size_t n2, const unsigned char *u3, size_t n3,
                            const unsigned char *u4, size_t n4, double out_rate, char *err, int errlen)
{
    as_voice *v = (as_voice *)calloc(1, sizeof(as_voice));
    if (!v) { snprintf(err, (size_t)errlen, "out of memory"); return NULL; }
    v->h = ash_create(u2, n2, u3, n3, u4, n4, NULL, out_rate, err, errlen);
    if (!v->h) { free(v); return NULL; }
    v->chip = ash_chip(v->h);
    ash_boot(v->h, BOOT_LIMIT);
    v->sent_rate = DEF_RATE; v->sent_pitch = DEF_PITCH; v->sent_voice = DEF_VOICE; v->sent_infl = DEF_INFL;
    v->rate = 50; v->pitch = 50; v->inflection = 100; v->volume = 100; v->numbers = 1;
    v->base_pitch = v->cur_pitch = DEF_PITCH;
    return v;
}

AS_API void asv_destroy(as_voice *v)
{
    if (!v) return;
    ash_destroy(v->h);
    free(v->pcm);
    free(v);
}

AS_API as_host *asv_host(as_voice *v) { return v->h; }

AS_API void asv_set(as_voice *v, int rate, int pitch, int inflection, int volume, int numbers)
{
    v->rate = rate; v->pitch = pitch; v->inflection = inflection; v->volume = volume; v->numbers = numbers != 0;
}

/* ---- _accent_pitch, _accent_settings, and the text: ../accent_text.c (shared with the Accent-mini's am_voice) */
AS_API int asv_pitch_step(int pitch) { return at_pitch_step(pitch); }

AS_API int asv_say_bytes(const char *utf8, int with_numbers, char *out, int cap)
{
    char *s = at_say_text(utf8, with_numbers);
    int n;
    if (!s) return -1;
    n = (int)strlen(s);
    if (out && n < cap) memcpy(out, s, (size_t)n + 1);
    free(s);
    return n;
}

/* ---- _speakJob / _speakItems ----------------------------------------------------------------------------------- */
static void say_str(as_voice *v, const char *s, int speech)
{
    ash_say(v->h, (const unsigned char *)s, (int)strlen(s), speech);
}

static void say_pitch(as_voice *v, int p)                     /* box.chip.snap_pitch = True; box.say("\x1bP%d") */
{
    char cmd[16];
    ssi263_set_snap_pitch(v->chip, 1);
    snprintf(cmd, sizeof cmd, "\x1bP%d", p);
    say_str(v, cmd, 0);
}

/* _speakJob's finally: the user's pitch again, only after the capital's audio exists */
static void restore_pitch(as_voice *v)
{
    if (v->cur_pitch != v->base_pitch) {
        say_pitch(v, v->base_pitch);
        v->cur_pitch = v->base_pitch;
        v->pitch_dirty = 1;
    }
}

AS_API int asv_speak(as_voice *v, const char *utf8, int pitch_offset)
{
    char r = at_rate_char(v->rate), cmd[64], *text, *line;
    int p = at_pitch_step(v->pitch), infl = at_inflection(v->inflection), k = 0;
    size_t n;
    v->active = 0;
    /* only what changed: repeated option commands emit preparation records of their own */
    if (r != v->sent_rate) k += snprintf(cmd + k, sizeof cmd - (size_t)k, "\x1bR%c", r);
    if (p != v->sent_pitch) k += snprintf(cmd + k, sizeof cmd - (size_t)k, "\x1bP%d", p);
    if (DEF_VOICE != v->sent_voice) k += snprintf(cmd + k, sizeof cmd - (size_t)k, "\x1bV%d", DEF_VOICE);
    if (infl != v->sent_infl) k += snprintf(cmd + k, sizeof cmd - (size_t)k, "\x1bM%d", infl);
    if (k) {
        say_str(v, cmd, 0);
        v->sent_rate = r; v->sent_pitch = p; v->sent_voice = DEF_VOICE; v->sent_infl = infl;
    }
    v->base_pitch = v->cur_pitch = p;
    v->gain = v->volume / 100.0;
    v->lead = 1;
    /* the items: PitchCommand(offset), then the text */
    if (pitch_offset) {
        int want = at_pitch_step(v->pitch + pitch_offset);
        if (want != v->cur_pitch) {
            say_pitch(v, want);
            v->cur_pitch = want;
            v->pitch_dirty = 1;
        }
    }
    text = at_say_text(utf8, v->numbers);
    if (!text || !*text) {
        free(text);
        restore_pitch(v);
        return 0;
    }
    n = strlen(text);
    line = (char *)malloc(n + 2);
    if (!line) { free(text); restore_pitch(v); return 0; }
    memcpy(line, text, n);
    line[n] = '\r';                                           /* ESC =F: the carriage return starts speech */
    free(text);
    v->t_start = ash_get_double(v->h, "time");
    ash_say(v->h, (const unsigned char *)line, (int)n + 1, -1);
    free(line);
    v->active = 1;
    return 1;
}

static void finish(as_voice *v)
{
    v->active = 0;
    restore_pitch(v);
}

AS_API int asv_render(as_voice *v, const short **pcm, int *done)
{
    const double *y;
    int n, start = 0, count;
    *pcm = v->pcm;
    *done = 0;
    if (!v->active) { *done = 1; return 0; }
    n = ash_run(v->h, BLOCK_S, STEP_S, &y);
    if (n < 0) { finish(v); *done = 1; return 0; }
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
        if (!q) { finish(v); *done = 1; return 0; }
        v->pcm = q;
        v->pcm_cap = count;
    }
    *pcm = v->pcm;
    if (count > 0)
        ssi_pcm16(y + start, count, v->gain, v->pcm);
    if (v->snap_until_speech && ash_get_double(v->h, "last_speech") >= v->t_start) {
        ssi263_set_snap_pitch(v->chip, 0);
        v->snap_until_speech = 0;
    }
    if (!ash_busy(v->h, QUIET, PATIENCE)) {
        v->pitch_dirty = 0;                                   /* the driver has taken everything sent so far */
        finish(v);
        *done = 1;
    } else {
        double last = ash_get_double(v->h, "last_speech");
        double quiet = ash_get_double(v->h, "time") - (last > v->t_start ? last : v->t_start);
        if (ash_speaking(v->h) && quiet > STALL_S) {          /* the driver restarts the card; the utterance ends */
            finish(v);
            *done = 1;
        }
    }
    return count > 0 ? count : 0;
}

/* cancel(), then _run's flush: box.cancel(), and _resend_pitch when a pitch command may have been dropped */
AS_API void asv_cancel(as_voice *v)
{
    if (v->active)
        finish(v);
    ash_cancel(v->h, CANCEL_LIMIT);
    if (v->pitch_dirty) {
        say_pitch(v, v->sent_pitch);
        v->snap_until_speech = 1;
        v->cur_pitch = v->sent_pitch;
    }
}
