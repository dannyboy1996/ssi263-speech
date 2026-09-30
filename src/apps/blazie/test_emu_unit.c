/* test_emu_unit.c -- the emulator's unit, headless: it boots and speaks its greeting, a chord makes it answer (and
 * the same run without the chord stays quiet there: the control), and it renders faster than real time.
 *
 *   test_emu_unit FIRMWARE STATE        (run_tests passes the source tree's firmware/blazie files)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "emu_unit.h"

#define RATE 44100

static int failures;

static double rms(const short *x, int n)
{
    double s = 0;
    int i;
    for (i = 0; i < n; i++)
        s += (double)x[i] * x[i];
    return sqrt(s / n) / 32768.0;
}

/* seconds of audio from `from` to `to` s, pressing `chord` (0 = none) at `at` s */
static double run(const char *fw, const char *st, int chord, double at, double from, double to, double *secs)
{
    char err[256];
    emu_unit *u = emu_create(fw, st, RATE, 0, err, sizeof err);
    int n = (int)(to * RATE), i, block = RATE / 50;
    short *buf;
    double r;
    clock_t c0;
    if (!u) {
        printf("FAIL create: %s\n", err);
        exit(1);
    }
    buf = (short *)calloc((size_t)n, sizeof(short));
    c0 = clock();
    for (i = 0; i < n; i += block) {
        int k = n - i < block ? n - i : block;
        if (chord && i <= (int)(at * RATE) && (int)(at * RATE) < i + block)
            emu_key(u, chord);
        emu_render(u, buf + i, k);
    }
    if (secs)
        *secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
    r = rms(buf + (int)(from * RATE), (int)((to - from) * RATE));
    free(buf);
    emu_destroy(u);
    return r;
}

static void check(const char *name, int ok, const char *detail)
{
    printf("%-4s %-22s %s\n", ok ? "ok" : "FAIL", name, detail);
    failures += !ok;
}

int main(int argc, char **argv)
{
    char d[200];
    double greet, with_key, without, secs;
    if (argc < 3) {
        printf("usage: test_emu_unit FIRMWARE STATE\n");
        return 2;
    }
    greet = run(argv[1], argv[2], 0, 0, 0.0, 2.0, NULL);
    snprintf(d, sizeof d, "rms %.4f over the first 2 s", greet);
    check("boot greeting", greet > 0.01, d);
    /* the greeting is over by ~7 s; a chord at 8 s (dot 1, 'a': the main menu answers) against no chord */
    with_key = run(argv[1], argv[2], 0x01, 8.0, 8.0, 10.0, &secs);
    without = run(argv[1], argv[2], 0, 8.0, 8.0, 10.0, NULL);
    snprintf(d, sizeof d, "8-10 s: rms %.4f with the chord, %.4f without (the control)", with_key, without);
    check("a chord is answered", with_key > 0.01 && without < 0.002, d);
    snprintf(d, sizeof d, "10 s of unit in %.2f s (%.1fx real time)", secs, 10.0 / secs);
    check("faster than real time", secs < 5.0, d);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
