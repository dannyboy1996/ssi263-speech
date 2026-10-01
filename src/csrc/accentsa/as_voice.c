/* as_voice.c -- see as_voice.h.  Each part names the driver function it ports (nvda/accent/synthDrivers/accentmini.py,
 * the "sa" voice); change them together.  test_android_native.py compares the two byte for byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "as_voice.h"
#include "../numwords.h"

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
static const char RATES[] = "0123456789ABCDEFGH";
/* DEFAULTS = ("5", 5, 5, 0): rate, pitch, voice, intonation after power-up */
#define DEF_RATE '5'
#define DEF_PITCH 5
#define DEF_VOICE 5
#define DEF_INFL 0

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

/* ---- _accent_pitch, _accent_settings (int() of a positive float = floor) --------------------------------------- */
static int clamp100(int x) { return x < 0 ? 0 : x > 100 ? 100 : x; }

static int accent_pitch(int p)
{
    p = clamp100(p);
    return p <= 50 ? (int)(p * 5 / 50.0 + 0.5) : 5 + (int)((p - 50) * 4 / 50.0 + 0.5);
}

AS_API int asv_pitch_step(int pitch) { return accent_pitch(pitch); }

static int accent_rate(int r)
{
    r = clamp100(r);
    return r <= 50 ? (int)(r * 5 / 50.0 + 0.5) : 5 + (int)((r - 50) * 12 / 50.0 + 0.5);
}

/* INFLECTION = ((0, 1), (25, 2), (50, 3), (75, 4), (100, 0)): min() by distance, the first of a tie */
static int accent_inflection(int x)
{
    static const int at[5] = {0, 25, 50, 75, 100}, cmd[5] = {1, 2, 3, 4, 0};
    int i, best = 0;
    for (i = 1; i < 5; i++)
        if (abs(at[i] - x) < abs(at[best] - x))
            best = i;
    return cmd[best];
}

/* ---- the text: currencies, _clean, strip, _numbers --------------------------------------------------------------- */

/* _clean: 7-bit, no control characters (ESC and Ctrl-X are the Accent's commands), no tilde ("~/" opens its phoneme
   input and swallows everything to the next "~") */
static int clean(const unsigned *in, int n, char *out)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        unsigned c = in[i];
        const char *r = NULL;
        if (c < 32 || c == 127 || c == '~') out[m++] = ' ';
        else if (c < 128) out[m++] = (char)c;
        else {
            switch (c) {
            case 0x2018: case 0x2019: r = "'"; break;
            case 0x201C: case 0x201D: r = "\""; break;
            case 0x2013: case 0x2014: r = "-"; break;
            case 0x2026: r = "..."; break;
            case 0xE9: case 0xE8: r = "e"; break;
            case 0xE1: case 0xE0: r = "a"; break;
            case 0xF6: r = "o"; break;
            case 0xFC: r = "u"; break;
            case 0xF1: r = "n"; break;
            case 0xE7: r = "c"; break;
            default: r = " "; break;
            }
            while (*r) out[m++] = *r++;
        }
    }
    out[m] = 0;
    return m;
}

/* _grouped: a dollar amount with its whole part re-grouped by commas (format(int, ",")) when it has 1-15 digits;
   appended to out, which has room for 2 * len + 8 */
static int grouped(const char *money, int len, char *out)
{
    char digits[16];
    int dot = 1, k, nd = 0, m = 0, i, lead = 0;
    while (dot < len && money[dot] != '.') dot++;
    for (k = 1; k < dot; k++)
        if (money[k] != ',') {
            if (nd < 16) digits[nd] = money[k];
            nd++;
        }
    if (!nd || nd > 15) {                                     /* as it came */
        memcpy(out, money, (size_t)len);
        return len;
    }
    out[m++] = '$';
    while (lead < nd - 1 && digits[lead] == '0') lead++;      /* int() drops leading zeros */
    for (i = lead; i < nd; i++) {
        out[m++] = digits[i];
        if ((nd - 1 - i) % 3 == 0 && i != nd - 1) out[m++] = ',';
    }
    memcpy(out + m, money + dot, (size_t)(len - dot));
    return m + len - dot;
}

