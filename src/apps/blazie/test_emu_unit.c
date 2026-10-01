/* test_emu_unit.c -- the emulator's unit, headless: it boots and speaks its greeting, a chord makes it answer (and
 * the same run without the chord stays quiet there: the control), and it renders faster than real time.
 *
 *   test_emu_unit bl FIRMWARE STATE     the Braille Lite (a chord: dot 1)
 *   test_emu_unit tns FIRMWARE -        the Type 'n Speak from cold (its cold reset's first question, then y
 *                                       answered: "are you sure?"); its key latency and options menu from its
 *                                       factory setup (tns_setup.h)
 * (run_tests passes the source tree's firmware/blazie files)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#define _getpid getpid
#endif
#include "emu_unit.h"
#include "tns_setup.h"

#define RATE 44100

static int g_kind;

static int failures;

/* the unit, its flash done at once: these checks keep the Type 'n Speak's cold-start timeline (y, y, then ready by
   12 s); the flash's own time, with its 32 s erase and chirps, is test_flash.c's */
static emu_unit *make(const char *fw, const char *st, char *err, int errlen)
{
    emu_unit *u = emu_create(g_kind, fw, st, RATE, 0, err, errlen);
    if (u)
        emu_set_flash_timed(u, 0);
    return u;
}

static double rms(const short *x, int n)
{
    double s = 0;
    int i;
    for (i = 0; i < n; i++)
        s += (double)x[i] * x[i];
    return sqrt(s / n) / 32768.0;
}

/* the rms from `from` to `to` s, pressing the key (0 = none: the control) at `at` s -- for the Braille Lite the chord
   dot 1, for the Type 'n Speak y down and up */
