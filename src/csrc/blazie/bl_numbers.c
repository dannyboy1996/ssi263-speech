/* bl_numbers.c -- see bl_numbers.h.  Each part names the driver function it ports (blazie.py); change them together. */
#include <stdlib.h>
#include <string.h>

#include "bl_numbers.h"
#include "bl_voice.h"
#include "../numwords.h"
#include "../numwords_es.h"

typedef struct {
    char *p;
    size_t n, cap;
    int oom;
} sbuf;

static void sb_putn(sbuf *b, const char *s, size_t k)
{
    if (b->oom) return;
    if (b->n + k + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        char *q;
        while (cap < b->n + k + 1) cap *= 2;
        q = (char *)realloc(b->p, cap);
        if (!q) { b->oom = 1; return; }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, k);
    b->n += k;
    b->p[b->n] = 0;
}

static void sb_puts(sbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }

/* numwords.normalise(s[a..e)) appended */
static void sb_normalise(sbuf *b, const char *s, size_t a, size_t e)
{
    char *part = (char *)malloc(e - a + 1), *w;
    if (!part) { b->oom = 1; return; }
    memcpy(part, s + a, e - a);
    part[e - a] = 0;
    w = nw_normalise(part);
    free(part);
    if (!w) { b->oom = 1; return; }
    sb_puts(b, w);
    free(w);
}

static int dig(char c) { return c >= '0' && c <= '9'; }

/* _money(amount): "$" + digits/commas (+ "." + digits) kept for the firmware up to 12 significant digits */
static void money(sbuf *b, const char *s, size_t a, size_t e)
{
    static const char *ones[10] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
    char *whole = (char *)malloc(e - a + 1);
    size_t k, nw = 0, dot = e, sig;
    if (!whole) { b->oom = 1; return; }
    for (k = a + 1; k < e; k++)
        if (s[k] == '.') { dot = k; break; }
    for (k = a + 1; k < dot; k++)
        if (s[k] != ',') whole[nw++] = s[k];
    whole[nw] = 0;
    for (sig = 0, k = 0; k < nw && whole[k] == '0'; k++) ;
    sig = nw - k;                                            /* len(whole.lstrip("0")) */
    if (sig <= 12) {
        sb_putn(b, s + a, e - a);
        free(whole);
        return;
    }
    sb_normalise(b, whole, 0, nw);
    free(whole);
    sb_puts(b, " dollars");
    if (dot < e && e - dot - 1 == 2) {                       /* cardinal(int(cents)) + " cents" */
        sb_puts(b, " ");
        sb_normalise(b, s, dot + 1, e);
        sb_puts(b, " cents");
    } else if (dot < e && e - dot - 1 > 0) {                 /* " point " + digits(cents) */
        sb_puts(b, " point");
        for (k = dot + 1; k < e; k++) { sb_puts(b, " "); sb_puts(b, ones[s[k] - '0']); }
    }
}

/* MONEY = (\$\d[\d,]*(?:\.\d+)?|\$\.\d+): the match's end at i, 0 for none */
static size_t money_at(const char *s, size_t n, size_t i)
{
    size_t e;
    if (s[i] != '$' || i + 1 >= n) return 0;
    if (dig(s[i + 1])) {
        for (e = i + 2; e < n && (dig(s[e]) || s[e] == ','); e++) ;
        if (e + 1 < n && s[e] == '.' && dig(s[e + 1]))
            for (e += 2; e < n && dig(s[e]); e++) ;
        return e;
    }
    if (s[i + 1] == '.' && i + 2 < n && dig(s[i + 2])) {
        for (e = i + 3; e < n && dig(s[e]); e++) ;
        return e;
    }
    return 0;
}

BL_API char *bl_numbers(const char *s, int encoding)
{
    if (encoding == BLV_CP850) {                             /* normalise(text, lang="es", decimal_comma=True) */
        size_t n = strlen(s);
        unsigned *cp = (unsigned *)malloc(sizeof(unsigned) * (n + 1));
        char *out;
        if (!cp) return NULL;
        out = nw_normalise_es(cp, nw_utf8(s, cp));
        free(cp);
        return out;
    } else {                                                 /* MONEY.split: money kept (_money), the rest normalised */
        sbuf b = {0};
        size_t n = strlen(s), i = 0, start = 0, e;
        sb_puts(&b, "");
        while (i < n && !b.oom) {
            e = money_at(s, n, i);
            if (!e) { i++; continue; }
            sb_normalise(&b, s, start, i);
            money(&b, s, i, e);
            i = start = e;
        }
        sb_normalise(&b, s, start, n);
        if (b.oom) { free(b.p); return NULL; }
        return b.p;
    }
}