/* MONEY = (\$\d[\d,]*(?:\.\d+)?|\$\.\d+): the end of the amount at i, or 0 */
static int money_at(const char *s, int n, int i)
{
    int j;
    if (s[i] != '$' || i + 1 >= n) return 0;
    if (s[i + 1] >= '0' && s[i + 1] <= '9') {
        for (j = i + 2; j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ','); j++) ;
        if (j + 1 < n && s[j] == '.' && s[j + 1] >= '0' && s[j + 1] <= '9')
            for (j += 1; j < n && s[j] >= '0' && s[j] <= '9'; j++) ;
        return j;
    }
    if (s[i + 1] == '.' && i + 2 < n && s[i + 2] >= '0' && s[i + 2] <= '9') {
        for (j = i + 2; j < n && s[j] >= '0' && s[j] <= '9'; j++) ;
        return j;
    }
    return 0;
}

/* _numbers: MONEY.split, the amounts _grouped and the text between them normalise()d, each part on its own */
static char *numbers(const char *s)
{
    int n = (int)strlen(s), i = 0, start = 0, cap = 256, len = 0;
    char *out = (char *)malloc((size_t)cap);
    if (!out) return NULL;
    out[0] = 0;
    for (;;) {
        int end = i < n ? money_at(s, n, i) : 0, need, w;
        char *part, *words;
        if (i < n && !end) { i++; continue; }
        /* the text before the amount (start..i), normalised on its own; then the amount, grouped */
        part = (char *)malloc((size_t)(i - start) + 1);
        if (!part) { free(out); return NULL; }
        memcpy(part, s + start, (size_t)(i - start));
        part[i - start] = 0;
        words = nw_normalise(part);
        free(part);
        if (!words) { free(out); return NULL; }
        w = (int)strlen(words);
        need = len + w + (end ? 2 * (end - i) : 0) + 9;
        if (need > cap) {
            char *q;
            while (cap < need) cap *= 2;
            q = (char *)realloc(out, (size_t)cap);
            if (!q) { free(words); free(out); return NULL; }
            out = q;
        }
        memcpy(out + len, words, (size_t)w);
        len += w;
        free(words);
        if (end)
            len += grouped(s + i, end - i, out + len);
        out[len] = 0;
        if (!end) break;
        i = start = end;
    }
    return out;
}

/* the text the driver sends (without its "\r"), malloc'd; NULL when out of memory */
static char *say_text(const char *utf8, int with_numbers)
{
    size_t n = strlen(utf8);
    unsigned *t = (unsigned *)malloc((n + 1) * sizeof(unsigned)), *u;
    char *c, *p, *q;
    int k, m;
    if (!t) return NULL;
    k = nw_utf8(utf8, t);
    u = (unsigned *)malloc(((size_t)k * 16 + 64) * sizeof(unsigned));
    if (!u) { free(t); return NULL; }
    m = nw_currencies(t, k, u);
    free(t);
    c = (char *)malloc((size_t)m * 3 + 1);                    /* "…" -> "..." */
    if (!c) { free(u); return NULL; }
    clean(u, m, c);
    free(u);
    for (p = c; *p == ' '; p++) ;                             /* strip(): only spaces are left to strip */
    for (q = p + strlen(p); q > p && q[-1] == ' '; q--) ;
    *q = 0;
    memmove(c, p, strlen(p) + 1);
    if (!with_numbers) return c;
    p = numbers(c);
    free(c);
    return p;
}

AS_API int asv_say_bytes(const char *utf8, int with_numbers, char *out, int cap)
{
    char *s = say_text(utf8, with_numbers);
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
    char r = RATES[accent_rate(v->rate)], cmd[64], *text, *line;
    int p = accent_pitch(v->pitch), infl = accent_inflection(v->inflection), k = 0;
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
        int want = accent_pitch(v->pitch + pitch_offset);
        if (want != v->cur_pitch) {
            say_pitch(v, want);
            v->cur_pitch = want;
            v->pitch_dirty = 1;
        }
    }
    text = say_text(utf8, v->numbers);
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
