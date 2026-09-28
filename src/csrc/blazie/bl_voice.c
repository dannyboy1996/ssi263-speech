/* bl_voice.c -- see bl_voice.h.  Each part names the driver function it ports (nvda/blazie/synthDrivers/blazie.py);
 * change them together.  voice_equiv.py compares the two byte for byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bl_voice.h"
#include "bl_cp850.h"

/* the driver's constants */
#define BLOCK_S 0.03
#define UNIT_VOLUME 6
#define MAKEUP 2.0
#define BOARD_LOWPASS_HZ 5000.0
#define CLOSURE_NOISE_LEAD_MS 10.0
#define DEFAULT_RATE 11
#define DEFAULT_PITCH 16
#define DEFAULT_TONE 7
#define MAX_RATE 15
#define MAX_WORDS 14
#define LEAD_THRESHOLD 0.003
#define LEAD_PREROLL 220
/* hosts/blazie.py boot_keys(menu=("punct_none", "numbers_toggle"), start=3000000, gap=1500000, status) */
#define KEY_START 3000000ULL
#define KEY_GAP 1500000ULL

struct bl_voice {
    ssi263 *chip;
    bl_host *host;
    int encoding;
    int rate, pitch, tone, volume, pack;          /* NVDA's scales */
    int sent_rate, sent_pitch, sent_tone;         /* unit.sent_settings */
    double gain;
    int lead, active;
    short *pcm;
    int pcm_cap;
};

/* ---- _boot ---------------------------------------------------------------------------------------------------- */
BL_API bl_voice *blv_create(const char *firmware, const char *state, int encoding, double out_rate, int inflection,
                            int whine, char *err, int errlen)
{
    bl_voice *v = (bl_voice *)calloc(1, sizeof(bl_voice));
    ssi263_params p;
    unsigned char codes[16];
    unsigned long long key_at[16];
    int n = 0, i;
    const double *y;
    if (!v) { snprintf(err, errlen, "out of memory"); return NULL; }
    /* params = {"closure_noise_lead_ms": CLOSURE_NOISE_LEAD_MS}, and carrier_rel_db -300 under the whine model */
    ssi263_default_params(&p);
    p.closure_noise_lead_s = CLOSURE_NOISE_LEAD_MS / 1000.0;
    if (whine)
        p.carrier_rel_db = -300.0;
    v->chip = ssi263_new(&p, ssi263_default_rom(), out_rate);
    if (!v->chip) { snprintf(err, errlen, "could not create the chip"); free(v); return NULL; }
    /* boot_keys: the status menu first when inflection goes off (34-chord, i, n, e-chord), then the speech menu
       (345-chord, z = punctuation none, n = full numbers, 123456-chord, L, e) */
    if (!inflection) {
        codes[n++] = 0x4C; codes[n++] = 0x0A; codes[n++] = 0x1D; codes[n++] = 0x51;
    }
    codes[n++] = 0x5C; codes[n++] = 0x35; codes[n++] = 0x1D; codes[n++] = 0x7F; codes[n++] = 0x07; codes[n++] = 0x51;
    for (i = 0; i < n; i++)
        key_at[i] = KEY_START + (unsigned long long)i * KEY_GAP;
    v->host = bh_create(firmware, state, v->chip, out_rate, BOARD_LOWPASS_HZ, key_at, codes, n,
                        KEY_START + (unsigned long long)n * KEY_GAP, 0, err, errlen);
    if (!v->host) { ssi263_free(v->chip); free(v); return NULL; }
    bh_send(v->host, (const unsigned char *)"\x18", 1);
    bh_send(v->host, (const unsigned char *)"\r\x06", 2);
    bh_run(v->host, 0.3, 0.0005, &y);
    {
        char vol[16];
        int k = snprintf(vol, sizeof vol, "\x05%dV", UNIT_VOLUME);
        bh_send(v->host, (const unsigned char *)vol, k);
    }
    bh_run(v->host, 0.05, 0.0005, &y);
    bh_set_whine(v->host, whine);
    v->encoding = encoding;
    v->sent_rate = DEFAULT_RATE; v->sent_pitch = DEFAULT_PITCH; v->sent_tone = DEFAULT_TONE;
    v->rate = 50; v->pitch = 50; v->tone = DEFAULT_TONE; v->volume = 100; v->pack = 1;
    return v;
}

BL_API void blv_destroy(bl_voice *v)
{
    if (!v) return;
    if (v->host) bh_destroy(v->host);
    if (v->chip) ssi263_free(v->chip);
    free(v->pcm);
    free(v);
}

BL_API bl_host *blv_host(bl_voice *v) { return v->host; }
BL_API ssi263 *blv_chip(bl_voice *v) { return v->chip; }

