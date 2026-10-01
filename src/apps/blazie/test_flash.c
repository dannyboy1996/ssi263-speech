/* test_flash.c -- the units' file flash as the firmware sees it (../../csrc/blazie/flash29.h), headless.
 *
 *   test_flash tns FIRMWARE [--break=instant|persist]   the Type 'n Speak from cold, through the emulator's unit
 *   test_flash bl FIRMWARE STATE [--break=instant]       the Braille Lite on its board
 *
 * Type 'n Speak (Jayson, Timothy): its first start, its cold reset, asks "initialize flash system?" after the file
 * system (tns_setup.h) -- the firmware asks only when the chip answers the 29F016's ID (a 29F040's: it skips the flash
 * and never asks) -- and after y, y erases the chip; the erase takes the chip's 32 s, and the unit chirps through the
 * speech chip meanwhile (heard: a sound every ~2 s); saved and started again, it does not ask again (y, y then erase
 * nothing).
 * Braille Lite: its reset (the i-chord held at power-on, then y four times) erases the flash with the same chirps,
 * each R4 F0, R1 F0, R2 FE, R3 58 and phoneme 17h; a file moved to flash lands at the top of the 2 MB chip the
 * firmware manages (E0h bits 0-1 page it), not over the first 512 KB, and survives a save and a restart.
 *
 * Must fail: --break=instant (every erase done at once: no wait, no chirps), --break=persist (saved before the flash
 * was initialised: the unit asks again), and test_flash_break.exe, built with BLAZIE_FLASH_BREAK (the Type 'n
 * Speak's chip answers a 29F040's ID; the Braille Lite's board puts every bank on the same 512 KB, as before).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "emu_unit.h"
#include "tns_setup.h"
#include "../../csrc/blazie/bl_board.h"

#define RATE 22050
#define BLOCK (RATE / 100)
#define STEP_CYCLES 122880ULL                /* 20 ms of the Braille Lite's 6.144 MHz */

static int failures;
static const char *brk = "";

static void check(const char *name, int ok, const char *detail)
{
    printf("%-4s %-34s %s\n", ok ? "ok" : "FAIL", name, detail);
    failures += !ok;
}

/* ---- the Type 'n Speak, through the emulator's unit ----------------------------------------------------------- */
typedef struct {
    double busy_from, busy_to;               /* chip time of the erase (-1: none) */
    int onsets, erases;                      /* sounds starting while it ran; chip erases */
    double first_rms;                        /* the first 3 s */
} tns_run_t;

/* `secs` of the unit in 10 ms blocks, yes down and up at the n_at times `at`; save to `save` at `save_at` s (0:
   never) */
static int tns_run(const char *fw, const char *st, int yes, const double *at, int n_at, double secs, const char *save,
                   double save_at, tns_run_t *r)
{
    char err[256];
    emu_unit *u = emu_create(EMU_TYPE_N_SPEAK, fw, st, RATE, 0, err, sizeof err);
    short buf[BLOCK];
    int i, j, k, loud = 0, n = (int)(secs * 100);
    double e0 = 0;
    if (!u) {
        printf("FAIL create: %s\n", err);
        return 0;
    }
    if (!strcmp(brk, "instant"))
        emu_set_flash_timed(u, 0);
    r->busy_from = r->busy_to = -1;
    r->onsets = 0;
    for (i = 0; i < n; i++) {
        double s = 0;
        int busy;
        for (k = 0; k < n_at; k++)
            if (i == (int)(at[k] * 100 + 0.5)) {
                emu_key(u, yes | 0x80);
                emu_key(u, yes & 0x7F);
            }
        if (save && i == (int)(save_at * 100) && !emu_save(u, save)) {
            printf("FAIL save %s\n", save);
            emu_destroy(u);
            return 0;
        }
        emu_render(u, buf, BLOCK);
        for (j = 0; j < BLOCK; j++)
            s += (double)buf[j] * buf[j];
        s = sqrt(s / BLOCK) / 32768.0;
        if (i < 300)
            e0 += s * s;
        busy = emu_flash(u, &r->erases) == 1;
        if (busy && r->busy_from < 0)
            r->busy_from = emu_time(u);
        if (!busy && r->busy_from >= 0 && r->busy_to < 0)
            r->busy_to = emu_time(u);
        if (busy && s > 0.005 && !loud)
            r->onsets++;
        loud = s > 0.005;
    }
    r->first_rms = sqrt(e0 / 300);
    emu_destroy(u);
    return 1;
}

