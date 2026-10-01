/* bl_idle.c -- the Braille Lite's idle channel as Tomi's unit sounds (see bl_idle.h; the numbers: bl_idle_table.h,
 * generated from src/hosts/blazie_idle.py).  Every build of the board links it: its own object beside the board on
 * MAME's Z180 (bl.dll, the Linux library, the emulator app), inside bl_unity.c on the z180emu reference.
 *
 * Per sample of a block: the block's events (chip writes, channel power) at their samples, then
 *   gate   how much of the channel is heard (0..1, 2 ms ramps; 30 ms after speech with BLI_OPEN_OFF)
 *   noise  uniform noise through the two measured poles, at the measured density (the same at every volume)
 *   lines  one period of the comb at the tone register, band-limited under 0.45 x rate, phase-continuous
 *   tick   the measured template every BLI_TICK_PERIOD_S, phased by the last click-off (the timer writes it)
 *   pop, click   two decaying exponentials each (a step through two first-order high-passes; the pop's spike a third)
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "bl_idle.h"
#include "bl_idle_table.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Built with BLI_TEST_HOOKS (the emulator's test_idle only), bli_test_break puts a known bug back so its must-fail
   controls can show the test sees it: 1 the hiss follows the volume (scaled by R3's nibble / 6), 2 a pop at every
   line's first phoneme whether or not the channel was clicked off. */
#ifdef BLI_TEST_HOOKS
int bli_test_break;
#endif

#define BLI_MAX_EV 4096
#define BLI_TABLE 1024
#define BLI_HOLD_S 0.3             /* BLI_OPEN_OFF: the channel stays heard this long after the last phoneme */
#define BLI_RAMP_S 0.002           /* the gate's ramps */
#define BLI_RELEASE_S 0.03         /* BLI_OPEN_OFF's release after the hold */

typedef struct { int pos, reg, val; } bli_ev;     /* reg -1: the channel's power (val 0/1) */

struct bl_idle {
    bl_idle_options o;
    double rate;
    double now;                    /* samples rendered before this block */
    int power, r3, r4, amp;        /* the channel as the firmware left it; amp: R3's volume nibble when last open */
    int speaking;
    double quiet_since;            /* the sample the last phoneme ended at (a PA loaded after it) */
    bli_ev ev[BLI_MAX_EV];
    int n_ev;
    unsigned long seed;
    double n1, n2, g1, g2, nscale;
    int key_r4, key_cls, tab_ok;
    double inc, ph, tab[BLI_TABLE];
    double p1, p2, p3, d1, d2, d3; /* the pop's modes and their per-sample decays */
    double c1, c2, e1, e2;         /* the click's */
    double anchor;                 /* a sample on the tick phase (the last click-off) */
    double gate;
};

static int chan_open(const bl_idle *s)
{
    return s->power && !(s->r3 & 0x80) && (s->r3 & 0x70);
}

static int line_class(const bl_idle *s)            /* 0 hiss, 1 whine, -1 none */
{
    switch (s->o.sound) {
    case BLI_SOUND_HISS: return 0;
    case BLI_SOUND_WHINE: return 1;
    case BLI_SOUND_BY_VOLUME: return s->amp & 1;    /* the unit: even volumes hiss, odd volumes whine */
    default: return -1;
    }
}

/* one 64-tick period of the comb at tone register r4, as blazie.py's whine_wave, from the absolute table */
static void make_lines(bl_idle *s, int r4, int cls)
{
    const double xck = 1e6;
    int n_div = 256 - r4, tone, n, j, nl = 0, has[65], ln[64];
    double fc, lv[65], la[64], lp[64];
    unsigned long long seed = BLI_LINE_SEED;
    const char *p;
    s->key_r4 = r4;
    s->key_cls = cls;
    s->tab_ok = 0;
    if (n_div <= 0 || cls < 0)
        return;
    fc = xck / (2.0 * n_div);
    tone = 32 - n_div;
    if (tone > 26) tone = 26;
    if (tone < 0) tone = 0;
    memset(has, 0, sizeof has);
    for (p = BLI_LINES[cls][tone]; *p; ) {
        char *end;
        long k = strtol(p, &end, 10);
        if (end == p || *end != ':' || k < 1 || k > 64)
            break;
        lv[k] = strtod(end + 1, &end);
        has[k] = 1;
        p = end;
        while (*p == ' ') p++;
    }
    for (n = 1; n < 65; n++) {
        seed = (seed * 1103515245ULL + 12345ULL) & 0x7FFFFFFFULL;
        if (has[n] && n * fc / 64.0 < 0.45 * s->rate) {
            ln[nl] = n;
            la[nl] = sqrt(2.0) * pow(10.0, lv[n] / 20.0) * BLI_REF_RMS;
            lp[nl] = 2 * M_PI * (double)seed / 0x7FFFFFFF;
            nl++;
        }
    }
    for (j = 0; j < BLI_TABLE; j++) {
        double sum = 0.0;
        int i;
        for (i = 0; i < nl; i++)
            sum += la[i] * cos(2 * M_PI * ln[i] * j / BLI_TABLE + lp[i]);
        s->tab[j] = sum;
    }
    s->inc = fc / 64.0 / s->rate;
    s->tab_ok = nl > 0;
}

