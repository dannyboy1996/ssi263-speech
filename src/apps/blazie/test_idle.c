/* test_idle.c -- the Braille Lite's idle channel in the emulator (../../csrc/blazie/bl_idle.c), against what Tomi's
 * unit does (src/hosts/blazie_idle.py; tools/idle_sounds.py measures it):
 *
 *   hiss level          the open channel's noise and lines, 60 Hz-10 kHz, -47.1 dB re the volume-6 vowel on the unit
 *   hiss vs volume      the same level at volume 1 and 15 (the unit: -46.5 and -46.5 dB re that vowel)
 *   until the click-off the channel silent after the firmware clicks it off
 *   always              the channel still heard 12-14 s after speech, past the click-off
 *   off                 nothing after speech
 *   pop                 a pop when a line opens the channel after the click-off, before its first phoneme
 *   no pop              none when a line comes before the click-off
 *   click               a click at the click-off (R3 = 00)
 *   tick                the 10 Hz tick in the open channel (folded at its period)
 *
 * The unit in speech-box mode, as the NVDA driver boots it, driven with ^E<n>V and lines.
 *
 *   test_idle FIRMWARE STATE [--break=KIND]
 * --break puts a bug back, for run_tests' must-fail controls: old-level (the drivers' hiss table instead),
 * follow-volume (the hiss scaled with the volume), until (keep_open "until" where always/off are asked), no-pop
 * (no pop or click), pop-every-line (a pop at every line), no-tick, tick-after-off (the tick kept after the click-off).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../csrc/ssi263.h"
#include "../../csrc/blazie/bl_host.h"
#include "../../csrc/blazie/bl_idle_table.h"     /* the unit's tick shape, for the matched fold */

#define RATE 44100.0
#define BLOCK 0.02
#define KEY_START 8000000ULL
#define KEY_GAP 10000000ULL
#define LINE "Readme for Microsoft Windows."

extern int bli_test_break;          /* bl_idle.c built with BLI_TEST_HOOKS */

static const char *g_fw, *g_st, *g_break = "";
static int failures;

static void check(const char *name, int ok, const char *detail)
{
    printf("%-4s %-40s %s\n", ok ? "ok" : "FAIL", name, detail);
    failures += !ok;
}

typedef struct {
    ssi263 *chip;
    bl_host *h;
    double t0;                      /* chip time at sample 0 */
    double *y;
    int n, cap;
} run_t;

static void start(run_t *r, int volume, int sound, int keep_open, int pop_click, int tick, int old_whine)
{
    char err[256], cmd[32];
    ssi263_params p;
    static const unsigned char codes[4] = {0x5C, 0x7F, 0x07, 0x51};   /* hosts/blazie.py boot_keys(): speech box */
    unsigned long long at[4];
    int i;
    for (i = 0; i < 4; i++)
        at[i] = KEY_START + i * KEY_GAP;
    ssi263_default_params(&p);
    p.closure_noise_lead_s = 0.010;
    p.carrier_rel_db = -300.0;
    r->chip = ssi263_new(&p, ssi263_default_rom(), RATE);
    r->h = bh_create(g_fw, g_st, r->chip, RATE, 5000.0, at, codes, 4, KEY_START + 4 * KEY_GAP, 1, err, sizeof err);
    if (!r->h) {
        printf("FAIL create: %s\n", err);
        exit(1);
    }
    bh_set_double(r->h, "turbo", 1.0);
    if (old_whine)
        bh_set_whine(r->h, 1);
    else {
        bl_idle_options o;
        o.sound = sound;
        o.keep_open = keep_open;
        o.pop_click = pop_click;
        o.tick = tick;
        bh_set_idle(r->h, &o);
    }
    snprintf(cmd, sizeof cmd, "\x05%dV\x05" "7T", volume);
    bh_send(r->h, (const unsigned char *)cmd, (int)strlen(cmd));
    r->y = NULL;
    r->n = r->cap = 0;
    r->t0 = ssi263_time(r->chip);
}

