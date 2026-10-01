/* test_audio.c -- the shell's sound queue (audio_pace.h) against a simulated sound card, headless, in simulated time
 * (Tomi: the emulator's speech stutters, the add-on's doesn't).
 *
 *   test_audio              the checks
 *   test_audio --old        the 0.7.0 draft's queue put back: four blocks of 10 ms, never grown, and the minute's
 *                           save on the window's thread holding the unit while the sound thread waits.  Must FAIL
 *                           (run_tests' control): a busy machine's and a remote card's gaps never stop, a slow save
 *                           leaves gaps.  (Gaps in the first 10 s are the automatic queue growing; a hold-up in the
 *                           first second, while the first blocks go out, is heard whatever the queue.)
 *   test_audio --calibrate  the model beside main_win's waveOut as measured on this desktop (below)
 *
 * The simulation, in steps of 0.1 ms.  The sound thread as main_win.c's: woken when the card hands a block back (or
 * after 100 ms), it reads the card's played position (ap_observe), renders ap_want blocks one after another and
 * queues each, then runs a waiting save when ap_save_now says so.  Now and then it is held up before it queues a
 * block (a busy or power-saving machine, Remote Desktop); some renders are slow.  The card plays continuously; a
 * queued block reaches it `lat` ms later (taken at the card's passes, every `period` ms, each up to `late` ms late),
 * and is handed back once played.  When the card has nothing to play the sound stops (a gap), and it starts again
 * only at a pass.
 *
 * Calibrated against the real thing (src/apps/blazie/README.md): waveOut on this desktop's card, the thread held up
 * a fixed time each second, or before 5% of its blocks for up to 60 ms; the sound lost by the card's played position
 * against the wall clock.  With lat 25 ms:              measured          model
 *   4 blocks, held 10 / 30 / 50 ms each second          110 / 390 / 540   121 / 360 / 551 ms lost of 12 s
 *   8 blocks, held 10, 20, 30 ms each second            0                 0
 *   4 / 8 / 12 blocks, 5% held up to 60 ms              1610-2007 / 370 / 30-50    1821 / 350 / 10 ms of 15 s
 * So four blocks of 10 ms (the 0.7.0 draft's) leave ~10 ms to spare on a card this fast: a thread held up longer is
 * heard as a gap, every time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_pace.h"

#define DT 0.1                       /* ms per step */
#define BLOCK_MS 10
#define SETTLE_MS 10000.0            /* the automatic queue has grown by then */
#define WARM_MS 1000.0               /* the start: the first blocks, a hold-up there is heard whatever the queue */
#define RING 4096

static int failures, old_queue;

static void check(const char *name, int ok, const char *detail)
{
    printf("%-4s %-48s %s\n", ok ? "ok" : "FAIL", name, detail);
    failures += !ok;
}

/* ---- a reproducible random number in [0, 1) ------------------------------------------------------------------- */
static unsigned long long g_rng;
static double rnd(void)
{
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(g_rng >> 11) / 9007199254740992.0;
}

typedef struct {
    double secs;
    double period, late, lat;        /* the card: a pass each `period` ms (up to `late` ms late); a block reaches it
                                        `lat` ms after it is queued */
    double p_stall, stall_max;       /* the thread held up before queueing a block: this often, up to stall_max ms */
    double p_slow, slow_ms;          /* a render: this often slow_ms (else 0.3-0.7 ms) */
    double save_every, save_ms;      /* the minute's save (0: none): every save_every ms, taking save_ms */
    unsigned long long seed;
    int fixed;                       /* calibration: this many blocks, never grown (0: the mode's queue) */
    int stall_each;                  /* calibration: every this many blocks the thread held stall_max ms */
} scenario;

typedef struct {
    double gap_ms, gap_after;        /* sound lost, in all and after SETTLE_MS */
    int gaps, gaps_after, gaps_warm; /* gaps: in all, after SETTLE_MS, after WARM_MS */
    double last_gap;                 /* when the last one began (ms) */
    int target_end, saves, detected; /* the queue at the end (blocks); saves; gaps ap_observe found */
    double save_wait_max;            /* a save's wait from due to start */
} result;