bl_idle *bl_idle_new(double rate, const bl_idle_options *o, int power, int r3, int r4)
{
    bl_idle *s = (bl_idle *)calloc(1, sizeof(bl_idle));
    double density;
    if (!s)
        return NULL;
    s->rate = rate;
    s->o = *o;
    s->power = power;
    s->r3 = r3;
    s->r4 = r4;
    s->amp = r3 & 0x0F;
    s->seed = 12345;
    s->g1 = 1.0 - exp(-2 * M_PI * BLI_NOISE_POLE1 / rate);
    s->g2 = 1.0 - exp(-2 * M_PI * BLI_NOISE_POLE2 / rate);
    density = pow(10.0, BLI_NOISE_DB / 10.0) * BLI_REF_RMS * BLI_REF_RMS;   /* per Hz, one-sided */
    s->nscale = sqrt(3.0 * density * rate / 2.0);   /* uniform noise of variance density x rate / 2 */
    s->d1 = exp(-1.0 / (BLI_POP[1] * rate));
    s->d2 = exp(-1.0 / (BLI_POP[2] * rate));
    s->d3 = exp(-1.0 / (BLI_POP[4] * rate));
    s->e1 = exp(-1.0 / (BLI_CLICK[1] * rate));
    s->e2 = exp(-1.0 / (BLI_CLICK[2] * rate));
    s->key_r4 = -1;
    s->gate = 0.0;
    s->quiet_since = -1e18;
    return s;
}

void bl_idle_free(bl_idle *s)
{
    free(s);
}

void bl_idle_set(bl_idle *s, const bl_idle_options *o)
{
    s->o = *o;
}

void bl_idle_begin(bl_idle *s)
{
    int i;
    for (i = 0; i < s->n_ev; i++)
        s->ev[i].pos = 0;
}

static void push(bl_idle *s, int pos, int reg, int val)
{
    if (s->n_ev < BLI_MAX_EV) {
        s->ev[s->n_ev].pos = pos;
        s->ev[s->n_ev].reg = reg;
        s->ev[s->n_ev].val = val;
        s->n_ev++;
    }
}

void bl_idle_write(bl_idle *s, int pos, int reg, int val)
{
    push(s, pos, reg, val);
}

void bl_idle_power(bl_idle *s, int pos, int on)
{
    push(s, pos, -1, on ? 1 : 0);
}

/* a step of amplitude a through two first-order high-passes (time constants t1 < t2): a (w1 e^-w1t - w2 e^-w2t) /
   (w1 - w2), as two modes */
static void step_modes(double a, double t1, double t2, double *m1, double *m2)
{
    double w1 = 1.0 / t1, w2 = 1.0 / t2;
    *m1 += a * w1 / (w1 - w2);
    *m2 += a * w2 / (w1 - w2);
}

static void apply(bl_idle *s, const bli_ev *e, double at)
{
    if (e->reg < 0) {
        if (e->val && !s->power) {                                   /* the channel opens: the pop */
            if (s->o.pop_click && s->o.keep_open == BLI_OPEN_UNTIL_CLICK) {
                step_modes(BLI_POP[0] * BLI_REF_RMS, BLI_POP[1], BLI_POP[2], &s->p1, &s->p2);
                s->p3 += BLI_POP[3] * BLI_REF_RMS;
            }
        } else if (!e->val && s->power) {                            /* clicked off: the click, and the tick's phase */
            if (s->o.pop_click && s->o.keep_open == BLI_OPEN_UNTIL_CLICK)
                step_modes(BLI_CLICK[0] * BLI_REF_RMS, BLI_CLICK[1], BLI_CLICK[2], &s->c1, &s->c2);
            s->anchor = at;
        }
        s->power = e->val;
        return;
    }
    if (e->reg == 3) {
        s->r3 = e->val;
        if (!(e->val & 0x80) && (e->val & 0x70))
            s->amp = e->val & 0x0F;
    } else if (e->reg == 4) {
        s->r4 = e->val;
    } else if (e->reg == 0 && !(s->r3 & 0x80)) {
#ifdef BLI_TEST_HOOKS
        if (bli_test_break == 2 && (e->val & 0x3F) && !s->speaking && at - s->quiet_since > 0.5 * s->rate)
            step_modes(BLI_POP[0] * BLI_REF_RMS, BLI_POP[1], BLI_POP[2], &s->p1, &s->p2);
#endif
        if (e->val & 0x3F)
            s->speaking = 1;
        else if (s->speaking) {
            s->speaking = 0;
            s->quiet_since = at;
        }
    }
}