static void render(run_t *r, double seconds)
{
    int k;
    for (k = 0; k < (int)(seconds / BLOCK + 0.5); k++) {
        const double *a;
        int m = bh_run(r->h, BLOCK, 0.0005, &a);
        if (r->n + m > r->cap) {
            r->cap = (r->n + m) * 2;
            r->y = (double *)realloc(r->y, sizeof(double) * (size_t)r->cap);
        }
        memcpy(r->y + r->n, a, sizeof(double) * (size_t)m);
        r->n += m;
    }
}

static double say(run_t *r)          /* returns the chip time the line was sent */
{
    static const char line[] = LINE "\r\x06\r\x06";
    double t = ssi263_time(r->chip);
    bh_say(r->h, (const unsigned char *)line, (int)strlen(line));
    return t;
}

static int idx(const run_t *r, double t)
{
    int i = (int)((t - r->t0) * RATE + 0.5);
    return i < 0 ? 0 : i > r->n ? r->n : i;
}

/* speech loads (R0 with a phoneme, R3's CTL clear) after time a: the first and the last; the first R3 = 00 after a */
static void loads(const run_t *r, double a, double b, double *first, double *last, double *off)
{
    const bh_write *w;
    int n = bh_writes(r->h, &w), i, r3 = 0;
    *first = *last = *off = -1.0;
    for (i = 0; i < n; i++) {
        if (w[i].reg == 3)
            r3 = w[i].val;
        if (w[i].t < a || w[i].t > b)
            continue;
        if (w[i].reg == 0 && !(r3 & 0x80) && (w[i].val & 0x3F)) {
            if (*first < 0)
                *first = w[i].t;
            *last = w[i].t;
        }
        if (w[i].reg == 3 && w[i].val == 0 && *off < 0)
            *off = w[i].t;
    }
}

/* 4th-order Butterworth sections (RBJ biquads), as tools/idle_sounds.py's scipy butter(4, ...) */
typedef struct { double b0, b1, b2, a1, a2, z1, z2; } biquad;

static void bq_init(biquad *q, int high, double f, double qf)
{
    double w = 2 * M_PI * f / RATE, al = sin(w) / (2 * qf), c = cos(w), a0 = 1 + al;
    q->b1 = (high ? -(1 + c) : (1 - c)) / a0;
    q->b0 = q->b2 = (high ? (1 + c) : (1 - c)) / 2 / a0;
    q->a1 = -2 * c / a0;
    q->a2 = (1 - al) / a0;
    q->z1 = q->z2 = 0;
}

