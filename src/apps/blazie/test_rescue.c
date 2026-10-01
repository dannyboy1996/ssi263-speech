/* test_rescue.c -- a Type 'n Speak saved before the emulator's first start reached the unit's cold reset, told apart
 * and set up anew with its files (tns_rescue.h; the Type 'n Speak's real cold reset; Timothy, Jayson), headless.
 *
 *   test_rescue FIRMWARE [STATE]    (STATE: a saved unit to check as well, read only -- a copy of one of the 0.6 or
 *                                    0.7 previews' tns_english.state)
 *
 * The old unit is made as the previews made it (tns_board.h tns_cold_break: the old cold start; its flash question
 * answered y, y), then with its own keys: notes ("hello world", its first file) and doc ("ab"), doc moved to flash.
 * It must be told apart, with one RAM file and one lost flash file; rescued, the unit must be set up, notes in its
 * RAM startup folder without the first character the old unit never stored (the report saying so), doc named as lost
 * (the old unit never wrote its text: blf_lost_get), the old state kept beside it byte for byte; and the unit itself,
 * started from it, must move notes to flash with its own command.  A unit as it leaves the factory now is not told
 * apart.
 * TEST_RESCUE_BREAK=1 (the control) leaves the old cold start on during the rescue: its setup must be refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#define _getpid getpid
#endif
#include "emu_unit.h"
#include "tns_keys.h"
#include "tns_rescue.h"
#include "tns_setup.h"
#include "../../csrc/blazie/bl_files.h"
#include "../../csrc/blazie/bl_files_state.h"
#include "../../csrc/blazie/tns_board.h"

#define RATE 22050
#define MAX_KEYS 200

static int failures;

static void check(const char *name, int ok, const char *detail)
{
    printf("%-4s %-46s %s\n", ok ? "ok" : "FAIL", name, detail);
    failures += !ok;
}

typedef struct { double t; int code; } key_t_;
typedef struct { key_t_ k[MAX_KEYS]; int n; double t; } script;

/* a key down and up (code: its down code); after it, `gap` s */
static void press(script *s, int code, double gap)
{
    if (s->n < MAX_KEYS) {
        s->k[s->n].t = s->t;
        s->k[s->n++].code = code;
    }
    s->t += gap;
}

static void press_named(script *s, const char *name, double gap) { press(s, tns_code_named(name), gap); }

static void type(script *s, const char *text)
{
    char one[2] = {0, 0};
    for (; *text; text++) {
        one[0] = *text;
        press_named(s, *text == ' ' ? "space" : one, 0.35);
    }
}

static int run(const char *fw, const char *in, const char *out, const script *s, double secs)
{
    char err[256];
    short buf[RATE / 100];
    emu_unit *u = emu_create(EMU_TYPE_N_SPEAK, fw, in, RATE, 0, err, sizeof err);
    int i, k = 0, n = (int)(secs * 100);
    if (!u) {
        printf("FAIL create: %s\n", err);
        return 0;
    }
    emu_set_flash_timed(u, 0);
    for (i = 0; i < n; i++) {
        while (k < s->n && s->k[k].t * 100 <= i) {
            int c = s->k[k++].code;
            if (c & 0x100)                  /* one event as it stands (a modifier held or let go) */
                emu_key(u, c & 0xFF);
            else {
                emu_key(u, c);
                emu_key(u, c & 0x7F);
            }
        }
        emu_render(u, buf, RATE / 100);
    }
    k = emu_save(u, out);
    emu_destroy(u);
    if (!k)
        printf("FAIL save %s\n", out);
    return k;
}

/* the files menu (F1), Shift+NumLock: move the open file; y; Esc */
static void move_file(script *s)
{
    int shift = tns_code_named("lshift"), num = tns_code_named("numlock");
    press_named(s, "f1", 3.6);
    press(s, 0x100 | shift, 0.2);
    press(s, 0x100 | num, 0.2);
    press(s, 0x100 | (num & 0x7F), 0.2);
    press(s, 0x100 | (shift & 0x7F), 3.6);
    press_named(s, "y", 8.6);
    press_named(s, "esc", 3.6);
}

static void open_or_create(script *s, int create, const char *name)
{
    press_named(s, "f1", 3.6);
    press_named(s, create ? "c" : "o", 3.6);
    type(s, name);
    press_named(s, "enter", 3.6);
}

static unsigned char *read_all(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (unsigned char *)malloc((size_t)*n);
    if (d && fread(d, 1, (size_t)*n, f) != (size_t)*n) { free(d); d = NULL; }
    fclose(f);
    return d;
}

