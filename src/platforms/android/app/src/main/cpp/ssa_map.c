/* ssa_map.c -- see ssa_map.h. */
#include <math.h>
#include "ssa_map.h"

int ssa_map_break = 0;

static int clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

int ssa_ssip_from_percent(int percent)
{
    double s;
    if (percent <= 0 || percent == 100)
        return 0;
    s = 50.0 * log2(percent / 100.0);
    return clamp((int)floor(s + 0.5), -100, 100);
}

int ssa_to100(int ssip)                               /* sd_ssi263.c's to100, as it is there */
{
    int v = (ssip + 100) / 2;
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static int on_top(int slider, int percent)
{
    return clamp(clamp(slider, 0, 100) + ssa_to100(ssa_ssip_from_percent(percent)) - 50, 0, 100);
}

int ssa_rate(int slider, int percent)
{
    return ssa_map_break ? clamp(slider, 0, 100) : on_top(slider, percent);
}

int ssa_pitch(int slider, int percent)
{
    return on_top(slider, percent);
}
