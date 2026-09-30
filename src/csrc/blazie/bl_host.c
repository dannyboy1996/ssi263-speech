/* bl_host.c -- src/hosts/blazie.py's Blazie class in C, line for line (see bl_host.h).  Every comparison, every order
 * of operations and every rounding is Python's: run() takes its sample count with round() (half to even:
 * nearbyint), run_until_request() truncates, cycles are int() (truncation) of CLOCK_HZ x dt [x speed].
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bl_board.h"
#include "bl_host.h"
#include "bl_whine_table.h"

#define CLOCK_HZ 6144000.0
#define PLAYING_MAX_S 1.0              /* longer than any phoneme the firmware loads (bh_busy) */

void ssi_onepole(double *x, int n, double b0, double b1, double a1, double *state);   /* ssi263dsp.c */

struct bl_host {
    bl_unit *unit;
    ssi263 *chip;
    double out_rate;
    int board_on;
    double b0, b1, a1, board_state[2];
    int whine, whine_key_r4, whine_key_mode, whine_has_key;
    double whine_key_rate, whine_fc, whine_ph;
    double whine_tab[BL_WHINE_TABLE];
    int whine_tab_ok;
    double cancel_cut, cancel_quiet;
    int ar;                        /* -1: none sent yet (Python's None) */
    unsigned char *tx;
    int n_tx, cap_tx;
    int sent_f, echo_f, stale_f;
    double say_time, last_speech, last_load, turbo, prep_step;
    int preparing, turbo_between_lines;
    double *buf;
    int n_buf, cap_buf;
    int log_on;
    bh_write *wl;
    int n_wl, cap_wl;
};

static int request(const bl_host *h)
{
    return ssi263_request(h->chip) ? 1 : 0;
}

/* Python's _cmd(): the unit's events since the last call, applied in order */
static void events(bl_host *h)
{
    const bl_event *ev;
    int n = bl_events(h->unit, &ev), i;
    for (i = 0; i < n; i++) {
        if (ev[i].type == 'W') {
            int reg = ev[i].a, val = ev[i].b;
            ssi263_write(h->chip, reg, val);
            if (h->log_on) {
                if (h->n_wl == h->cap_wl) {
                    int cap = h->cap_wl ? h->cap_wl * 2 : 1024;
                    bh_write *w = (bh_write *)realloc(h->wl, (size_t)cap * sizeof(bh_write));
                    if (w) { h->wl = w; h->cap_wl = cap; }
                }
                if (h->n_wl < h->cap_wl) {
                    h->wl[h->n_wl].t = ssi263_time(h->chip);
                    h->wl[h->n_wl].reg = reg;
                    h->wl[h->n_wl].val = val;
                    h->n_wl++;
                }
            }
            if (reg == 0 && !(ssi263_reg(h->chip, 3) & 0x80))
                h->last_load = ssi263_time(h->chip);        /* any phoneme, PA included (bh_busy) */
            if (reg == 0 && !(ssi263_reg(h->chip, 3) & 0x80) && (val & 0x3F)) {
                h->last_speech = ssi263_time(h->chip);      /* PA (code 00) is not speech */
                h->preparing = 0;
            }
        } else {
            unsigned char b = ev[i].a;
            if (h->n_tx == h->cap_tx) {
                int cap = h->cap_tx ? h->cap_tx * 2 : 256;
                unsigned char *t = (unsigned char *)realloc(h->tx, (size_t)cap);
                if (t) { h->tx = t; h->cap_tx = cap; }
            }
            if (h->n_tx < h->cap_tx)
                h->tx[h->n_tx++] = b;
            if (b == 0x06) {
                h->echo_f += 1;
                h->say_time = ssi263_time(h->chip);        /* progress: patience counts from here */
                if (h->stale_f > 0)
                    h->stale_f -= 1;                       /* an earlier say()'s held flush line */
                else if (h->turbo_between_lines && bh_owed(h) > 0)
                    h->preparing = 1;                      /* a line is done; the next is being read */
            }
        }
    }
    bl_clear_events(h->unit);
}

static void set_ar(bl_host *h)
{
    int r = request(h);
    if (r != h->ar) {
        h->ar = r;
        bl_set_ar(h->unit, r);
        events(h);
    }
}