BL_API void blv_set(bl_voice *v, int rate, int pitch, int tone, int volume, int pack)
{
    v->rate = rate; v->pitch = pitch; v->tone = tone; v->volume = volume; v->pack = pack != 0;
}

/* ---- _unit_rate, _unit_pitch (int() of a positive float = floor) ---------------------------------------------- */
static int clamp100(int x) { return x < 0 ? 0 : x > 100 ? 100 : x; }

static int unit_rate(int r)
{
    r = clamp100(r);
    return r <= 50 ? 1 + (int)(r * (DEFAULT_RATE - 1) / 50.0 + 0.5)
                   : DEFAULT_RATE + (int)((r - 50) * (MAX_RATE - DEFAULT_RATE) / 50.0 + 0.5);
}

static int unit_pitch(int p)
{
    p = clamp100(p);
    return p <= 50 ? 1 + (int)(p * (DEFAULT_PITCH - 1) / 50.0 + 0.5)
                   : DEFAULT_PITCH + (int)((p - 50) * (63 - DEFAULT_PITCH) / 50.0 + 0.5);
}

/* ---- text as Python sees it: code points, str.isspace ---------------------------------------------------------- */
static int utf8_decode(const char *s, unsigned *out)      /* returns the count; invalid bytes -> U+FFFD */
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p) {
        unsigned c = *p, cp;
        int k, len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len) { out[n++] = 0xFFFD; p++; continue; }
        cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (k = 1; k < len; k++) {
            if ((p[k] & 0xC0) != 0x80) break;
            cp = (cp << 6) | (p[k] & 0x3F);
        }
        if (k < len) { out[n++] = 0xFFFD; p++; continue; }
        out[n++] = cp;
        p += len;
    }
    return n;
}

static int py_isspace(unsigned c)
{
    return (c >= 9 && c <= 13) || (c >= 28 && c <= 32) || c == 0x85 || c == 0xA0 || c == 0x1680
        || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

static int cp850_byte(unsigned c)                           /* -1 if cp850 has no such character */
{
    int i;
    if (c < 0x80) return (int)c;
    for (i = 0; i < 128; i++)
        if (bl_cp850_high[i] == c) return 0x80 + i;
    return -1;
}

/* ---- _clean ---------------------------------------------------------------------------------------------------- */
static int clean(const unsigned *in, int n, int encoding, unsigned *out)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        unsigned c = in[i];
        if (c < 32 || c == 127) out[m++] = ' ';
        else if (c < 128) out[m++] = c;
        else if (encoding == BLV_CP850 && cp850_byte(c) >= 0) out[m++] = c;
        else if (c == 0x2018 || c == 0x2019) out[m++] = '\'';
        else if (c == 0x201C || c == 0x201D) out[m++] = '"';
        else if (c == 0x2013 || c == 0x2014) out[m++] = '-';
        else if (c == 0x2026) { out[m++] = '.'; out[m++] = '.'; out[m++] = '.'; }
        else out[m++] = ' ';
    }
    return m;
}

/* ---- _lines: sentences split at whitespace after . ! ?, words by str.split(), long sentences cut at a comma (4+
   words) or at MAX_WORDS, then packed from the second line on.  Every line is a run of consecutive words. ---------- */
typedef struct { int w0, w1; } span;

static int lines(const unsigned *t, int n, int pack, int *ws, int *we, span *out)
{
    int nw = 0, i = 0, np = 0, no = 0, k;
    span *pieces = (span *)malloc(sizeof(span) * (size_t)(n + 1));
    int sent_w0 = 0;
    if (!pieces) return 0;
    while (i <= n) {
        /* one sentence: up to a whitespace run that follows . ! ? (or the end) */
        int end_of_sentence = 0;
        if (i < n && !py_isspace(t[i])) {
            ws[nw] = i;
            while (i < n && !py_isspace(t[i])) i++;
            we[nw++] = i;
            continue;
        }
        if (i == n) end_of_sentence = 1;
        else {
            int run = i;
            if (run > 0 && (t[run - 1] == '.' || t[run - 1] == '!' || t[run - 1] == '?')) end_of_sentence = 1;
            while (i < n && py_isspace(t[i])) i++;
        }
        if (end_of_sentence) {
            int a = sent_w0, b = nw;
            if (b - a <= MAX_WORDS) {
                if (b > a) { pieces[np].w0 = a; pieces[np].w1 = b; np++; }
            } else {
                int start = a, cnt = 0, w;
                for (w = a; w < b; w++) {
                    cnt++;
                    if ((t[we[w] - 1] == ',' && cnt >= 4) || cnt >= MAX_WORDS) {
                        pieces[np].w0 = start; pieces[np].w1 = w + 1; np++;
                        start = w + 1; cnt = 0;
                    }
                }
                if (cnt) { pieces[np].w0 = start; pieces[np].w1 = b; np++; }
            }
            sent_w0 = nw;
            if (i == n) break;
        }
    }
    for (k = 0; k < np; k++) {
        if (pack && no > 1 && (out[no - 1].w1 - out[no - 1].w0) + (pieces[k].w1 - pieces[k].w0) <= MAX_WORDS)
            out[no - 1].w1 = pieces[k].w1;
        else
            out[no++] = pieces[k];
    }
    free(pieces);
    return no;
}