static result simulate(const scenario *s, int mode)
{
    static double queued_at[RING];   /* when each block was queued */
    audio_pace p;
    result r;
    double t = 0, end = s->secs * 1000.0;
    /* the card */
    double card = 0, played = 0, next_pass = 0;
    int written = 0, taken = 0, returned = 0, started = 0, dry = 0, pass = 0;
    /* the sound thread: waiting for a hand-back, waking, rendering, saving */
    enum { WAIT, WAKE, RENDER, SAVE } state = WAKE;
    double until = 0, waited_from = 0, save_due = s->save_every > 0 ? s->save_every : 1e18, gui_until = -1;
    int signalled = 0, to_render = 0, rendering = 0, save_pending = 0;
    memset(&r, 0, sizeof r);
    g_rng = s->seed;
    ap_init(&p, old_queue ? AP_SHORT : mode, BLOCK_MS);
    if (s->fixed)
        p.target = s->fixed;
    while (t < end) {
        /* the card: plays a step, hands back each block played, takes the blocks that have reached it at a pass */
        if (card >= DT && !dry) {
            card -= DT;
            played += DT;
        } else if (started) {
            double lost = DT - (dry ? 0.0 : card);
            if (!dry) {
                played += card;
                card = 0;
                r.gaps++;
                if (t >= SETTLE_MS) r.gaps_after++;
                if (t >= WARM_MS) r.gaps_warm++;
                r.last_gap = t;
            }
            dry = 1;
            r.gap_ms += lost;
            if (t >= SETTLE_MS) r.gap_after += lost;
        }
        while ((returned + 1) * BLOCK_MS <= played + 1e-6) {
            returned++;
            signalled = 1;           /* the block handed back: the thread's event */
        }
        if (t >= next_pass) {
            while (taken < written && t >= queued_at[taken % RING] + s->lat) {
                taken++;
                card += BLOCK_MS;
                started = 1;
            }
            if (card > 0)
                dry = 0;             /* run dry, the card starts again only at a pass */
            pass++;
            next_pass = pass * s->period + rnd() * s->late;
        }
        /* the minute's save comes due */
        if (t >= save_due) {
            save_due += s->save_every;
            if (old_queue)
                gui_until = t + s->save_ms;   /* the window's thread holds the unit: no render meanwhile */
            else if (!save_pending) {
                save_pending = 1;
                waited_from = t;
                signalled = 1;       /* the window wakes the sound thread */
            }
            r.saves++;
        }
        /* the sound thread */
        switch (state) {
        case WAIT:
            if (signalled || t - until >= 100.0) {
                signalled = 0;
                state = WAKE;
                until = t + 0.02 + rnd() * 0.28;
            }
            break;
        case WAKE:
            if (t >= until) {
                if (ap_observe(&p, t, played) > 0)
                    r.detected++;
                to_render = ap_want(&p, written - returned, save_pending);
                state = RENDER;
            }
            break;
        case RENDER:
            if (rendering && t >= until) {    /* a render finished: its block queued */
                queued_at[written % RING] = t;
                written++;
                rendering = 0;
                to_render--;
            }
            if (rendering || t < gui_until)   /* (the window's save holds the unit: the render waits) */
                break;
            if (to_render > 0) {
                rendering = 1;
                until = t + (rnd() < s->p_slow ? s->slow_ms : 0.3 + rnd() * 0.4)
                        + (rnd() < s->p_stall ? rnd() * s->stall_max : 0.0)
                        + (s->stall_each && written % s->stall_each == s->stall_each / 2 ? s->stall_max : 0.0);
            } else if (ap_save_now(&p, written - returned, save_pending)) {
                if (t - waited_from > r.save_wait_max)
                    r.save_wait_max = t - waited_from;
                state = SAVE;
                until = t + s->save_ms;
            } else if (signalled) {           /* an event set meanwhile: the wait returns at once */
                signalled = 0;
                state = WAKE;
                until = t;
            } else {
                state = WAIT;
                until = t;
            }
            break;
        case SAVE:
            if (t >= until) {
                save_pending = 0;
                state = WAKE;
                until = t;
            }
            break;
        }
        t += DT;
    }
    r.target_end = ap_target(&p);
    return r;
}

