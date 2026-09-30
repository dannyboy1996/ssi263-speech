/* test_run_ahead.c -- run_ahead.h's contract (Astra, Reply 107), on a synthetic board and chip: no firmware, no claim
 * about any real unit.  Built by build_board.py as test_run_ahead.exe; nvda/tools/run_tests.py runs it, and its
 * controls:
 *
 *   test_run_ahead.exe                   every case must pass
 *   test_run_ahead.exe old-completion    RA_BRK_COMPLETION: playback exhausted = finished (must fail)
 *   test_run_ahead.exe old-limit         RA_BRK_LIMIT: the capture limit ends as a success (must fail)
 *   test_run_ahead.exe old-alloc         RA_BRK_ALLOC: failed allocations ignored (must fail)
 *   test_run_ahead.exe old-reading       RA_BRK_READING: every answer over 5 ms moved up (must fail)
 *   test_run_ahead.exe old-settle        RA_BRK_SETTLE: a cancel meets the unit where the capture stopped it (must fail)
 *
 * The chip: a phoneme load drops A/R for its duration (spoken 50 ms, pause 30 ms); a spoken phoneme sounds on (0.5)
 * after its duration until the next load, as the SSI-263 holds it; a pause is silence.  The board: a unit that loads
 * its phonemes one per request, each a given CPU time after A/R rises, optionally with register writes before it.
 * The first two cases are Astra's run_ahead_probe.c fixtures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t fail_size;                   /* an allocation of exactly this many bytes fails (0: none) */
static void *test_realloc(void *p, size_t n)
{
    return n == fail_size ? NULL : realloc(p, n);
}
#define RA_REALLOC test_realloc
#include "run_ahead.c"

#define CLK 6144000.0
#define RATE 44100.0
#define MS(x) ((unsigned long long)((x) * CLK / 1000.0))

struct ssi263 {
    double time, until;
    int request, code, never;
    double req_at[4096];                   /* when A/R rose, in order */
    int n_req;
};

double ssi263_time(const ssi263 *c) { return c->time; }
int ssi263_request(const ssi263 *c) { return c->request; }

static void tick(ssi263 *c, double *out)
{
    *out = c->code ? 0.5 : 0.0;
    c->time += 1.0 / RATE;
    if (!c->request && !c->never && c->time >= c->until - 1e-12) {
        c->request = 1;
        if (c->n_req < 4096)
            c->req_at[c->n_req++] = c->time;
    }
}

long ssi263_run(ssi263 *c, long n, double *out)
{
    long i;
    for (i = 0; i < n; i++)
        tick(c, out + i);
    return n;
}

long ssi263_run_until_request(ssi263 *c, long n, double *out)
{
    long i;
    for (i = 0; i < n && !c->request; i++)
        tick(c, out + i);
    return i;
}

typedef struct {
    double lat_ms;                         /* after A/R rises (or after the last write, if A/R was up already) */
    int code;                              /* the phoneme loaded (0: a pause) */
    int regs;                              /* register writes (R1, R2, R4) just before the load */
} step;

typedef struct {
    run_ahead r;
    ssi263 chip;
    unsigned long long cyc, due;
    int ar, waiting, next, n, owe_always, applied, phase;
    const step *steps;
    double applied_at[4096];
    int applied_code[4096], n_loads;
} board;

static void arm(board *b)
{
    if (b->ar && !b->waiting && b->next < b->n) {
        b->waiting = 1;
        b->phase = 0;
        b->due = b->cyc + MS(b->steps[b->next].lat_ms);
    }
}

static void run(void *ctx, unsigned long long n)
{
    board *b = (board *)ctx;
    b->cyc += n;
    if (b->waiting && b->cyc >= b->due) {
        const step *s = &b->steps[b->next];
        while (b->phase < s->regs) {       /* 3 cycles apart: all in this slice */
            ra_write_reg(&b->r, 1 + (b->phase == 2 ? 3 : b->phase), 0x40 + b->phase);
            b->phase++;
        }
        b->waiting = 0;
        b->next++;
        b->ar = 0;                         /* the load drops A/R (the stand-in raises it again) */
        ra_write_reg(&b->r, 0, s->code);
    }
}

static unsigned long long cycles(void *ctx) { return ((board *)ctx)->cyc; }
static int idle_cb(void *ctx) { return !((board *)ctx)->waiting; }   /* case 11: awake while it works to a load */
static void set_ar(void *ctx, int v) { board *b = (board *)ctx; b->ar = v; arm(b); }
static int more(void *ctx) { board *b = (board *)ctx; return b->owe_always || b->next < b->n; }

