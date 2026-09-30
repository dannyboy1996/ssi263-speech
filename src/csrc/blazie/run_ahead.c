/* run_ahead.c -- see run_ahead.h.  Included by bl_host.c (one translation unit, so no build list changes), and by
 * test_run_ahead.c (the contract's tests, a synthetic board and chip, no firmware). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "run_ahead.h"

#ifndef RA_REALLOC
#define RA_REALLOC realloc         /* test_run_ahead.c makes chosen allocations fail */
#endif
#define RA_SLICE 32                /* CPU cycles per capture step: an acknowledgement lands within ~5 us of its time */

void ra_init(run_ahead *r, const ra_board *b)
{
    memset(r, 0, sizeof *r);
    r->b = *b;
    r->lat = (unsigned long long)(RA_LAT_S * b->clock_hz);
    r->last_spoken = r->last_load = -1;
}

void ra_free(run_ahead *r)
{
    free(r->w);
    free(r->ack);
    free(r->ack_pa);
    r->w = NULL;
    r->ack = NULL;
    r->ack_pa = NULL;
    r->nw = r->capw = r->nack = r->capack = 0;
}

/* the capture cannot keep what the unit did: it ends here, as an error (never a shorter script taken as whole) */
static void fail(run_ahead *r)
{
    r->capturing = 0;
    r->end = RA_END_ALLOC;
}

static int push_ack(run_ahead *r, unsigned long long cyc, int pa)
{
    if (r->nack == r->capack) {
        int cap = r->capack ? r->capack * 2 : 256;
        unsigned long long *a = (unsigned long long *)RA_REALLOC(r->ack, (size_t)cap * sizeof *a);
        unsigned char *p;
        if (a)
            r->ack = a;
        p = a ? (unsigned char *)RA_REALLOC(r->ack_pa, (size_t)cap) : NULL;
        if (p)
            r->ack_pa = p;
        if (!a || !p)
            return 0;
        r->capack = cap;
    }
    r->ack[r->nack] = cyc;
    r->ack_pa[r->nack] = (unsigned char)pa;
    r->nack++;
    return 1;
}

int ra_start(run_ahead *r, int r3, int requesting)
{
    if (r->active)
        return 0;                                          /* the host completes, cancels or aborts it first */
    r->nw = r->nack = r->ri = r->seg = r->seg_ready = r->loaded = r->over = 0;
    r->last_spoken = -1;
    r->shift = 0;
    r->active = r->capturing = 1;
    r->end = RA_END_NONE;
    r->outcome = 0;
    r->r3 = r->r3_play = r3;
    r->last_load = -1;
    r->final_at = -1.0;
    r->last_write = r->b.cycles(r->b.ctx);
    r->pending = !requesting;                              /* a chip still busy: acknowledged at once */
    r->pend_pa = 1;                                        /* ... as the utterance's start: not its own speech */
    r->pend_at = r->last_write;
    if (!push_ack(r, r->last_write, 1))                    /* ack[0]: the start */
        fail(r);
    r->b.set_ar(r->b.ctx, requesting);
    return 1;
}

void ra_write_reg(run_ahead *r, int reg, int val)
{
    unsigned long long now = r->b.cycles(r->b.ctx);
    if (!r->capturing)
        return;                                            /* the host routes writes here only while capturing, or
                                                              after a failure (end = RA_END_ALLOC): lost, not hidden */
    if (r->nw == r->capw) {
        int cap = r->capw ? r->capw * 2 : 1024;
        ra_write *w = (ra_write *)RA_REALLOC(r->w, (size_t)cap * sizeof *w);
        if (!w) {
            if (r->brk != RA_BRK_ALLOC)
                fail(r);
            return;
        }
        r->w = w;
        r->capw = cap;
    }
    r->w[r->nw].cyc = now;
    r->w[r->nw].seg = r->nack - 1;
    r->w[r->nw].reg = (unsigned char)reg;
    r->w[r->nw].val = (unsigned char)val;
    r->nw++;
    r->last_write = now;
    if (reg == 3)
        r->r3 = val;
    if (reg == 0 && !(r->r3 & 0x80)) {                     /* a phoneme load: the chip drops A/R; acknowledged soon */
        if (val & 0x3F)
            r->last_spoken = r->nw - 1;
        r->pending = 1;
        r->pend_pa = (val & 0x3F) == 0;
        r->pend_at = now + (unsigned long long)(RA_ACK_S * r->b.clock_hz);
    }
}