static void tns_checks(const char *fw)
{
    char d[300], path[64];
    tns_run_t a, b;
    int yes = tns_setup_yes_code(tns_setup_yes(fw)) & 0x7F, k;   /* y; the Spanish unit's yes is s */
    double span, at[4], done;
    static const double again[] = {3.0, 6.0};
    snprintf(path, sizeof path, "test_flash.%d.state", (int)_getpid());
    /* from cold: its cold reset's first four questions (tns_setup.h): the file system y y, the flash y y -- the
       erase, 32 s, done a little before the folder question; saved then (persist: at 2 s, while the unit still asks;
       the rest of the setup is test_files.c's) */
    for (k = 0; k < 4; k++)
        at[k] = tns_setup_answer_at(k, 1);
    done = tns_setup_answer_at(4, 1) - 2.0;
    if (!tns_run(fw, NULL, yes, at, 4, done + 1.0, path, !strcmp(brk, "persist") ? 2.0 : done, &a))
        exit(1);
    snprintf(d, sizeof d, "%d chip erase after the flash's y, y (0: the firmware refused the chip's ID and never "
             "asked)", a.erases);
    check("flash ID accepted, flash initialised", a.erases == 1, d);
    span = a.busy_from >= 0 && a.busy_to >= 0 ? a.busy_to - a.busy_from : 0;
    snprintf(d, sizeof d, "the erase ran %.1f s of chip time (want 31-33: a 29F016's 32 s)", span);
    check("erase takes the chip's time", span > 31.0 && span < 33.0, d);
    snprintf(d, sizeof d, "%d sounds began while it ran (want >= 12: a chirp every ~2 s)", a.onsets);
    check("erase chirps", a.onsets >= 12, d);
    /* started again from what it saved: it speaks, and y, y erase nothing (it did not ask again) */
    if (!tns_run(fw, path, yes, again, 2, 12.0, NULL, 0, &b))
        exit(1);
    snprintf(d, sizeof d, "restarted: rms %.4f in the first 3 s; %d chip erases after y, y (0: it did not ask again)",
             b.first_rms, b.erases);
    check("flash kept across save and restart", b.first_rms > 0.01 && b.erases == 0, d);
    remove(path);
}

/* ---- the Braille Lite, on its board ---------------------------------------------------------------------------- */
typedef struct { double t; int chord; } bl_key_t;

/* run the board in 20 ms steps to `secs`, pressing `keys`; count the erase's span and the chirp loads during it */
static void bl_steps(bl_unit *u, const bl_key_t *keys, int n_keys, double secs, double *span, int *chirps)
{
    int i, k, ctl = 0, n = (int)(secs * 50), was = 0;
    double from = -1, t;
    *chirps = 0;
    *span = 0;
    for (i = 0; i < n; i++) {
        const bl_event *ev;
        int m, busy;
        t = i * 0.02;
        for (k = 0; k < n_keys; k++)
            if ((int)(keys[k].t * 50 + 0.5) == i)
                bl_key(u, keys[k].chord);
        bl_run(u, STEP_CYCLES);
        busy = bl_flash_busy(u, NULL, NULL) == 1;
        m = bl_events(u, &ev);
        for (k = 0; k < m; k++) {
            if (ev[k].type != 'W')
                continue;
            if (ev[k].a == 3)
                ctl = ev[k].b >> 7;
            else if (ev[k].a == 0 && !ctl && (ev[k].b & 0x3F) == 0x17 && busy)
                ++*chirps;
        }
        bl_clear_events(u);
        if (busy && !was)
            from = t;
        if (!busy && was && from >= 0)
            *span += t - from;
        was = busy;
    }
}