static void grows(const char *name, const scenario *s)
{
    char label[80], d[300];
    result r = simulate(s, AP_AUTO);
    snprintf(d, sizeof d, "%d gaps after 10 s (%.0f ms), %d in all (%.0f ms, the last at %.1f s), %d found; queue %d "
             "ms at the end", r.gaps_after, r.gap_after, r.gaps, r.gap_ms, r.last_gap / 1000.0, r.detected,
             r.target_end * BLOCK_MS);
    snprintf(label, sizeof label, "%s: no gap after 10 s", name);
    check(label, r.gaps_after == 0, d);
    snprintf(label, sizeof label, "%s: under 400 ms lost while it grows", name);
    check(label, r.gap_ms < 400.0, d);
    snprintf(label, sizeof label, "%s: the queue at most 250 ms", name);
    check(label, r.target_end * BLOCK_MS <= AP_MAX_MS, d);
    r = simulate(s, AP_LONG);
    snprintf(d, sizeof d, "%d gaps after the first second, %d in all (%.0f ms) with the long queue", r.gaps_warm,
             r.gaps, r.gap_ms);
    snprintf(label, sizeof label, "%s, long: no gap after the first second", name);
    check(label, r.gaps_warm == 0, d);
}

static void scenario_checks(void)
{
    /* this desktop's card, the thread rarely held up, and then 2 ms at most */
    static const scenario steady = {.secs = 60, .period = 10, .late = 1, .lat = 25, .p_stall = 0.002, .stall_max = 2,
                                    .seed = 1};
    /* the same card on a busy or power-saving machine: 5% of the blocks held up to 60 ms (measured above) and 1% of
       the renders 15 ms */
    static const scenario busy = {.secs = 60, .period = 10, .late = 1, .lat = 25, .p_stall = 0.05, .stall_max = 60,
                                  .p_slow = 0.01, .slow_ms = 15, .seed = 2};
    /* a card farther away (Remote Desktop's audio, a Bluetooth headset): a block reaches it 50 ms after it is queued,
       in passes of 20 ms up to 10 ms late; 2% of the blocks held up to 40 ms */
    static const scenario remote = {.secs = 60, .period = 20, .late = 10, .lat = 50, .p_stall = 0.02, .stall_max = 40,
                                    .p_slow = 0.01, .slow_ms = 15, .seed = 3};
    /* this desktop's card, and a save of 50 ms every 10 s (a slow disk; this desktop's take 0.5-1.4 ms) */
    static const scenario saving = {.secs = 60, .period = 10, .late = 1, .lat = 25, .p_stall = 0.002, .stall_max = 2,
                                    .save_every = 10000, .save_ms = 50, .seed = 4};
    char d[300];
    result r;

    r = simulate(&steady, AP_AUTO);
    snprintf(d, sizeof d, "%d gaps after the first second, %d in all (%.0f ms); queue %d ms at the end", r.gaps_warm,
             r.gaps, r.gap_ms, r.target_end * BLOCK_MS);
    check("steady card: no gap after the first second", r.gaps_warm == 0, d);
    if (!old_queue)
        check("steady card: the automatic queue stays at 60 ms", r.target_end * BLOCK_MS == AP_AUTO_START_MS, d);

    grows("busy machine", &busy);
    grows("remote card", &remote);

    r = simulate(&saving, AP_AUTO);
    snprintf(d, sizeof d, "%d saves of 50 ms: %d gaps after the first second (%.0f ms in all); a save waited at most "
             "%.1f ms", r.saves, r.gaps_warm, r.gap_ms, r.save_wait_max);
    check("autosave: no gap", r.gaps_warm == 0 && r.saves >= 5, d);
    check("autosave: a save waits at most 20 ms", r.save_wait_max <= 20.0, d);
}