static void apply_w(void *ctx, int reg, int val)
{
    board *b = (board *)ctx;
    b->applied++;
    if (reg == 0) {
        b->chip.code = val & 0x3F;
        b->chip.request = 0;
        b->chip.until = b->chip.time + (b->chip.code ? 0.050 : 0.030);
        if (b->n_loads < 4096) {
            b->applied_at[b->n_loads] = b->chip.time;
            b->applied_code[b->n_loads++] = b->chip.code;
        }
    }
}

static int brk, failures, cases;
static double buf[44100 * 12];

static void setup(board *b, const step *steps, int n)
{
    ra_board iface;
    memset(b, 0, sizeof *b);
    b->steps = steps;
    b->n = n;
    b->chip.request = 1;
    iface.ctx = b;
    iface.run = run;
    iface.cycles = cycles;
    iface.set_ar = set_ar;
    iface.more = more;
    iface.apply = apply_w;
    iface.opened = NULL;
    iface.idle = NULL;
    iface.clock_hz = CLK;
    ra_init(&b->r, &iface);
    b->r.brk = brk;
    ra_start(&b->r, 0x00, 1);
}

/* plays in 30 ms calls until the utterance ends or `limit_s`; the samples above 0 after its end in *after */
static long play(board *b, double limit_s, long *loud)
{
    long total = 0, n, i;
    double played;
    *loud = 0;
    while (b->r.active && b->chip.time < limit_s) {
        n = ra_play(&b->r, &b->chip, RATE, 0.03, buf, &played);
        for (i = 0; i < n; i++)
            *loud += buf[i] != 0.0;
        total += n;
    }
    return total;
}

static void report(int ok, const char *name, const char *fmt, double a, double b2, double c)
{
    char msg[256];
    cases++;
    failures += !ok;
    snprintf(msg, sizeof msg, fmt, a, b2, c);
    printf("%-4s %s: %s\n", ok ? "ok" : "FAIL", name, msg);
}

static const char *names[] = {"idle", "capturing", "replaying", "trailing", "final load", "complete", "limit", "error",
                              "cancelled"};

