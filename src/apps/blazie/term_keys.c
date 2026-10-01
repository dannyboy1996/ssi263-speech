/* term_keys.c -- see term_keys.h. */
#include <string.h>
#include "term_keys.h"

void term_dec_init(term_dec *d)
{
    memset(d, 0, sizeof *d);
}

/* one byte on its own: 1 and the key, or 0 (nothing: a byte of a UTF-8 character) */
static int byte_key(term_dec *d, int c, key_event *e)
{
    e->type = KE_PRESS;
    e->mods = 0;
    if (d->utf8 > 0 && c >= 0x80 && c < 0xC0) {
        d->utf8--;
        return 0;
    }
    d->utf8 = 0;
    if (c >= 0x80) {                            /* beyond ASCII: skipped with its continuation bytes */
        d->utf8 = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        return 0;
    }
    if (c == 0x0D || c == 0x0A) e->key = K_ENTER;
    else if (c == 0x09) e->key = K_TAB;
    else if (c == 0x08 || c == 0x7F) e->key = K_BACKSPACE;
    else if (c == 0x1B) e->key = K_ESC;
    else if (c == 0x00) { e->key = ' '; e->mods = KM_CTRL; }
    else if (c < 0x1B) { e->key = 'a' + c - 1; e->mods = KM_CTRL; }
    else if (c == 0x1C) { e->key = '\\'; e->mods = KM_CTRL; }
    else if (c == 0x1D) { e->key = ']'; e->mods = KM_CTRL; }
    else if (c == 0x1E) { e->key = '^'; e->mods = KM_CTRL; }
    else if (c == 0x1F) { e->key = '-'; e->mods = KM_CTRL; }
    else e->key = c;
    return 1;
}

static int tilde_key(int n)                     /* ESC [ n ~ */
{
    switch (n) {
    case 1: case 7: return K_HOME;
    case 2: return K_INSERT;
    case 3: return K_DELETE;
    case 4: case 8: return K_END;
    case 5: return K_PGUP;
    case 6: return K_PGDN;
    case 11: case 12: case 13: case 14: case 15: return K_F1 + n - 11;
    case 17: case 18: case 19: case 20: case 21: return K_F6 + n - 17;
    case 23: return K_F11;
    case 24: return K_F12;
    }
    return 0;
}

static int final_key(int c)                     /* ESC [ ... c, ESC O c */
{
    switch (c) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case 'P': case 'Q': case 'R': case 'S': return K_F1 + c - 'P';
    }
    return 0;
}

/* the sequence at the start of b (n bytes): 1 a key in *e, 0 none (dropped), -1 incomplete; *used: bytes taken.
   force: no more bytes are coming (TERM_ESC_S passed) */
static int parse(term_dec *d, const unsigned char *b, int n, int force, key_event *e, int *used)
{
    int i;
    e->type = KE_PRESS;
    e->mods = 0;
    if (b[0] != 0x1B) {
        *used = 1;
        return byte_key(d, b[0], e);
    }
    if (n == 1) {
        if (!force)
            return -1;
        *used = 1;
        e->key = K_ESC;
        return 1;
    }
    if (b[1] == 0x1B) {                         /* Esc, and a new sequence starts */
        *used = 1;
        e->key = K_ESC;
        return 1;
    }
    if (b[1] == '[' || b[1] == 'O') {
        if (n == 2) {
            if (!force)
                return -1;
            *used = 2;                          /* Alt+[ or Alt+Shift+O */
            e->key = b[1] == '[' ? '[' : 'O';
            e->mods = KM_ALT;
            return 1;
        }
        if (b[1] == 'O') {                      /* SS3: the application cursor keys, F1-F4 */
            *used = 3;
            e->key = b[2] == 'M' ? K_ENTER : final_key(b[2]);
            return e->key != 0;
        }
        if (b[2] == '[') {                      /* the Linux console's F1-F5: ESC [ [ A..E */
            if (n == 3) {
                if (!force)
                    return -1;
                *used = 3;
                return 0;
            }
            *used = 4;
            e->key = b[3] >= 'A' && b[3] <= 'E' ? K_F1 + b[3] - 'A' : 0;
            return e->key != 0;
        }
        for (i = 2; i < n; i++)                 /* CSI: parameters and intermediates, then the final byte */
            if (b[i] >= 0x40 && b[i] <= 0x7E)
                break;
        if (i == n) {
            if (!force && n < (int)sizeof d->buf)
                return -1;
            *used = n;
            return 0;
        }
        *used = i + 1;
        {
            int p[2] = {0, 0}, k = 0, j, sub = 0;
            for (j = 2; j < i; j++) {
                if (b[j] >= '0' && b[j] <= '9') {
                    if (!sub && p[k] < 1000)
                        p[k] = p[k] * 10 + (b[j] - '0');
                } else if (b[j] == ';') {
                    sub = 0;
                    if (++k > 1)
                        break;
                } else if (b[j] == ':')
                    sub = 1;                    /* a sub-parameter (an event type): not read */
                else
                    return 0;                   /* a private sequence (?, >, <): not a key */
            }
            if (b[i] == '~')
                e->key = tilde_key(p[0]);
            else if (b[i] == 'Z') {
                e->key = K_TAB;
                e->mods = KM_SHIFT;
                return 1;
            } else
                e->key = final_key(b[i]);
            if (p[1] > 1)
                e->mods = (p[1] - 1) & 7;
            return e->key != 0;
        }
    }
    *used = 2;                                  /* ESC then a key: that key with Alt */
    if (!byte_key(d, b[1], e))
        return 0;
    e->mods |= KM_ALT;
    return 1;
}

static int drain(term_dec *d, int force, key_event *out, int cap)
{
    int got = 0;
    while (d->n > 0 && got < cap) {
        int used = 0, r = parse(d, d->buf, d->n, force, &out[got], &used);
        if (r < 0)
            break;
        if (r > 0)
            got++;
        if (used <= 0)
            used = 1;
        memmove(d->buf, d->buf + used, (size_t)(d->n - used));
        d->n -= used;
    }
    return got;
}

int term_dec_feed(term_dec *d, const unsigned char *bytes, int n, double now, key_event *out, int cap)
{
    int got = 0, i;
    for (i = 0; i < n; i++) {
        if (d->n == (int)sizeof d->buf)         /* never complete: dropped */
            d->n = 0;
        if (d->n == 0)
            d->since = now;
        d->buf[d->n++] = bytes[i];
        got += drain(d, 0, out + got, cap - got);
    }
    return got;
}

int term_dec_flush(term_dec *d, double now, key_event *out, int cap)
{
    if (d->n == 0 || now - d->since < TERM_ESC_S)
        return 0;
    return drain(d, 1, out, cap);
}

double term_dec_deadline(const term_dec *d)
{
    return d->n ? d->since + TERM_ESC_S : -1.0;
}