static void run_cpu(bl_host *h, double cycles_f)
{
    long long c = (long long)cycles_f;                     /* int(): truncation */
    if (c < 1)
        c = 1;
    bl_run(h->unit, (unsigned long long)c);
    events(h);
}

BL_API bl_host *bh_create(const char *firmware, const char *state, ssi263 *chip, double out_rate, double board_hz,
                          const unsigned long long *key_at, const unsigned char *key_val, int n_keys,
                          unsigned long long boot_instr, int log_writes, char *err, int errlen)
{
    bl_host *h = (bl_host *)calloc(1, sizeof(bl_host));
    if (!h) { snprintf(err, errlen, "out of memory"); return NULL; }
    h->chip = chip;
    h->log_on = log_writes != 0;
    h->out_rate = out_rate;
    if (board_hz > 0.0) {                                  /* dsp.onepole_coeffs: butter(1, hz, fs) */
        double k = tan(M_PI * board_hz / out_rate);
        h->board_on = 1;
        h->b0 = k / (1.0 + k);
        h->b1 = k / (1.0 + k);
        h->a1 = (k - 1.0) / (k + 1.0);
    }
    h->cancel_cut = 0.1;
    h->cancel_quiet = 0.08;
    h->ar = -1;
    h->say_time = 0.0;
    h->turbo = 4.0;
    h->prep_step = 0.0;                                    /* None */
    h->last_speech = -1.0;
    h->last_load = -1.0;
    h->unit = bl_create(firmware, state, 5.0, key_at, key_val, n_keys, err, errlen);
    if (!h->unit) { free(h); return NULL; }
    bl_boot(h->unit, boot_instr);                          /* _cmd("B ...") */
    events(h);
    bl_live(h->unit);                                      /* _cmd("LIVE") */
    events(h);
    return h;
}

BL_API void bh_destroy(bl_host *h)
{
    if (!h)
        return;
    bl_destroy(h->unit);
    free(h->tx);
    free(h->buf);
    free(h->wl);
    free(h);
}

BL_API int bh_writes(const bl_host *h, const bh_write **writes)
{
    *writes = h->wl;
    return h->n_wl;
}

BL_API void bh_clear_writes(bl_host *h)
{
    h->n_wl = 0;
}

BL_API void bh_send(bl_host *h, const unsigned char *data, int n)
{
    int i;
    if (n <= 0)
        return;
    for (i = 0; i < n; i++)
        if (data[i] == 0x06)
            h->sent_f++;
    bl_queue(h->unit, data, n);
    events(h);
}

BL_API void bh_say(bl_host *h, const unsigned char *data, int n)
{
    h->say_time = ssi263_time(h->chip);
    h->preparing = 1;
    h->stale_f = h->sent_f - h->echo_f > 0 ? h->sent_f - h->echo_f : 0;   /* echoes still due from earlier sends */
    bh_send(h, data, n);
}

BL_API int bh_owed(const bl_host *h)
{
    return h->sent_f - 1 - h->echo_f;
}

BL_API int bh_busy(const bl_host *h, double quiet, double patience)
{
    double now = ssi263_time(h->chip);
    double since = h->say_time > h->last_speech ? h->say_time : h->last_speech;   /* max(say_time, last_speech) */
    if ((now - h->last_speech) < quiet)
        return 1;
    /* the firmware is still inside an utterance while the chip plays a phoneme it loaded -- when speech has loaded
       since the last ^F echo (see blazie.py's busy) */
    if (h->last_speech > h->say_time && !request(h) && (now - h->last_load) < PLAYING_MAX_S)
        return 1;
    return bh_owed(h) > 0 && (now - since) < patience;
}

BL_API double bh_skip(bl_host *h, double seconds)
{
    double t = 0.0;
    while (t < seconds - 1e-9) {
        double before, dt;
        set_ar(h);
        before = ssi263_time(h->chip);
        ssi263_skip(h->chip, seconds - t);
        dt = ssi263_time(h->chip) - before;
        if (dt < 1e-5)
            dt = 1e-5;                                     /* max(..., 1e-5) */
        run_cpu(h, CLOCK_HZ * dt);
        t += dt;
    }
    return t;
}