/* file `name` in the state: its bytes shown, its place */
static int file_is(const char *path, const char *name, const char *want, int in_flash, char *d, size_t cap)
{
    bls_unit u;
    blf_fs *fs;
    char err[200], text[64];
    int i, ok = 0;
    unsigned long n = 0, k;
    const unsigned char *x;
    if (!bls_load(path, &u, err, sizeof err)) { snprintf(d, cap, "%s", err); return 0; }
    fs = blf_open(u.model, u.ram, u.flash, u.flash_size, err, sizeof err);
    i = fs ? blf_find(fs, name) : -1;
    x = i >= 0 ? blf_data(fs, i, &n) : NULL;
    for (k = 0; x && k < n && k < sizeof text - 1; k++)
        text[k] = x[k] >= 32 && x[k] < 127 ? (char)x[k] : '?';
    text[x ? k : 0] = 0;
    if (x) {
        const blf_file *f = blf_get(fs, i);
        snprintf(d, cap, "%s \"%s\" in %s, folder %d", name, text, f->in_flash ? "flash" : "RAM", f->folder);
        ok = n == strlen(want) && !memcmp(x, want, n) && f->in_flash == in_flash && f->folder == (in_flash ? 1 : 0)
             && blf_ram_ok(fs) && blf_folders_ok(fs) && blf_check(fs, err, sizeof err);
    } else
        snprintf(d, cap, "%s: not on the unit", name);
    blf_close(fs);
    bls_free(&u);
    return ok;
}

int main(int argc, char **argv)
{
    char old[64], fresh[64], before[80], err[300], d[600];
    const char *fw;
    script s;
    tns_rescue_report r;
    int got, ok;
    long n0 = 0, n1;
    unsigned char *a, *orig;
    if (argc < 2) {
        printf("usage: test_rescue FIRMWARE [STATE]\n");
        return 2;
    }
    fw = argv[1];
    snprintf(old, sizeof old, "test_rescue.%d.state", (int)_getpid());
    snprintf(fresh, sizeof fresh, "test_rescue.%d.fresh.state", (int)_getpid());
    snprintf(before, sizeof before, "%s.before-setup", old);

    /* the unit as the previews made it: the old cold start, its flash question y y; then notes, doc, doc to flash */
    tns_cold_break = 1;
    memset(&s, 0, sizeof s);
    s.t = 3.0;
    press_named(&s, "y", 3.0);
    press_named(&s, "y", 8.0);
    open_or_create(&s, 1, "notes");
    type(&s, "hello world");
    s.t += 2.0;
    open_or_create(&s, 1, "doc");
    type(&s, "ab");
    s.t += 2.0;
    move_file(&s);
    if (!run(fw, NULL, old, &s, s.t + 3.0))
        return 1;
    tns_cold_break = 0;

    got = tns_needs_setup(old, &r);
    snprintf(d, sizeof d, "never set up: %s; %d RAM file, %d lost flash file", got == 1 ? "yes" : got ? "?" : "no",
             r.ram_files, r.lost_flash);
    check("the old unit told apart, its files found", got == 1 && r.ram_files == 1 && r.lost_flash == 1, d);

    orig = read_all(old, &n0);
    if (getenv("TEST_RESCUE_BREAK"))
        tns_cold_break = 1;
    ok = tns_rescue(fw, old, 1, &r, err, sizeof err);
    tns_cold_break = 0;
    snprintf(d, sizeof d, "%s; %d carried, %d missing a first character, %d named only", ok ? "done" : err, r.carried,
             r.first_lost, r.unrecoverable);
    check("rescued: set up anew, its files carried", ok && r.carried == 1 && r.first_lost == 1
          && strstr(r.log, "first character") && r.unrecoverable == 1 && strstr(r.log, "LOST: doc"), d);
    if (ok) {
        check("notes in the RAM startup folder", file_is(old, "notes", "ello world", 0, d, sizeof d), d);
        a = read_all(before, &n1);
        snprintf(d, sizeof d, "%s: %ld bytes, %s", before, a ? n1 : -1L,
                 a && orig && n1 == n0 && !memcmp(a, orig, (size_t)n0) ? "the old unit's, byte for byte" : "DIFFERENT");
        check("the old state kept beside it", a && orig && n1 == n0 && !memcmp(a, orig, (size_t)n0), d);
        free(a);
        /* the unit as its own factory setup left it (the rescue's) is not told apart */
        got = tns_needs_setup(old, NULL);
        snprintf(d, sizeof d, "never set up: %s", got == 1 ? "yes" : got ? "?" : "no");
        check("a factory-set-up unit is not told apart", got == 0, d);
        /* the unit itself: notes opened and moved to flash with its own command */
        memset(&s, 0, sizeof s);
        s.t = 8.0;
        open_or_create(&s, 0, "notes");
        move_file(&s);
        if (run(fw, old, fresh, &s, s.t + 3.0)) {
            check("the unit moves notes to flash", file_is(fresh, "notes", "ello world", 1, d, sizeof d), d);
            remove(fresh);
        }
    }
    free(orig);
    if (argc > 2) {                         /* a preview's saved unit: told apart, read only */
        got = tns_needs_setup(argv[2], &r);
        snprintf(d, sizeof d, "%s: never set up: %s; %d RAM files, %d lost flash files", argv[2],
                 got == 1 ? "yes" : got ? "unreadable" : "no", r.ram_files, r.lost_flash);
        check("the given state", got >= 0, d);
    }
    remove(old);
    remove(before);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
