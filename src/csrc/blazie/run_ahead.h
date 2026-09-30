/* run_ahead.h -- a unit run ahead of its chip ("drain"), for any board whose CPU drives an SSI-263 and answers its A/R
 * request (the Braille Lite's bl_board, the Type 'n Speak's tns_board, or another unit on a Z180 or anything else).
 * It knows the chip's side only: which writes load a phoneme, when the chip requests the next one.  Nothing of any
 * firmware.
 *
 * Capture: the CPU runs flat out.  The chip is played by an "instant" stand-in: every phoneme load is acknowledged
 * (A/R raised) RA_ACK_S of CPU time later, so the unit writes a whole utterance in a fraction of its speaking time.
 * Each write is kept with its CPU cycle and the number of acknowledgements before it (its segment).
 *
 * Replay: the real chip plays the script in real time.  Segment k starts at the chip's own k-th request, and each of
 * its writes follows at its CPU distance from the k-th acknowledgement: the unit's answer to a request, at its own
 * speed.  A segment the unit answered only after more than RA_READ_S (it was reading its next line or starting the
 * utterance) is moved up to RA_LAT (the unit's usual answer); if it follows a pause phoneme (code 00) it does not wait
 * for that pause to end either -- the lockstep host has had the next line ready by then (measured: every such joint
 * in the English and Spanish test corpora).
 *
 * The unit's register values are the same as the lockstep's (nvda/tools/run_ahead_equiv.py); the timing differs: the
 * answers come at the unit's own speed instead of the lockstep's 0.5 ms / 125 us slices, and the reading before an
 * utterance and between its lines is not heard (as the "short pauses" option does with its x4 CPU).
 */
#ifndef RUN_AHEAD_H
#define RUN_AHEAD_H

#include "../ssi263.h"

#define RA_ACK_S 0.001             /* the stand-in chip's acknowledgement, after a load (CPU time) */
#define RA_READ_S 0.005            /* an answer later than this: the unit was reading, not answering */
#define RA_LAT_S 58e-6             /* the unit's answer to a request until one is measured (the corpora's median) */
#define RA_QUIET_S 0.2             /* capture ends: nothing more owed and no write for this long (CPU time) */
#define RA_LIMIT_S 3.0             /* ... or no write at all for this long */
#define RA_AHEAD 16                /* segments captured ahead of the replay */

typedef struct {
    void *ctx;
    void (*run)(void *ctx, unsigned long long cycles);   /* runs the CPU; the host hands its writes to ra_write */
    unsigned long long (*cycles)(void *ctx);
    void (*set_ar)(void *ctx, int requesting);           /* the A/R line into the unit */
    int (*more)(void *ctx);                              /* the unit still owes speech for input it was given */
    void (*apply)(void *ctx, int reg, int val);          /* a write to the chip, now (the host's bookkeeping too) */
    double clock_hz;
} ra_board;

typedef struct {
    unsigned long long cyc;
    int seg;
    unsigned char reg, val;
} ra_write;

typedef struct {
    ra_board b;
    int active;                    /* an utterance is being captured or replayed */
    int capturing;
    int finished;                  /* the last one replayed to its end (cleared by ra_start / ra_abort) */
    ra_write *w;
    int nw, capw;
    unsigned long long *ack;       /* ack[k]: the CPU cycle of the k-th acknowledgement (ack[0]: the start) */
    int nack, capack;
    int r3, r3_play, pending;
    unsigned long long pend_at, last_write;
    int ri, seg, seg_ready, last_load;
    int last_spoken;               /* index of the last spoken load (code not 00) captured, -1: none */
    double anchor;
    unsigned long long shift, lat;
} run_ahead;

void ra_init(run_ahead *r, const ra_board *b);
void ra_free(run_ahead *r);
/* a new utterance from the current state: r3 = the chip's R3 now, requesting = its A/R request now */
void ra_start(run_ahead *r, int r3, int requesting);
/* the unit wrote a chip register during capture */
void ra_write_reg(run_ahead *r, int reg, int val);
/* plays up to `seconds` of the script on the chip into out (room for seconds x out_rate + 64 samples); returns the
   samples, and the chip time played in *played (less than `seconds` when the script ended: ra.active is then 0) */
long ra_play(run_ahead *r, ssi263 *chip, double out_rate, double seconds, double *out, double *played);
/* 1 while the utterance still has sound to come: the capture runs, a spoken phoneme is still to be loaded, or the
   last one loaded is still playing.  What follows the last spoken phoneme is pauses (code 00) and register writes:
   silence, so the utterance is over when the last spoken phoneme ends (the host's "done") */
int ra_sounding(const run_ahead *r, const ssi263 *chip);
/* the rest of the script -- only silence once ra_sounding is 0 -- applied to the chip at once, in order (a new
   utterance or a cancel after the end: the chip is left with every value the unit wrote) */
void ra_flush(run_ahead *r);
/* stops: the script not yet played is dropped (the unit stays where the capture left it) */
void ra_abort(run_ahead *r);

#endif