/* ---- _speakSegment + _speakItems (one text item) ----------------------------------------------------------------- */
BL_API int blv_speak(bl_voice *v, const char *utf8)
{
    int n = (int)strlen(utf8), m, nl, k, w, len = 0;
    unsigned *cps, *t;
    int *ws, *we;
    span *ln;
    unsigned char *data;
    int r = unit_rate(v->rate), p = unit_pitch(v->pitch), tone = v->tone;
    const double *y;
    if (r != v->sent_rate || p != v->sent_pitch || tone != v->sent_tone) {
        char cmd[48];
        int c = snprintf(cmd, sizeof cmd, "\x05%dE\x05%dP\x05%dT", r, p, tone);
        bh_send(v->host, (const unsigned char *)cmd, c);
        bh_run(v->host, 0.02, 0.0005, &y);
        v->sent_rate = r; v->sent_pitch = p; v->sent_tone = tone;
    }
    v->gain = MAKEUP * v->volume / 100.0;
    v->lead = 1;
    v->active = 0;
    cps = (unsigned *)malloc(sizeof(unsigned) * (size_t)(n + 1));
    t = (unsigned *)malloc(sizeof(unsigned) * (size_t)(3 * n + 1));
    ws = (int *)malloc(sizeof(int) * (size_t)(3 * n + 1));
    we = (int *)malloc(sizeof(int) * (size_t)(3 * n + 1));
    ln = (span *)malloc(sizeof(span) * (size_t)(3 * n + 1));
    data = (unsigned char *)malloc((size_t)(3 * n) * 2 + 16);
    if (!cps || !t || !ws || !we || !ln || !data) {
        free(cps); free(t); free(ws); free(we); free(ln); free(data);
        return 0;
    }
    m = clean(cps, utf8_decode(utf8, cps), v->encoding, t);
    nl = lines(t, m, v->pack, ws, we, ln);
    /* unit.say(lines): each line encoded ("replace" -> '?'), then \r ^F; one more \r ^F flushes */
    for (k = 0; k < nl; k++) {
        for (w = ln[k].w0; w < ln[k].w1; w++) {
            int i;
            if (w > ln[k].w0) data[len++] = ' ';
            for (i = ws[w]; i < we[w]; i++) {
                int b = v->encoding == BLV_CP850 ? cp850_byte(t[i]) : (t[i] < 256 ? (int)t[i] : -1);
                data[len++] = (unsigned char)(b < 0 ? '?' : b);
            }
        }
        data[len++] = '\r'; data[len++] = 0x06;
    }
    if (nl) {
        data[len++] = '\r'; data[len++] = 0x06;
        bh_set_int(v->host, "turbo_between_lines", v->pack);
        bh_say(v->host, data, len);
        v->active = 1;
    }
    free(cps); free(t); free(ws); free(we); free(ln); free(data);
    return nl;
}

BL_API int blv_render(bl_voice *v, const short **pcm, int *done)
{
    const double *y;
    int n, start = 0;
    *pcm = v->pcm;
    if (!v->active) { *done = 1; return 0; }
    n = bh_run(v->host, BLOCK_S, 0.0005, &y);
    if (v->lead) {                                   /* _trim_lead */
        int k;
        for (k = 0; k < n; k++)
            if (y[k] > LEAD_THRESHOLD || y[k] < -LEAD_THRESHOLD) break;
        if (k < n) { start = k > LEAD_PREROLL ? k - LEAD_PREROLL : 0; v->lead = 0; }
        else start = n;
    }
    if (n - start > v->pcm_cap) {
        short *b = (short *)realloc(v->pcm, sizeof(short) * (size_t)(n - start));
        if (!b) { *done = 1; v->active = 0; return 0; }
        v->pcm = b;
        v->pcm_cap = n - start;
    }
    if (n > start)
        ssi_pcm16(y + start, n - start, v->gain, v->pcm);
    *pcm = v->pcm;
    *done = 0;
    if (!bh_busy(v->host, 0.1, 3.0)) {
        v->active = 0;
        *done = 1;
    }
    return n - start;
}

BL_API void blv_cancel(bl_voice *v)
{
    bh_cancel(v->host, 3.0, -1.0, -1.0);
    v->active = 0;
}