static void bl_checks(const char *fw, const char *st)
{
    char err[256], d[300], path[64];
    bl_unit *u;
    double span;
    int chirps;
    const unsigned char *fl;
    long size = -1;
    FILE *f;
    /* the reset: "initialize file system?" y, "are you sure?" y, "initialize flash system?" y, "are you sure?" y --
       "please wait", the chip erase */
    static const bl_key_t reset[] = {{10, 0x3D}, {16, 0x3D}, {22, 0x3D}, {28, 0x3D}};
    /* o-chord f o a e-chord (open file a), h i, o-chord f, dots 1-2-6 chord (move it), y ("move to flash?") */
    static const bl_key_t move[] = {{8, 0x55}, {10, 0x0B}, {13, 0x15}, {16, 0x01}, {18, 0x51}, {22, 0x13},
                                    {23, 0x0A}, {25, 0x55}, {27, 0x0B}, {31, 0x63}, {35, 0x3D}};
    u = bl_create(fw, NULL, 20.0, NULL, NULL, 0, err, sizeof err);
    if (!u) {
        printf("FAIL create: %s\n", err);
        exit(1);
    }
    bl_hold(u, 0x4A);
    bl_flash_timed(u, strcmp(brk, "instant") != 0);
    bl_steps(u, reset, 4, 64.0, &span, &chirps);
    bl_destroy(u);
    snprintf(d, sizeof d, "the reset's erase ran %.1f s (want 31-33), %d chirps (phoneme 17h) meanwhile (want >= 12)",
             span, chirps);
    check("Braille Lite reset: erase and chirps", span > 31.0 && span < 33.0 && chirps >= 12, d);

    /* a file moved to flash, then saved and restarted */
    snprintf(path, sizeof path, "test_flash.%d.state", (int)_getpid());
    u = bl_create(fw, st, 60.0, NULL, NULL, 0, err, sizeof err);
    if (!u) {
        printf("FAIL create: %s\n", err);
        exit(1);
    }
    bl_flash_timed(u, strcmp(brk, "instant") != 0);
    bl_steps(u, move, (int)(sizeof move / sizeof *move), 47.0, &span, &chirps);
    bl_memory(u, 1, &fl);
    snprintf(d, sizeof d, "top of the 2 MB: %02X %02X (want 68 69, \"hi\"); top of the first 512 KB: %02X %02X (want FF)",
             fl[0x1FFE00], fl[0x1FFE01], fl[0x7FE00], fl[0x7FE01]);
    check("a flash file in the fourth 512 KB", fl[0x1FFE00] == 'h' && fl[0x1FFE01] == 'i' && fl[0x7FE00] == 0xFF
          && fl[0x7FE01] == 0xFF, d);
    if (!bl_save_state(u, path))
        printf("FAIL save %s\n", path);
    bl_destroy(u);
    if ((f = fopen(path, "rb")) != NULL) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
    }
    u = bl_create(fw, path, 60.0, NULL, NULL, 0, err, sizeof err);
    if (u)
        bl_memory(u, 1, &fl);
    snprintf(d, sizeof d, "saved %ld bytes (want 2359296: 256 KB + 2 MB); restarted: %02X %02X at the top", size,
             u ? fl[0x1FFE00] : 0, u ? fl[0x1FFE01] : 0);
    check("the flash file kept across a restart", u && size == 2359296L && fl[0x1FFE00] == 'h' && fl[0x1FFE01] == 'i',
          d);
    bl_destroy(u);
    remove(path);
}

int main(int argc, char **argv)
{
    int i;
    if (argc < 3) {
        printf("usage: test_flash tns FIRMWARE | bl FIRMWARE STATE  [--break=instant|persist]\n");
        return 2;
    }
    for (i = 3; i < argc; i++)
        if (!strncmp(argv[i], "--break=", 8))
            brk = argv[i] + 8;
    if (!strcmp(argv[1], "tns"))
        tns_checks(argv[2]);
    else if (argc > 3 && strncmp(argv[3], "--", 2))
        bl_checks(argv[2], argv[3]);
    else {
        printf("usage: test_flash bl FIRMWARE STATE\n");
        return 2;
    }
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