/* runs the capture until write `need` exists and RA_AHEAD segments lie ahead of the replay, or the capture ends */
static void capture(run_ahead *r, int need)
{
    const unsigned long long quiet = (unsigned long long)(RA_QUIET_S * r->b.clock_hz);
    const unsigned long long limit = (unsigned long long)(RA_LIMIT_S * r->b.clock_hz);
    while (r->capturing && (r->nw <= need || r->nack - 1 < r->seg + RA_AHEAD)) {
        unsigned long long now;
        r->b.run(r->b.ctx, RA_SLICE);
        if (!r->capturing)
            break;                                         /* a write it could not keep */
        now = r->b.cycles(r->b.ctx);
        if (r->pending && now >= r->pend_at) {
            r->pending = 0;
            if (!push_ack(r, now, r->pend_pa) && r->brk != RA_BRK_ALLOC) {
                fail(r);
                break;
            }
            r->b.set_ar(r->b.ctx, 1);
        }
        if (!r->pending && now - r->last_write >= quiet) {
            if (!r->b.more(r->b.ctx)) {
                r->capturing = 0;                          /* exhausted -- by the board's policy, not a proof */
                r->end = RA_END_QUIET;
            } else if (now - r->last_write >= limit) {
                r->capturing = 0;                          /* speech still owed: a limit, not an end */
                r->end = r->brk == RA_BRK_LIMIT ? RA_END_QUIET : RA_END_LIMIT;
            }
        }
    }
}

/* the utterance ends (ra.active = 0) as `outcome` */
static void finish(run_ahead *r, int outcome)
{
    r->active = r->capturing = 0;
    r->outcome = outcome;
}

static void apply(run_ahead *r, const ra_write *w)
{
    if (w->reg == 3)
        r->r3_play = w->val;
    if (w->reg == 0 && !(r->r3_play & 0x80)) {
        r->last_load = w->val & 0x3F;
        r->loaded = 1;
    }
    r->ri++;
    r->b.apply(r->b.ctx, w->reg, w->val);
}

long ra_play(run_ahead *r, ssi263 *chip, double out_rate, double seconds, double *out, double *played)
{
    const double clk = r->b.clock_hz;
    const unsigned long long read = (unsigned long long)(RA_READ_S * clk);
    double t0 = ssi263_time(chip), t_end = t0 + seconds;
    long n = 0;
    while (r->active) {
        double now = ssi263_time(chip), due;
        const ra_write *w;
        long k;
        if (now >= t_end - 1e-12)
            break;
        /* the speech is over once a pause after the last spoken load has ended (the chip requests), with only idle
           writes left; it stays over while those play */
        if (!r->over && !r->capturing && r->end == RA_END_QUIET && r->ri > r->last_spoken && r->last_load == 0
                && ssi263_request(chip) && ra_rest_is_idle(r))
            r->over = 1;
        if (r->stop_trailing && r->over)
            break;                                         /* the host has input waiting */
        capture(r, r->ri);
        if (r->end == RA_END_ALLOC) {                      /* an explicit error: nothing after it is played */
            finish(r, RA_ERROR);
            break;
        }
        if (r->ri >= r->nw) {                              /* the capture has ended and all of it is played */
            if (r->brk == RA_BRK_COMPLETION) {             /* the 0.7 draft: exhausted = finished */
                finish(r, RA_COMPLETE);
                break;
            }
            if (r->final_at < 0.0)
                r->final_at = now;
            if (ssi263_request(chip)) {                    /* chip completion: the final load has ended */
                finish(r, r->end == RA_END_QUIET ? RA_COMPLETE : RA_LIMIT);
                break;
            }
            if (now - r->final_at >= RA_FINAL_S - 1e-12) { /* it never ended: a limit */
                finish(r, RA_LIMIT);
                break;
            }
            {
                double stop = r->final_at + RA_FINAL_S < t_end ? r->final_at + RA_FINAL_S : t_end;
                k = (long)ceil((stop - now) * out_rate);
                n += ssi263_run_until_request(chip, k > 0 ? k : 1, out + n);
            }
            continue;
        }
        w = &r->w[r->ri];
        if (r->pace) {                                     /* lane 1: the given schedule */
            long room = (long)ceil((t_end - now) * out_rate);
            if (r->ri >= r->npace) {                       /* no time given for this write: it waits */
                n += ssi263_run(chip, room > 0 ? room : 1, out + n);
                break;
            }
            r->seg = w->seg;
            due = r->pace[r->ri];
            k = (long)floor((due - now) * out_rate + 0.5);
            if (k > 0) {
                n += ssi263_run(chip, k < room ? k : (room > 0 ? room : 1), out + n);
                continue;
            }
            apply(r, w);
            continue;
        }
        if (!r->seg_ready || w->seg > r->seg) {            /* w opens its segment */
            unsigned long long first = w->cyc - r->ack[w->seg];
            /* reading: a late first answer where the unit reads -- at the utterance's start or after a pause (the
               corpora: every such answer); a slow answer after a spoken phoneme is kept as it is */
            int reading = first > read && (r->ack_pa[w->seg] || r->brk == RA_BRK_READING);
            int nowait = reading && r->last_load == 0;
            if (r->seg_ready && !nowait && !ssi263_request(chip)) {
                k = (long)ceil((t_end - now) * out_rate);
                n += ssi263_run_until_request(chip, k > 0 ? k : 1, out + n);
                continue;                                  /* at the request (or at the end of this call) */
            }
            r->seg = w->seg;
            r->seg_ready = 1;
            r->anchor = ssi263_time(chip);
            r->shift = reading ? first - r->lat : 0;
            if (!reading)
                r->lat = first;
            if (r->b.opened)
                r->b.opened(r->b.ctx, r->seg, nowait ? 2 : reading ? 1 : 0);
        }
        due = r->anchor + (double)(w->cyc - r->ack[r->seg] - r->shift) / clk;
        if (due >= t_end) {
            k = (long)ceil((t_end - now) * out_rate);
            n += ssi263_run(chip, k > 0 ? k : 1, out + n);
            break;
        }
        if (due > now) {
            k = (long)floor((due - now) * out_rate);
            if (k > 0) {
                n += ssi263_run(chip, k, out + n);
                continue;
            }
        }
        apply(r, w);
    }
    *played = ssi263_time(chip) - t0;
    return n;
}

