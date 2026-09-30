/* test_chords.c -- chords.c: keys pressed and released in any order make one chord, sent on the last release. */
#include <stdio.h>
#include "chords.h"

static int failures;

static void check(const char *name, int got, int want)
{
    printf("%-4s %-28s got %02X want %02X\n", got == want ? "ok" : "FAIL", name, got, want);
    if (got != want)
        failures++;
}

int main(void)
{
    chord_state s;
    int r1, r2, r3;
    chord_reset(&s);
    /* dots 1 2 4 (f): down in order, up in another order -- only the last up sends */
    chord_down(&s, CHORD_DOT(1));
    chord_down(&s, CHORD_DOT(2));
    chord_down(&s, CHORD_DOT(4));
    r1 = chord_up(&s, CHORD_DOT(2));
    r2 = chord_up(&s, CHORD_DOT(4));
    r3 = chord_up(&s, CHORD_DOT(1));
    check("no chord before the last up", r1 | r2, 0);
    check("f = dots 124", r3, 0x0B);
    /* a key released and pressed again inside one chord stays in it */
    chord_down(&s, CHORD_SPACE);
    chord_down(&s, CHORD_DOT(1));
    chord_up(&s, CHORD_DOT(1));
    chord_down(&s, CHORD_DOT(5));
    chord_up(&s, CHORD_DOT(5));
    check("space chord 15 (e-chord)", chord_up(&s, CHORD_SPACE), 0x51);
    /* auto-repeat: the same key down again changes nothing */
    chord_down(&s, CHORD_DOT(3));
    chord_down(&s, CHORD_DOT(3));
    chord_down(&s, CHORD_DOT(3));
    check("auto-repeat", chord_up(&s, CHORD_DOT(3)), 0x04);
    /* an up for a key never seen down (the window got focus mid-press) sends nothing */
    check("stray up", chord_up(&s, CHORD_DOT(6)), 0);
    /* the next chord starts clean */
    chord_down(&s, CHORD_ADVANCE);
    check("advance bar alone", chord_up(&s, CHORD_ADVANCE), 0x80);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