int main(int argc, char **argv)
{
    board *b = (board *)calloc(1, sizeof *b);
    long loud, n;
    int st, i;
    if (argc > 1)
        brk = !strcmp(argv[1], "old-completion") ? RA_BRK_COMPLETION : !strcmp(argv[1], "old-limit") ? RA_BRK_LIMIT
            : !strcmp(argv[1], "old-alloc") ? RA_BRK_ALLOC : !strcmp(argv[1], "old-reading") ? RA_BRK_READING
            : !strcmp(argv[1], "old-settle") ? RA_BRK_SETTLE : -1;
    if (brk < 0 || !b) {
        fprintf(stderr, "usage: test_run_ahead [old-completion|old-limit|old-alloc|old-reading|old-settle]\n");
        return 2;
    }

    {   /* 1. Astra's fixture: one spoken load, no cleanup write.  Complete only once the chip requests again */
        static const step s[] = {{32.0 / 6144.0, 5, 0}};
        int seen_final = 0, early = 0;
        long samples = 0;
        double played;
        setup(b, s, 1);
        while (b->r.active && b->chip.time < 2.0) {
            samples += ra_play(&b->r, &b->chip, RATE, 0.01, buf, &played);
            st = ra_state(&b->r, &b->chip);
            seen_final |= st == RA_FINAL_LOAD;
            early |= (st == RA_COMPLETE || !b->r.active) && !b->chip.request;
        }
        st = ra_state(&b->r, &b->chip);
        report(st == RA_COMPLETE && seen_final && !early && b->chip.request && samples > 0,
               "last load, no cleanup", "final load seen %.0f, ended while the chip still sounded %.0f, "
               "%.0f samples played", seen_final, early, (double)samples);
        if (st != RA_COMPLETE || early)
            printf("     ... ended as %s with the chip %s\n", names[st], b->chip.request ? "requesting" : "SOUNDING");
        ra_free(&b->r);
    }
    {   /* 2. Astra's fixture: speech still owed, its first load 3.1 s of CPU in.  A limit, never a success */
        static const step s[] = {{3100.0, 5, 0}};
        setup(b, s, 1);
        b->owe_always = 1;
        play(b, 10.0, &loud);
        st = ra_state(&b->r, &b->chip);
        report(st == RA_LIMIT && b->r.end == RA_END_LIMIT && more(b), "owed, first load at 3.1 s",
               "ended as state %.0f (limit = 6), capture end %.0f (limit = 2), still owed %.0f", st, b->r.end,
               more(b));
        ra_free(&b->r);
    }
    {   /* 3. a final load that never ends (the chip never requests): a limit at RA_FINAL_S, not complete, not stuck */
        static const step s[] = {{0.1, 5, 0}, {0.1, 0, 0}};
        setup(b, s, 2);
        b->chip.never = 0;
        while (b->r.active && b->n_loads < 2 && b->chip.time < 2.0) {
            double played;
            ra_play(&b->r, &b->chip, RATE, 0.005, buf, &played);
        }
        b->chip.never = 1;                 /* from the final load on */
        b->chip.request = 0;
        play(b, 5.0, &loud);
        st = ra_state(&b->r, &b->chip);
        report(st == RA_LIMIT && !b->r.active, "final load never ends", "ended as state %.0f (limit = 6) at %.3f s",
               st, b->chip.time, 0);
        ra_free(&b->r);
    }
    {   /* 4. a normal utterance: 3 spoken loads and a cleanup pause.  The speech is over after the pause (trailing
           or complete), never while a spoken phoneme sounds; after the end, silence */
        static const step s[] = {{0.06, 5, 3}, {0.06, 7, 3}, {0.06, 9, 3}, {0.06, 0, 3}};
        int early = 0;
        double played;
        setup(b, s, 4);
        while (b->r.active && b->chip.time < 3.0) {
            ra_play(&b->r, &b->chip, RATE, 0.002, buf, &played);
            st = ra_state(&b->r, &b->chip);
            if ((st == RA_TRAILING || st == RA_COMPLETE) && b->chip.code)
                early = 1;
        }
        st = ra_state(&b->r, &b->chip);
        n = ssi263_run(&b->chip, 4410, buf);            /* 0.1 s after the end */
        for (loud = 0, i = 0; i < (int)n; i++)
            loud += buf[i] != 0.0;
        report(st == RA_COMPLETE && !early && b->applied == 16 && !loud, "a normal utterance",
               "complete %.0f, done while a spoken phoneme sounded %.0f, %.0f writes applied", st == RA_COMPLETE,
               early, b->applied);
        ra_free(&b->r);
    }
    {   /* 5. an allocation fails for the acknowledgements (the 257th): an error, never a shortened script played */
        static step s[300];
        for (i = 0; i < 300; i++) {
            s[i].lat_ms = 0.06;
            s[i].code = i & 1 ? 0 : 5;
        }
        setup(b, s, 300);
        fail_size = 512 * sizeof(unsigned long long);
        play(b, 30.0, &loud);
        fail_size = 0;
        st = ra_state(&b->r, &b->chip);
        report(st == RA_ERROR && b->n_loads < 300, "allocation fails (acknowledgements)",
               "ended as state %.0f (error = 7) after %.0f of 300 loads", st, b->n_loads, 0);
        ra_free(&b->r);
    }
    {   /* 6. ... and for the writes (the 1025th) */
        static step s[210];
        for (i = 0; i < 210; i++) {
            s[i].lat_ms = 0.06;
            s[i].code = i & 1 ? 0 : 5;
            s[i].regs = 4;
        }
        setup(b, s, 210);
        fail_size = 2048 * sizeof(ra_write);
        play(b, 30.0, &loud);
        fail_size = 0;
        st = ra_state(&b->r, &b->chip);
        report(st == RA_ERROR && b->applied < 1024, "allocation fails (writes)",
               "ended as state %.0f (error = 7) after %.0f of 1050 writes", st, b->applied, 0);
        ra_free(&b->r);
    }
    {   /* 7. the 5 ms threshold: answers of 4.9, 5.0 and 5.1 ms after spoken phonemes are kept (answers, however slow);
           after a pause, 4.9 is kept and 5.1 moved up (the unit reading its next line: a classification) */
        static const step s[] = {{0.06, 5, 0}, {4.9, 7, 0}, {0.06, 9, 0}, {5.0, 11, 0}, {0.06, 13, 0}, {5.1, 15, 0},
                                 {0.06, 0, 0}, {4.9, 17, 0}, {0.06, 0, 0}, {5.1, 19, 0}, {0.06, 0, 0}};
        static const double want[] = {-1, 4.9, 0.06, 5.0, 0.06, 5.1, 0.06, 4.9, 0.06, -2, 0.06};
        double worst = 0.0, got = 0.0;
        int bad = -1;
        setup(b, s, 11);
        play(b, 5.0, &loud);
        for (i = 1; i < b->n_loads && i < 11; i++) {
            double prev_end = b->applied_at[i - 1] + (b->applied_code[i - 1] ? 0.050 : 0.030);
            double lat = (b->applied_at[i] - prev_end) * 1e3;
            double err = want[i] == -2 ? (lat < -20.0 ? 0.0 : 1.0) : lat - want[i];
            if (err < 0)
                err = -err;
            if (err > 0.1 && bad < 0) {
                bad = i;
                got = lat;
            }
            if (err > worst)
                worst = err;
        }
        report(bad < 0 && b->n_loads == 11, "5 ms threshold (answers after spoken phonemes and after pauses)",
               "worst error %.3f ms; first wrong: load %.0f answered %.3f ms after the request", worst, bad, got);
        ra_free(&b->r);
    }
    {   /* 8. a slow-service unit: every answer 50 ms after the request, all in speech.  Kept, not "reading" */
        static step s[12];
        double worst = 0.0;
        for (i = 0; i < 12; i++) {
            s[i].lat_ms = i ? 50.0 : 0.06;
            s[i].code = i == 11 ? 0 : 5 + i;
        }
        setup(b, s, 12);
        play(b, 5.0, &loud);
        for (i = 1; i < b->n_loads; i++) {
            double lat = (b->applied_at[i] - b->applied_at[i - 1] - 0.050) * 1e3 - 50.0;
            if (lat < 0)
                lat = -lat;
            if (lat > worst)
                worst = lat;
        }
        report(worst < 0.1 && b->n_loads == 12, "slow service (50 ms answers in speech)",
               "worst answer off by %.3f ms over %.0f loads (of %.0f)", worst, b->n_loads, 12);
        ra_free(&b->r);
    }
    {   /* 9. no second utterance over a running one; no flush over speech still to come */
        static const step s[] = {{0.06, 5, 0}, {0.06, 7, 0}, {0.06, 0, 0}};
        double played;
        int refused, flush_refused;
        setup(b, s, 3);
        ra_play(&b->r, &b->chip, RATE, 0.01, buf, &played);
        refused = !ra_start(&b->r, 0, 1);
        flush_refused = !ra_flush(&b->r);
        report(refused && flush_refused && b->r.active, "start or flush over speech",
               "start refused %.0f, flush refused %.0f, still active %.0f", refused, flush_refused, b->r.active);
        ra_free(&b->r);
    }
    {   /* 10. lane 1: the script at a given schedule, to the sample */
        static const step s[] = {{0.06, 5, 2}, {0.06, 7, 0}, {0.06, 0, 0}};
        static const double t[] = {0.010, 0.0101, 0.0102, 0.100, 0.200};
        double worst = 0.0;
        setup(b, s, 3);
        ra_pace(&b->r, t, 5);
        play(b, 3.0, &loud);
        /* the loads are writes 2, 3 and 4 */
        for (i = 0; i < 3 && i < b->n_loads; i++) {
            double e = (b->applied_at[i] - t[i + 2]) * RATE;
            if (e < 0)
                e = -e;
            if (e > worst)
                worst = e;
        }
        report(worst < 0.5 && b->n_loads == 3 && ra_state(&b->r, &b->chip) == RA_COMPLETE, "lane 1: paced schedule",
               "worst %.2f samples off over %.0f loads (%.0f)", worst, b->n_loads, 3);
        ra_free(&b->r);
    }
    {   /* 11. a cancel while the capture runs (ra_settle): the capture stops the unit just after an acknowledgement,
           which wakes it -- mid-routine (here: 2 ms of work before each load).  Before the cancel reaches it, the unit
           runs on, nothing more acknowledged, until it waits for an interrupt; what it writes meanwhile is the
           dropped script's, never applied.  Its control (old-settle) cancels it where the capture left it */
        static step s[40];
        double played;
        int applied, nw, busy_before;
        for (i = 0; i < 40; i++) {
            s[i].lat_ms = 2.0;
            s[i].code = 5 + (i & 7);
        }
        setup(b, s, 40);
        b->r.b.idle = idle_cb;
        ra_play(&b->r, &b->chip, RATE, 0.12, buf, &played);         /* the capture is ahead of the chip */
        busy_before = !idle_cb(b);
        applied = b->applied;
        nw = b->r.nw;
        ra_settle(&b->r);
        ra_abort(&b->r);
        report(busy_before && b->r.settled == 1 && idle_cb(b) && b->r.dropped >= 1 && b->applied == applied
               && b->r.nw == nw && ra_state(&b->r, &b->chip) == RA_CANCELLED,
               "cancel while capturing: the unit settles first",
               "mid-routine at the stop %.0f, waits for an interrupt at the cancel %.0f, %.0f of its writes dropped "
               "(none applied)", busy_before, idle_cb(b), b->r.dropped);
        ra_free(&b->r);
    }
    if (failures)
        printf("run ahead contract: %d of %d FAILED\n", failures, cases);
    else
        printf("run ahead contract: all %d ok\n", cases);
    free(b);
    return failures ? 1 : 0;
}