int ra_state(const run_ahead *r, const ssi263 *chip)
{
    (void)chip;
    if (r->outcome)
        return r->outcome;
    if (!r->active)
        return RA_IDLE;
    if (r->end == RA_END_ALLOC)
        return RA_ERROR;
    if (r->capturing)
        return RA_CAPTURING;
    if (ra_trailing(r, chip))
        return RA_TRAILING;
    return r->ri < r->nw ? RA_REPLAYING : RA_FINAL_LOAD;
}

int ra_trailing(const run_ahead *r, const ssi263 *chip)
{
    (void)chip;
    return r->active && r->over && ra_rest_is_idle(r);
}

int ra_sounding(const run_ahead *r, const ssi263 *chip)
{
    return r->active && (r->capturing || r->ri <= r->last_spoken || (r->last_load > 0 && !ssi263_request(chip)));
}

int ra_rest_is_idle(const run_ahead *r)
{
    int i, r3 = r->r3_play;
    if (r->capturing)
        return 0;
    for (i = r->ri; i < r->nw; i++) {
        const ra_write *w = &r->w[i];
        if (w->reg == 3)
            r3 = w->val;
        if (w->reg == 0 && !(r3 & 0x80) && (w->val & 0x3F))
            return 0;                                      /* a spoken load still to come */
    }
    return 1;
}

int ra_flush(run_ahead *r)
{
    if (!r->active)
        return 1;
    if (!ra_rest_is_idle(r) && r->brk != RA_BRK_COMPLETION)
        return 0;
    if (r->capturing)                                      /* the 0.7 draft's flush only: the capture stopped */
        r->end = RA_END_ABORT;
    while (r->ri < r->nw)
        apply(r, &r->w[r->ri]);
    finish(r, r->end == RA_END_QUIET ? RA_COMPLETE : r->end == RA_END_LIMIT ? RA_LIMIT
              : r->end == RA_END_ALLOC ? RA_ERROR : RA_CANCELLED);
    return 1;
}

void ra_abort(run_ahead *r)
{
    if (!r->active)
        return;
    if (r->capturing)
        r->end = RA_END_ABORT;
    finish(r, RA_CANCELLED);
}

void ra_pace(run_ahead *r, const double *t, int n)
{
    r->pace = t;
    r->npace = t ? n : 0;
}