static double run(const char *fw, const char *st, int press, double at, double from, double to, double *secs)
{
    char err[256];
    emu_unit *u = make(fw, st, err, sizeof err);
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
        if (press && i <= (int)(at * RATE) && (int)(at * RATE) < i + block) {
            if (g_kind == EMU_TYPE_N_SPEAK) {
                emu_key(u, 0xBD);           /* y down */
                emu_key(u, 0x3D);           /* y up */
            } else
                emu_key(u, 0x01);
        }
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

/* chip ms from a key at 8 s (the greeting over) to the first audible sample, in the app's 10 ms blocks; -1: none in
   1.5 s.  TEST_EMU_QUICK_BREAK=1 never switches quick response on (the control: the quick check must fail).  The
   Type 'n Speak from its factory setup (in its main menu, as the Braille Lite from its state). */
static double key_latency(const char *fw, const char *st, int quick)
{
    char err[256];
    emu_unit *u = make(fw, st, err, sizeof err);
    int block = RATE / 100, i, j;
    short buf[RATE / 100];
    double t0 = -1, found = -1;
    int at = 800;
    if (!u) {
        printf("FAIL create: %s\n", err);
        exit(1);
    }
    if (quick && !getenv("TEST_EMU_QUICK_BREAK"))
        emu_set_quick(u, 1);
    for (i = 0; i < at + 150 && found < 0; i++) {
        if (i == at) {
            t0 = emu_time(u);
            if (g_kind == EMU_TYPE_N_SPEAK) {
                emu_key(u, 0x94);           /* a down */
                emu_key(u, 0x14);           /* a up */
            } else
                emu_key(u, 0x01);
        }
        emu_render(u, buf, block);
        if (t0 >= 0)
            for (j = 0; j < block; j++)
                if (buf[j] > 200 || buf[j] < -200) {
                    found = (emu_time(u) - (double)(block - j) / RATE - t0) * 1000.0;
                    break;
                }
    }
    emu_destroy(u);
    return found;
}

int main(int argc, char **argv)
{
    char d[200];
    static char ready_path[64];
    double greet, with_key, without, secs;
    const char *fw, *st, *ready;
    if (argc < 4) {
        printf("usage: test_emu_unit bl|tns FIRMWARE STATE|-\n");
        return 2;
    }
    g_kind = !strcmp(argv[1], "tns") ? EMU_TYPE_N_SPEAK : EMU_BRAILLE_LITE;
    fw = argv[2];
    st = strcmp(argv[3], "-") ? argv[3] : NULL;
    ready = st;
    if (g_kind == EMU_TYPE_N_SPEAK && !st) {   /* the Type 'n Speak in its main menu: its factory setup, saved */
        char err[256];
        snprintf(ready_path, sizeof ready_path, "test_emu_unit.%d.ready.state", (int)_getpid());
        if (!tns_factory_setup(fw, ready_path, err, sizeof err)) {
            printf("FAIL factory setup: %s\n", err);
            return 1;
        }
        ready = ready_path;
    }
    greet = run(fw, st, 0, 0, 0.0, 6.0, NULL);         /* the Type 'n Speak's cold reset: its first question */
    snprintf(d, sizeof d, "rms %.4f over the first 6 s", greet);
    check("boot greeting", greet > 0.01, d);
    /* the greeting (or the cold reset's first question) is over by ~7 s; a key at 8 s (y: "are you sure?") against
       none */
    with_key = run(fw, st, 1, 8.0, 8.0, 10.0, &secs);
    without = run(fw, st, 0, 8.0, 8.0, 10.0, NULL);
    snprintf(d, sizeof d, "8-10 s: rms %.4f with the chord, %.4f without (the control)", with_key, without);
    check("a chord is answered", with_key > 0.01 && without < 0.002, d);
    snprintf(d, sizeof d, "10 s of unit in %.2f s (%.1fx real time)", secs, 10.0 / secs);
    check("faster than real time", secs < 5.0, d);
    {   /* key to first sound, in chip time: the firmware's own pace (the English Braille Lite ~283 ms, the Spanish
           ~107, the Type 'n Speak ~242: the unit's work before it speaks), which the host must not lengthen; and with
           quick key response the words come sooner */
        double slow = key_latency(fw, ready, 0), fast = key_latency(fw, ready, 1);
        snprintf(d, sizeof d, "key to first sound %.1f ms of chip time (at most 320: the firmware's own)", slow);
        check("key latency", slow > 0 && slow <= 320.0, d);
        snprintf(d, sizeof d, "key to first sound %.1f ms with quick key response, %.1f ms without", fast, slow);
        check("quick key response", fast > 0 && fast <= 100.0 && fast < slow, d);
    }
    {   /* switched off and on: the saved memory is the state format, and the unit boots from it and speaks */
        char err[256], path[64];     /* per process: run_tests runs the bl and tns checks side by side */
        emu_unit *u;
        short *buf = (short *)calloc(RATE, sizeof(short));
        FILE *f;
        long size = -1;
        double again;
        snprintf(path, sizeof path, "test_emu_unit.%d.saved.state", (int)_getpid());
        u = make(fw, st, err, sizeof err);
        emu_render(u, buf, RATE);
        emu_key(u, g_kind == EMU_TYPE_N_SPEAK ? 0xBD : 0x01);
        emu_render(u, buf, RATE);
        check("save", emu_save(u, path), "emu_save returned 1");
        emu_destroy(u);
        if ((f = fopen(path, "rb")) != NULL) {
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            fclose(f);
        }
        again = run(fw, path, 0, 0, 0.0, 2.0, NULL);
        /* the memory, then the clock controller (bl_clock.h) */
        snprintf(d, sizeof d, "%ld bytes (want %ld); booted from it: rms %.4f over the first 2 s", size,
                 (g_kind == EMU_TYPE_N_SPEAK ? 5242880L : 786432L) + BLC_SAVE_SIZE, again);
        check("switched off and on", size == (g_kind == EMU_TYPE_N_SPEAK ? 5242880L : 786432L) + BLC_SAVE_SIZE
                                     && again > 0.01, d);
        remove(path);
        free(buf);
    }
    if (g_kind == EMU_TYPE_N_SPEAK) {  /* the options menu (F9), up arrow -- it polls the 8255's port B for the
                                          chip's A/R; answered FFh it hung -- then escape must be answered */
        char err[256];
        emu_unit *u = make(fw, ready, err, sizeof err);
        static const struct { double t; int down, up; } keys[] = {
            {12.0, 0xC6, 0x46}, {15.0, 0xDA, 0x5A}, {18.0, 0x89, 0x09}};
        int n = 21 * RATE, i, k, block = RATE / 50;
        short *buf = (short *)calloc((size_t)n, sizeof(short));
        double after;
        for (i = 0; i < n; i += block) {
            for (k = 0; k < 3; k++)
                if (i == (int)(keys[k].t * RATE)) {
                    emu_key(u, keys[k].down);
                    emu_key(u, keys[k].up);
                }
            emu_render(u, buf + i, block);
        }
        after = rms(buf + 18 * RATE, 3 * RATE);
        snprintf(d, sizeof d, "rms %.4f after escape (silent if the up arrow hung the unit)", after);
        check("options menu up arrow", after > 0.01, d);
        free(buf);
        emu_destroy(u);
    }
    if (g_kind == EMU_BRAILLE_LITE) {   /* the status menu's % (dots 146) reads the battery gauge; then the unit must
                                           still answer (without the gauge it waited forever) */
        char err[256];
        emu_unit *u = make(fw, st, err, sizeof err);
        int n = 18 * RATE, i, block = RATE / 50;
        short *buf = (short *)calloc((size_t)n, sizeof(short));
        double after;
        for (i = 0; i < n; i += block) {
            if (i == 8 * RATE) emu_key(u, 0x4C);         /* 34-chord: the status menu */
            if (i == 11 * RATE) emu_key(u, 0x29);        /* %: the battery */
            if (i == 15 * RATE) emu_key(u, 0x51);        /* e-chord: leave the menu */
            emu_render(u, buf + i, block);
        }
        after = rms(buf + 15 * RATE, 3 * RATE);
        snprintf(d, sizeof d, "rms %.4f after leaving the menu (silent if the unit hung on the battery gauge)", after);
        check("status menu %", after > 0.01, d);
        free(buf);
        emu_destroy(u);
    }
    if (ready == ready_path)
        remove(ready_path);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
