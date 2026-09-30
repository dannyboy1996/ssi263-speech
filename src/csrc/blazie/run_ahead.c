/* run_ahead.c -- see run_ahead.h.  Included by bl_host.c (one translation unit, so no build list changes). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "run_ahead.h"

#define RA_SLICE 32                /* CPU cycles per capture step: an acknowledgement lands within ~5 us of its time */

void ra_init(run_ahead *r, const ra_board *b)
{
    memset(r, 0, sizeof *r);
    r->b = *b;
    r->lat = (unsigned long long)(RA_LAT_S * b->clock_hz);
}

void ra_free(run_ahead *r)
{
    free(r->w);
    free(r->ack);
    r->w = NULL;
    r->ack = NULL;
    r->nw = r->capw = r->nack = r->capack = 0;
}

static int push_ack(run_ahead *r, unsigned long long cyc)
{
    if (r->nack == r->capack) {
        int cap = r->capack ? r->capack * 2 : 256;
        unsigned long long *a = (unsigned long long *)realloc(r->ack, (size_t)cap * sizeof *a);
        if (!a)
            return 0;
        r->ack = a;
        r->capack = cap;
    }
    r->ack[r->nack++] = cyc;
    return 1;
}

void ra_start(run_ahead *r, int r3, int requesting)
{
    if (r->active)
        ra_flush(r);                                       /* the last utterance's trailing silence, applied now */
    r->nw = r->nack = r->ri = r->seg = r->seg_ready = 0;
    r->last_spoken = -1;
    r->shift = 0;
    r->active = r->capturing = 1;
    r->finished = 0;
    r->r3 = r->r3_play = r3;
    r->last_load = -1;
    r->last_write = r->b.cycles(r->b.ctx);
    push_ack(r, r->last_write);                            /* ack[0]: the start */
    r->pending = !requesting;                              /* a chip still busy: acknowledged at once */
    r->pend_at = r->last_write;
    r->b.set_ar(r->b.ctx, requesting);
}

void ra_write_reg(run_ahead *r, int reg, int val)
{
    unsigned long long now = r->b.cycles(r->b.ctx);
    if (!r->capturing)
        return;
    if (r->nw == r->capw) {
        int cap = r->capw ? r->capw * 2 : 1024;
        ra_write *w = (ra_write *)realloc(r->w, (size_t)cap * sizeof *w);
        if (!w)
            return;
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
        r->pend_at = now + (unsigned long long)(RA_ACK_S * r->b.clock_hz);
    }
}

/* runs the capture until write `need` exists and RA_AHEAD segments lie ahead of the replay, or the unit is done */
static void capture(run_ahead *r, int need)
{
    const unsigned long long quiet = (unsigned long long)(RA_QUIET_S * r->b.clock_hz);
    const unsigned long long limit = (unsigned long long)(RA_LIMIT_S * r->b.clock_hz);
    while (r->capturing && (r->nw <= need || r->nack - 1 < r->seg + RA_AHEAD)) {
        unsigned long long now;
        r->b.run(r->b.ctx, RA_SLICE);
        now = r->b.cycles(r->b.ctx);
        if (r->pending && now >= r->pend_at) {
            r->pending = 0;
            r->b.set_ar(r->b.ctx, 1);
            push_ack(r, now);
        }
        if (!r->pending && ((now - r->last_write >= quiet && !r->b.more(r->b.ctx)) || now - r->last_write >= limit))
            r->capturing = 0;
    }
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
        if (now >= t_end - 1e-12)
            break;
        capture(r, r->ri);
        if (r->ri >= r->nw) {                              /* the capture has ended and all of it is played */
            r->active = 0;
            r->finished = 1;
            break;
        }
        w = &r->w[r->ri];
        if (!r->seg_ready || w->seg > r->seg) {            /* w opens its segment */
            unsigned long long first = w->cyc - r->ack[w->seg];
            int reading = first > read;
            if (r->seg_ready && !(reading && r->last_load == 0) && !ssi263_request(chip)) {
                long k = (long)ceil((t_end - now) * out_rate);
                n += ssi263_run_until_request(chip, k > 0 ? k : 1, out + n);
                continue;                                  /* at the request (or at the end of this call) */
            }
            r->seg = w->seg;
            r->seg_ready = 1;
            r->anchor = ssi263_time(chip);
            r->shift = reading ? first - r->lat : 0;
            if (!reading)
                r->lat = first;
        }
        due = r->anchor + (double)(w->cyc - r->ack[r->seg] - r->shift) / clk;
        if (due >= t_end) {
            long k = (long)ceil((t_end - now) * out_rate);
            n += ssi263_run(chip, k > 0 ? k : 1, out + n);
            break;
        }
        if (due > now) {
            long k = (long)floor((due - now) * out_rate);
            if (k > 0) {
                n += ssi263_run(chip, k, out + n);
                continue;
            }
        }
        if (w->reg == 3)
            r->r3_play = w->val;
        if (w->reg == 0 && !(r->r3_play & 0x80))
            r->last_load = w->val & 0x3F;
        r->ri++;
        r->b.apply(r->b.ctx, w->reg, w->val);
    }
    *played = ssi263_time(chip) - t0;
    return n;
}

int ra_sounding(const run_ahead *r, const ssi263 *chip)
{
    return r->active && (r->capturing || r->ri <= r->last_spoken || (r->last_load > 0 && !ssi263_request(chip)));
}

void ra_flush(run_ahead *r)
{
    while (r->ri < r->nw) {
        const ra_write *w = &r->w[r->ri++];
        if (w->reg == 3)
            r->r3_play = w->val;
        if (w->reg == 0 && !(r->r3_play & 0x80))
            r->last_load = w->val & 0x3F;
        r->b.apply(r->b.ctx, w->reg, w->val);
    }
    r->active = r->capturing = 0;
    r->finished = 1;
}

void ra_abort(run_ahead *r)
{
    r->active = r->capturing = r->finished = 0;
    r->nw = r->nack = r->ri = 0;
}