/* ---- the gap detector alone ----------------------------------------------------------------------------------- */
/* `secs` of a card playing: drift (ms per s its clock runs slow of the wall clock's), its position reported in steps
   of `steps` ms (0: smooth), gaps of gap_ms every gap_every ms (0: none); observed each 10 ms.  Gaps found, and their
   sum in *found_ms. */
static int detector(double secs, double drift, double steps, double gap_every, double gap_ms, double *found_ms)
{
    audio_pace p;
    double t, played = 0, next_gap = gap_every > 0 ? gap_every : 1e18, gap_left = 0;
    int found = 0;
    ap_init(&p, AP_SHORT, BLOCK_MS);
    *found_ms = 0;
    for (t = 0; t < secs * 1000.0; t += 1.0) {
        double seen, g;
        if (t >= next_gap) {
            gap_left = gap_ms;
            next_gap += gap_every;
        }
        if (gap_left > 0)
            gap_left -= 1.0;
        else
            played += 1.0 - drift / 1000.0;
        if ((int)t % 10)
            continue;
        seen = steps > 0 ? steps * (double)(long)(played / steps) : played;
        if ((g = ap_observe(&p, t, seen)) > 0) {
            found++;
            *found_ms += g;
        }
    }
    return found;
}

static void detector_checks(void)
{
    char d[200];
    double ms;
    int n;
    n = detector(120, 1.0, 0, 0, 0, &ms);
    snprintf(d, sizeof d, "%d found (the card's clock 0.1%% slow)", n);
    check("detector: no gap in a slow clock", n == 0, d);
    n = detector(120, -1.0, 0, 0, 0, &ms);
    snprintf(d, sizeof d, "%d found (the card's clock 0.1%% fast)", n);
    check("detector: no gap in a fast clock", n == 0, d);
    n = detector(120, 0, 40, 0, 0, &ms);
    snprintf(d, sizeof d, "%d found (the position in 40 ms steps)", n);
    check("detector: no gap in a stepped position", n == 0, d);
    n = detector(61, 0.5, 0, 3000, 20, &ms);    /* gaps at 3, 6, ... 60 s */
    snprintf(d, sizeof d, "%d found of 20, %.0f ms of 400", n, ms);
    check("detector: 20 gaps of 20 ms found", n == 20 && ms > 360 && ms < 440, d);
    n = detector(61, 0, 20, 3000, 30, &ms);
    snprintf(d, sizeof d, "%d found of 20, %.0f ms of 600 (the position in 20 ms steps: each within a step)", n, ms);
    check("detector: gaps found in a stepped position", n == 20 && ms > 500 && ms < 1000, d);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--calibrate")) {
        static const int nb[] = {4, 8, 12};
        static const double held[] = {10, 20, 30, 50};
        int k, m;
        for (k = 0; k < 3; k++)
            for (m = 0; m < 4; m++) {
                scenario c = {.secs = 12, .period = 10, .late = 1, .lat = 25, .stall_max = held[m], .seed = 7,
                              .fixed = nb[k], .stall_each = 100};
                result r = simulate(&c, AP_SHORT);
                printf("%2d blocks, held %2.0f ms each second: %4.0f ms lost of 12 s\n", nb[k], held[m], r.gap_ms);
            }
        for (k = 0; k < 3; k++) {
            scenario c = {.secs = 15, .period = 10, .late = 1, .lat = 25, .p_stall = 0.05, .stall_max = 60, .seed = 7,
                          .fixed = nb[k]};
            result r = simulate(&c, AP_SHORT);
            printf("%2d blocks, 5%% of the blocks held up to 60 ms: %4.0f ms lost of 15 s\n", nb[k], r.gap_ms);
        }
        return 0;
    }
    old_queue = argc > 1 && !strcmp(argv[1], "--old");
    if (old_queue)
        printf("the 0.7.0 draft's queue: four blocks of 10 ms, the save on the window's thread\n");
    scenario_checks();
    detector_checks();
    if (failures)
        printf("audio: %d FAILED\n", failures);
    else
        printf("audio: all passed\n");
    return failures ? 1 : 0;
}