BL_API double bh_cancel(bl_host *h, double limit, double quiet, double cut)
{
    int holding;
    double t = 0.0;
    h->sent_f -= bl_drop(h->unit);                         /* _cmd("D"): drop what the unit has not taken yet */
    events(h);
    holding = bh_owed(h) == 0;                             /* decided after the drop (see blazie.py) */
    h->preparing = 0;
    if (cut < 0.0)
        cut = h->cancel_cut;
    if (quiet < 0.0)
        quiet = h->cancel_quiet;
    while (t < limit) {
        bl_urgent(h->unit, 0x18);                          /* _cmd("U 18") */
        events(h);
        t += bh_skip(h, cut);
        if (t >= 0.04 && ssi263_time(h->chip) - h->last_speech > quiet
                && (bh_owed(h) <= 0 || t >= 1.0))
            break;
    }
    h->echo_f = holding ? h->sent_f - 1 : h->sent_f;
    return t;
}

static int reserve(bl_host *h, int n)
{
    if (h->n_buf + n > h->cap_buf) {
        int cap = h->cap_buf ? h->cap_buf : 4096;
        double *b;
        while (cap < h->n_buf + n)
            cap *= 2;
        b = (double *)realloc(h->buf, (size_t)cap * sizeof(double));
        if (!b)
            return 0;
        h->buf = b;
        h->cap_buf = cap;
    }
    return 1;
}

/* whine_wave(): one 64-tick period at tone register r4, band-limited under 0.45 x rate */
static int whine_wave(bl_host *h, int r4, double rate, int mode)
{
    const double xck = 1e6;
    int n_div = 256 - r4, tone, n, j, nl = 0;
    double fc, lv[65];
    int has[65];
    unsigned long long seed = BL_WHINE_SEED;
    int ln[64];
    double la[64], lp[64];
    const char *s;
    if (n_div <= 0)
        return 0;
    fc = xck / (2.0 * n_div);
    tone = 32 - n_div;
    if (tone > 26) tone = 26;
    if (tone < 0) tone = 0;
    memset(has, 0, sizeof has);
    for (s = BL_WHINE_TONES[mode - 1][tone]; *s; ) {
        char *end;
        long k = strtol(s, &end, 10);
        if (end == s || *end != ':')
            break;
        lv[k] = strtod(end + 1, &end);
        has[k] = 1;
        s = end;
        while (*s == ' ') s++;
    }
    for (n = 1; n < 65; n++) {
        seed = (seed * 1103515245ULL + 12345ULL) & 0x7FFFFFFFULL;
        if (has[n] && n * fc / 64.0 < 0.45 * rate) {
            ln[nl] = n;
            la[nl] = sqrt(2.0) * pow(10.0, lv[n] / 20.0) * BL_WHINE_VOWEL_RMS;
            lp[nl] = 2 * M_PI * (double)seed / 0x7FFFFFFF;
            nl++;
        }
    }
    for (j = 0; j < BL_WHINE_TABLE; j++) {
        double sum = 0.0;
        int i;
        for (i = 0; i < nl; i++)
            sum += la[i] * cos(2 * M_PI * ln[i] * j / BL_WHINE_TABLE + lp[i]);
        h->whine_tab[j] = sum;
    }
    h->whine_fc = fc;
    return 1;
}

static void add_whine(bl_host *h, double *y, int n)
{
    int r4 = ssi263_reg(h->chip, 4), r3 = ssi263_reg(h->chip, 3), i;
    double inc, ph;
    if (!h->whine_has_key || r4 != h->whine_key_r4 || h->out_rate != h->whine_key_rate || h->whine != h->whine_key_mode) {
        h->whine_has_key = 1;
        h->whine_key_r4 = r4;
        h->whine_key_rate = h->out_rate;
        h->whine_key_mode = h->whine;
        h->whine_tab_ok = whine_wave(h, r4, h->out_rate, h->whine);
    }
    if (!h->whine_tab_ok || !n)
        return;
    inc = h->whine_fc / 64.0 / h->out_rate;
    ph = h->whine_ph;
    if ((r3 & 0x80) || !(r3 & 0x70)) {
        h->whine_ph = fmod(ph + inc * n, 1.0);
        return;
    }
    for (i = 0; i < n; i++) {
        double pos = ph * BL_WHINE_TABLE, fr;
        int j = (int)pos;
        fr = pos - j;
        y[i] += h->whine_tab[j % BL_WHINE_TABLE] * (1.0 - fr) + h->whine_tab[(j + 1) % BL_WHINE_TABLE] * fr;
        ph += inc;
        if (ph >= 1.0)
            ph -= 1.0;
    }
    h->whine_ph = ph;
}