static double bq(biquad *q, double x)
{
    double y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

/* the rms (dB) of r->y over [a, b) s of chip time, band-passed 60 Hz-10 kHz (the tool's hiss band) */
static double band_db(const run_t *r, double a, double b)
{
    int i0 = idx(r, a - 0.3), i1 = idx(r, a), i2 = idx(r, b), i;
    biquad q[4];
    double s = 0;
    bq_init(&q[0], 1, 60, 0.5412);
    bq_init(&q[1], 1, 60, 1.3066);
    bq_init(&q[2], 0, 10000, 0.5412);
    bq_init(&q[3], 0, 10000, 1.3066);
    for (i = i0; i < i2; i++) {
        double v = bq(&q[3], bq(&q[2], bq(&q[1], bq(&q[0], r->y[i]))));
        if (i >= i1)
            s += v * v;
    }
    return 10 * log10(s / (i2 > i1 ? i2 - i1 : 1) + 1e-30);
}

/* the loud-vowel level (dB): 20 ms frames of the 60 Hz high-passed speech within 6 dB of the loudest */
static double vowel_db(const run_t *r, double a, double b)
{
    int i0 = idx(r, a), i1 = idx(r, b), h = (int)(0.02 * RATE), i, k, nf = 0;
    double e[1024], mx = 0, s = 0;
    int cnt = 0;
    biquad q[2];
    bq_init(&q[0], 1, 60, 0.5412);
    bq_init(&q[1], 1, 60, 1.3066);
    for (i = i0; i + h <= i1 && nf < 1024; i += h) {
        double acc = 0;
        for (k = 0; k < h; k++) {
            double y1 = bq(&q[1], bq(&q[0], r->y[i + k]));
            acc += y1 * y1;
        }
        e[nf] = acc / h;
        if (e[nf] > mx)
            mx = e[nf];
        nf++;
    }
    for (k = 0; k < nf; k++)
        if (e[k] > mx * 0.2512)
            s += e[k], cnt++;
    return 10 * log10(s / (cnt ? cnt : 1) + 1e-30);
}

/* the largest |y| after a 20 Hz low-pass (two one-pole stages) over [a, b): the pop's and click's low end, which
   speech lacks */
static double low_peak(const run_t *r, double a, double b)
{
    int i0 = idx(r, a - 0.3), i1 = idx(r, a), i2 = idx(r, b), i;
    double g = 1 - exp(-2 * M_PI * 20 / RATE), l1 = 0, l2 = 0, mx = 0;
    for (i = i0; i < i2; i++) {
        l1 += g * (r->y[i] - l1);
        l2 += g * (l1 - l2);
        if (i >= i1 && fabs(l2) > mx)
            mx = fabs(l2);
    }
    return mx;
}

/* the 10 Hz tick: the open idle (60 Hz high-passed) folded at the period, then matched against the unit's tick shape
   (the measured template) at every lag: the best match against the matches elsewhere.  A comb line close to a
   multiple of 10 Hz (2.5 kHz at tone 7) folds too, but does not look like the tick. */
static double tick_ratio(const run_t *r, double a, double b, double *dip)
{
    double per = 0.09998 * RATE, f[5000], tpl[1000], c[5000], mean = 0, sd = 0, best = 0;
    int m = (int)((b - a) / 0.09998) - 1, len = (int)per, nt = (int)(BLI_TICK_LEN * RATE / BLI_TICK_RATE) - 1;
    int k, j, jm = 0, cnt = 0;
    memset(f, 0, sizeof f);
    for (k = 0; k < m; k++) {
        int s0 = idx(r, a) + (int)(k * per + 0.5);
        biquad q[2];
        bq_init(&q[0], 1, 60, 0.5412);
        bq_init(&q[1], 1, 60, 1.3066);
        for (j = -2000; j < len; j++) {               /* settled first */
            double y1 = bq(&q[1], bq(&q[0], r->y[s0 + j]));
            if (j >= 0)
                f[j] += y1 / m;
        }
    }
    for (j = 0; j < nt; j++) {                        /* the template at this rate */
        double p = j * BLI_TICK_RATE / RATE;
        int i = (int)p;
        tpl[j] = BLI_TICK[i] + (BLI_TICK[i + 1] - BLI_TICK[i]) * (p - i);
        mean += tpl[j] / nt;
    }
    for (j = 0; j < nt; j++)
        tpl[j] -= mean;
    for (k = 0; k < len; k++) {
        double s = 0;
        for (j = 0; j < nt; j++)
            s += f[(k + j) % len] * tpl[j];
        c[k] = s;
        if (fabs(s) > fabs(best))
            best = s, jm = k;
    }
    for (k = 0; k < len; k++) {
        int d = abs(k - jm);
        if (d > len / 2)
            d = len - d;
        if (d > nt)
            sd += c[k] * c[k], cnt++;
    }
    *dip = f[(jm + (int)(0.0022 * RATE)) % len];      /* the fold at the template's dip */
    return best / sqrt(sd / (cnt ? cnt : 1) + 1e-30);
}

static void finish(run_t *r)
{
    bh_destroy(r->h);
    ssi263_free(r->chip);
    free(r->y);
}

int main(int argc, char **argv)
{
    char d[256];
    run_t r;
    double s1, s2, s3, f1, l1, off1, f2, l2, off2, f3, l3, off3, hiss6, v6, hiss1, hiss15, open, after, dip, ratio, pk;
    int brk_until, brk_nopop, brk_notick, i;
    if (argc < 3) {
        printf("usage: test_idle FIRMWARE STATE [--break=old-level|follow-volume|until|no-pop|pop-every-line|no-tick|tick-after-off]\n");
        return 2;
    }
    g_fw = argv[1];
    g_st = argv[2];
    for (i = 3; i < argc; i++)
        if (!strncmp(argv[i], "--break=", 8))
            g_break = argv[i] + 8;
    bli_test_break = !strcmp(g_break, "follow-volume") ? 1 : !strcmp(g_break, "pop-every-line") ? 2 : 0;
    brk_until = !strcmp(g_break, "until");
    brk_nopop = !strcmp(g_break, "no-pop");
    brk_notick = !strcmp(g_break, "no-tick");

    /* volume 6, until the click-off: a line; the click-off; a line after it (the pop); a line before the next one */
    start(&r, 6, BLI_SOUND_BY_VOLUME, BLI_OPEN_UNTIL_CLICK, !brk_nopop, !brk_notick, !strcmp(g_break, "old-level"));
    render(&r, 1.0);
    s1 = say(&r);
    render(&r, 16.0);
    s2 = say(&r);
    render(&r, 6.0);
    s3 = say(&r);
    render(&r, 4.0);
    loads(&r, s1, s2, &f1, &l1, &off1);
    loads(&r, s2, s3, &f2, &l2, &off2);
    loads(&r, s3, s3 + 4.0, &f3, &l3, &off3);
    if (f1 < 0 || f2 < 0 || f3 < 0 || off1 < 0) {
        printf("FAIL the unit did not speak its lines or never clicked off (%.2f %.2f %.2f, off %.2f)\n", f1, f2, f3, off1);
        return 1;
    }
    v6 = vowel_db(&r, f1, l1 + 0.1);
    hiss6 = band_db(&r, l1 + 1.2, l1 + 8.2);
    snprintf(d, sizeof d, "%.1f dB re the volume-6 vowel (the unit: -47.1; the drivers' table: about -67)", hiss6 - v6);
    check("hiss level at volume 6", fabs(hiss6 - v6 + 47.1) < 2.0, d);
    after = band_db(&r, off1 + 1.0, s2 - 0.05);
    snprintf(d, sizeof d, "%.1f dB after the click-off (%.1f s after the last phoneme), %.1f dB while open",
             after, off1 - l1, hiss6);
    check("until the click-off: silent after it", after < hiss6 - 30.0, d);
    pk = low_peak(&r, f2 - 0.4, f2 + 0.15);
    {
        double lead = -1.0;                           /* the pop's edge before the first phoneme (the unit: ~0.27 s) */
        for (i = idx(&r, f2 - 0.6); i < idx(&r, f2); i++)
            if (r.y[i] > 0.05) {
                lead = f2 - (r.t0 + i / RATE);
                break;
            }
        snprintf(d, sizeof d, "20 Hz low-passed peak %.4f, its edge %.3f s before the first phoneme, after %.1f s closed",
                 pk, lead, f2 - off1);
        check("pop on reopening after the click-off", pk > 0.05 && lead > 0.2 && lead < 0.35, d);
    }
    pk = low_peak(&r, f3 - 0.4, f3 + 0.15);
    snprintf(d, sizeof d, "20 Hz low-passed peak %.4f around the first phoneme, %.1f s after the last line",
             pk, f3 - l2);
    check("no pop on reopening before the click-off", pk < 0.01, d);
    pk = low_peak(&r, off1 - 0.005, off1 + 0.1);
    snprintf(d, sizeof d, "20 Hz low-passed peak %.4f at R3 = 00", pk);
    check("click at the click-off", pk > 0.03, d);
    finish(&r);

    /* the tick alone (no noise or lines): there while the channel is open, gone once it is clicked off */
    start(&r, 6, BLI_SOUND_NONE, !strcmp(g_break, "tick-after-off") ? BLI_OPEN_ALWAYS : BLI_OPEN_UNTIL_CLICK, 0,
          !brk_notick, 0);
    render(&r, 1.0);
    s1 = say(&r);
    render(&r, 14.0);
    loads(&r, s1, s1 + 14.0, &f1, &l1, &off1);
    ratio = tick_ratio(&r, l1 + 1.2, l1 + 9.2, &dip);
    after = band_db(&r, off1 + 0.5, off1 + 3.5);
    snprintf(d, sizeof d, "folded at 99.98 ms: %.1f x the matches elsewhere; after the click-off %.1f dB", ratio, after);
    check("the 10 Hz tick while open, none after", off1 > 0 && ratio > 20.0 && after < -200.0, d);
    finish(&r);

    /* volumes 1 and 15: the same hiss */
    start(&r, 1, BLI_SOUND_BY_VOLUME, BLI_OPEN_UNTIL_CLICK, 1, 1, 0);
    render(&r, 1.0);
    s1 = say(&r);
    render(&r, 11.0);
    loads(&r, s1, s1 + 11.0, &f1, &l1, &off1);
    hiss1 = band_db(&r, l1 + 1.2, l1 + 8.2);
    finish(&r);
    start(&r, 15, BLI_SOUND_BY_VOLUME, BLI_OPEN_UNTIL_CLICK, 1, 1, 0);
    render(&r, 1.0);
    s1 = say(&r);
    render(&r, 11.0);
    loads(&r, s1, s1 + 11.0, &f1, &l1, &off1);
    hiss15 = band_db(&r, l1 + 1.2, l1 + 8.2);
    finish(&r);
    snprintf(d, sizeof d, "%.1f dB at volume 1, %.1f dB at volume 15 (the unit: 0.0 dB apart)", hiss1, hiss15);
    check("hiss the same at volume 1 and 15", fabs(hiss1 - hiss15) < 1.5, d);

    /* always: still heard past the click-off */
    start(&r, 6, BLI_SOUND_BY_VOLUME, brk_until ? BLI_OPEN_UNTIL_CLICK : BLI_OPEN_ALWAYS, 1, 1, 0);
    render(&r, 1.0);
    s1 = say(&r);
    render(&r, 15.5);
    loads(&r, s1, s1 + 15.5, &f1, &l1, &off1);
    open = band_db(&r, l1 + 1.2, l1 + 8.2);
    after = band_db(&r, l1 + 12.0, l1 + 14.0);
    snprintf(d, sizeof d, "%.1f dB 12-14 s after the last phoneme (clicked off at %.1f s), %.1f dB at 1-8 s", after,
             off1 - l1, open);
    check("always: open 12 s after speech", off1 > 0 && fabs(after - open) < 2.0, d);
    finish(&r);

    /* off: nothing after speech */
    start(&r, 6, BLI_SOUND_BY_VOLUME, brk_until ? BLI_OPEN_UNTIL_CLICK : BLI_OPEN_OFF, 1, 1, 0);
    render(&r, 1.0);
    s1 = say(&r);
    render(&r, 9.5);
    loads(&r, s1, s1 + 9.5, &f1, &l1, &off1);
    after = band_db(&r, l1 + 1.2, l1 + 8.2);
    snprintf(d, sizeof d, "%.1f dB 1-8 s after the last phoneme (open: %.1f)", after, hiss6);
    check("off: silent after speech", after < hiss6 - 30.0, d);
    finish(&r);

    if (failures)
        printf("idle sounds: %d FAILED\n", failures);
    else
        printf("idle sounds: all passed\n");
    return failures ? 1 : 0;
}
