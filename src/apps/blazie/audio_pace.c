/* audio_pace.c -- see audio_pace.h. */
#include <string.h>
#include "audio_pace.h"

static const char *const NAMES[AP_N_MODES] = {"auto", "short", "medium", "long"};

static int blocks(int ms, int block_ms)
{
    return (ms + block_ms - 1) / block_ms;
}

int ap_capacity(int block_ms)
{
    if (block_ms < AP_MIN_BLOCK_MS)
        block_ms = AP_MIN_BLOCK_MS;
    return blocks(AP_MAX_MS, block_ms) + blocks(AP_SAVE_AHEAD_MS, block_ms);
}

void ap_set_mode(audio_pace *p, int mode)
{
    static const int MS[AP_N_MODES] = {AP_AUTO_START_MS, AP_SHORT_MS, AP_MEDIUM_MS, AP_LONG_MS};
    if (mode < 0 || mode >= AP_N_MODES)
        mode = AP_AUTO;
    p->mode = mode;
    p->target = blocks(MS[mode], p->block_ms);
}

void ap_init(audio_pace *p, int mode, int block_ms)
{
    memset(p, 0, sizeof *p);
    p->block_ms = block_ms < AP_MIN_BLOCK_MS ? AP_MIN_BLOCK_MS : block_ms;
    p->over_since = -1.0;
    p->last_grow = -1e9;
    ap_set_mode(p, mode);
}

int ap_target(const audio_pace *p)
{
    return p->target;
}

static void grow(audio_pace *p, double now_ms)
{
    int most = blocks(AP_MAX_MS, p->block_ms), step = p->target / 2;
    if (now_ms - p->last_grow < AP_GROW_HOLD_MS)
        return;
    p->last_grow = now_ms;
    if (step < blocks(AP_GROW_MIN_MS, p->block_ms))
        step = blocks(AP_GROW_MIN_MS, p->block_ms);
    p->target = p->target + step > most ? most : p->target + step;
}

double ap_observe(audio_pace *p, double now_ms, double played_ms)
{
    double lag = now_ms - played_ms, excess, gap;
    if (!p->primed) {                /* the card has not started playing: nothing to measure yet */
        if (played_ms <= 0.0)
            return 0.0;
        p->primed = 1;
        p->base = lag;
        p->last_t = now_ms;
        return 0.0;
    }
    if (p->base + AP_DRIFT_PER_S * (now_ms - p->last_t) / 1000.0 < lag)
        p->base += AP_DRIFT_PER_S * (now_ms - p->last_t) / 1000.0;
    else
        p->base = lag;               /* the lowest lead: the card playing without a gap */
    p->last_t = now_ms;
    excess = lag - p->base;
    if (excess < AP_GAP_MIN_MS) {
        p->over_since = -1.0;
        return 0.0;
    }
    if (p->over_since < 0.0) {
        p->over_since = now_ms;
        return 0.0;
    }
    if (now_ms - p->over_since < AP_GAP_PERSIST_MS)
        return 0.0;
    gap = excess;                    /* held: the sound stopped this long, for good */
    p->base = lag;
    p->over_since = -1.0;
    p->gaps++;
    p->gap_ms += gap;
    if (p->mode == AP_AUTO)
        grow(p, now_ms);
    return gap;
}

int ap_want(const audio_pace *p, int in_flight, int save_pending)
{
    int goal = p->target + (save_pending ? blocks(AP_SAVE_AHEAD_MS, p->block_ms) : 0), cap = ap_capacity(p->block_ms);
    if (goal > cap)
        goal = cap;
    return goal > in_flight ? goal - in_flight : 0;
}

int ap_save_now(const audio_pace *p, int in_flight, int save_pending)
{
    return save_pending && ap_want(p, in_flight, 1) == 0;
}

const char *ap_mode_name(int mode)
{
    return mode >= 0 && mode < AP_N_MODES ? NAMES[mode] : NAMES[AP_AUTO];
}

int ap_mode_of(const char *name)
{
    int k;
    for (k = 0; k < AP_N_MODES; k++)
        if (!strcmp(name, NAMES[k]))
            return k;
    return -1;
}