BL_API int bh_run(bl_host *h, double seconds, double step, const double **audio)
{
    double t = 0.0;
    h->n_buf = 0;
    while (t < seconds) {
        double before, st, dt, speed;
        long n, got;
        set_ar(h);
        before = ssi263_time(h->chip);
        st = (h->preparing && h->prep_step > 0.0) ? h->prep_step : step;
        if (!request(h)) {
            n = (long)(st * h->out_rate);                  /* int(): truncation */
            if (!reserve(h, (int)n + 1)) break;
            got = ssi263_run_until_request(h->chip, n, h->buf + h->n_buf);
        } else {
            n = (long)nearbyint(st / 4 * h->out_rate);     /* round(): half to even */
            if (!reserve(h, (int)n + 1)) break;
            got = ssi263_run(h->chip, n, h->buf + h->n_buf);
        }
        h->n_buf += (int)got;
        dt = ssi263_time(h->chip) - before;
        if (dt < 1e-5)
            dt = 1e-5;
        speed = h->preparing ? h->turbo : 1.0;
        run_cpu(h, CLOCK_HZ * dt * speed);
        t += dt;
    }
    if (h->board_on && h->n_buf)
        ssi_onepole(h->buf, h->n_buf, h->b0, h->b1, h->a1, h->board_state);
    if (h->whine)
        add_whine(h, h->buf, h->n_buf);
    *audio = h->buf;
    return h->n_buf;
}

BL_API void bh_set_whine(bl_host *h, int mode)
{
    h->whine = (mode >= 0 && mode <= 2) ? mode : 0;
}

BL_API int bh_get_whine(const bl_host *h)
{
    return h->whine;
}

BL_API int bh_get_int(const bl_host *h, const char *name)
{
    if (!strcmp(name, "sent_f")) return h->sent_f;
    if (!strcmp(name, "echo_f")) return h->echo_f;
    if (!strcmp(name, "stale_f")) return h->stale_f;
    if (!strcmp(name, "preparing")) return h->preparing;
    if (!strcmp(name, "turbo_between_lines")) return h->turbo_between_lines;
    if (!strcmp(name, "ar")) return h->ar;
    if (!strcmp(name, "log_writes")) return h->log_on;
    return 0;
}

BL_API void bh_set_int(bl_host *h, const char *name, int v)
{
    if (!strcmp(name, "sent_f")) h->sent_f = v;
    else if (!strcmp(name, "echo_f")) h->echo_f = v;
    else if (!strcmp(name, "stale_f")) h->stale_f = v;
    else if (!strcmp(name, "preparing")) h->preparing = v;
    else if (!strcmp(name, "turbo_between_lines")) h->turbo_between_lines = v;
    else if (!strcmp(name, "log_writes")) h->log_on = v != 0;
}

BL_API double bh_get_double(const bl_host *h, const char *name)
{
    if (!strcmp(name, "last_speech")) return h->last_speech;
    if (!strcmp(name, "say_time")) return h->say_time;
    if (!strcmp(name, "turbo")) return h->turbo;
    if (!strcmp(name, "prep_step")) return h->prep_step;
    if (!strcmp(name, "cancel_cut")) return h->cancel_cut;
    if (!strcmp(name, "cancel_quiet")) return h->cancel_quiet;
    return 0.0;
}

BL_API void bh_set_double(bl_host *h, const char *name, double v)
{
    if (!strcmp(name, "last_speech")) h->last_speech = v;
    else if (!strcmp(name, "say_time")) h->say_time = v;
    else if (!strcmp(name, "turbo")) h->turbo = v;
    else if (!strcmp(name, "prep_step")) h->prep_step = v;
    else if (!strcmp(name, "cancel_cut")) h->cancel_cut = v;
    else if (!strcmp(name, "cancel_quiet")) h->cancel_quiet = v;
}

BL_API int bh_tx(const bl_host *h, const unsigned char **bytes)
{
    *bytes = h->tx;
    return h->n_tx;
}

int bh_key(bl_host *h, int chord)
{
    return bl_key(h->unit, chord);
}

int bh_save_state(const bl_host *h, const char *path)
{
    return bl_save_state(h->unit, path);
}