void bl_idle_render(bl_idle *s, double *y, int n)
{
    int i, k = 0, cls;
    const double ramp = 1.0 / (BLI_RAMP_S * s->rate), release = 1.0 / (BLI_RELEASE_S * s->rate);
    const double per = BLI_TICK_PERIOD_S * s->rate, lead = BLI_TICK_LEAD_S * s->rate;
    const double tpos = (double)BLI_TICK_RATE / s->rate, fade0 = BLI_TICK_LEN - BLI_TICK_FADE_S * BLI_TICK_RATE;
    for (i = 0; i < n; i++) {
        double t = s->now + i, target, v = 0.0, ph;
        while (k < s->n_ev && s->ev[k].pos <= i) {
            apply(s, &s->ev[k], t);
            k++;
        }
        if (s->o.keep_open == BLI_OPEN_ALWAYS)
            target = 1.0;
        else if (s->o.keep_open == BLI_OPEN_UNTIL_CLICK)
            target = chan_open(s) ? 1.0 : 0.0;
        else
            target = (s->speaking || t - s->quiet_since < BLI_HOLD_S * s->rate) ? 1.0 : 0.0;
        if (s->gate < target)
            s->gate = s->gate + ramp < target ? s->gate + ramp : target;
        else if (s->gate > target) {
            double r = s->o.keep_open == BLI_OPEN_OFF ? release : ramp;
            s->gate = s->gate - r > target ? s->gate - r : target;
        }
        /* the noise and the lines run on whether heard or not, so the stream does not depend on when it was heard */
        s->seed = (s->seed * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
        s->n1 += s->g1 * (s->nscale * ((double)s->seed / 1073741824.0 - 1.0) - s->n1);
        s->n2 += s->g2 * (s->n1 - s->n2);
        cls = line_class(s);
        if (s->r4 != s->key_r4 || cls != s->key_cls)
            make_lines(s, s->r4, cls);
        if (s->gate > 0.0) {
            if (s->o.sound != BLI_SOUND_NONE) {
                v += s->n2;
                if (s->tab_ok) {
                    double pos = s->ph * BLI_TABLE, fr;
                    int j = (int)pos;
                    fr = pos - j;
                    v += s->tab[j % BLI_TABLE] * (1.0 - fr) + s->tab[(j + 1) % BLI_TABLE] * fr;
                }
#ifdef BLI_TEST_HOOKS
                if (bli_test_break == 1)
                    v *= s->amp / 6.0;
#endif
            }
            if (s->o.tick) {
                ph = fmod(t - s->anchor + lead, per);
                if (ph < 0)
                    ph += per;
                ph *= tpos;                                  /* template samples since this period's start */
                if (ph < BLI_TICK_LEN - 1) {
                    int j = (int)ph;
                    double fr = ph - j, w = BLI_TICK[j] * (1.0 - fr) + BLI_TICK[j + 1] * fr;
                    if (ph > fade0)
                        w *= 0.5 + 0.5 * cos(M_PI * (ph - fade0) / (BLI_TICK_LEN - 1 - fade0));
                    v += w * BLI_REF_RMS;
                }
            }
            v *= s->gate;
        }
        if (s->tab_ok) {
            s->ph += s->inc;
            if (s->ph >= 1.0)
                s->ph -= 1.0;
        }
        v += (s->p1 - s->p2 + s->p3) + (s->c1 - s->c2);
        s->p1 *= s->d1;
        s->p2 *= s->d2;
        s->p3 *= s->d3;
        s->c1 *= s->e1;
        s->c2 *= s->e2;
        y[i] += v;
    }
    for (; k < s->n_ev; k++)                                 /* none land after the block, but keep the state right */
        apply(s, &s->ev[k], s->now + n);
    s->n_ev = 0;
    s->now += n;
}
