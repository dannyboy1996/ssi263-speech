/* test_bl_board.c -- two units in one process don't affect each other.
 *
 * Each unit (e.g. the English and the Spanish firmware) is run on its own, then both again in the same process,
 * interleaved in small steps; each unit's event stream (SSI writes and serial bytes, in order) must be identical
 * either way.  The drive is fixed: boot, live mode with A/R held requesting, a line queued, a run.
 *
 *   test_bl_board FIRMWARE1 STATE1 FIRMWARE2 STATE2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bl_board.h"

#define STEP 3000ULL
#define STEPS 1500

typedef struct { bl_event *e; int n, cap; } stream;

static void take(bl_unit *u, stream *s)
{
    const bl_event *ev;
    int n = bl_events(u, &ev), i;
    for (i = 0; i < n; i++) {
        if (s->n == s->cap) {
            s->cap = s->cap ? s->cap * 2 : 1024;
            s->e = (bl_event *)realloc(s->e, (size_t)s->cap * sizeof(bl_event));
        }
        s->e[s->n++] = ev[i];
    }
    bl_clear_events(u);
}

static bl_unit *start(const char *fw, const char *st, stream *s)
{
    char err[256];
    static const unsigned char line[] = "Hello, how are you?\r\x06\r\x06";
    bl_unit *u = bl_create(fw, st, 5.0, NULL, NULL, 0, err, sizeof err);
    if (!u) { fprintf(stderr, "%s\n", err); exit(2); }
    bl_boot(u, 3000000ULL);
    bl_live(u);
    bl_set_ar(u, 1);
    bl_queue(u, line, (int)sizeof line - 1);
    take(u, s);
    return u;
}

static int same(const stream *a, const stream *b)
{
    return a->n == b->n && !memcmp(a->e, b->e, (size_t)a->n * sizeof(bl_event));
}

int main(int argc, char **argv)
{
    stream solo[2] = {{0}}, mixed[2] = {{0}};
    bl_unit *u[2];
    int k, i, ok = 1;
    if (argc < 5) { fprintf(stderr, "usage: test_bl_board FW1 STATE1 FW2 STATE2\n"); return 2; }
    for (k = 0; k < 2; k++) {                  /* each alone */
        bl_unit *v = start(argv[1 + 2 * k], argv[2 + 2 * k], &solo[k]);
        for (i = 0; i < STEPS; i++) { bl_run(v, STEP); take(v, &solo[k]); }
        bl_destroy(v);
    }
    u[0] = start(argv[1], argv[2], &mixed[0]); /* both, interleaved */
    u[1] = start(argv[3], argv[4], &mixed[1]);
    for (i = 0; i < STEPS; i++)
        for (k = 0; k < 2; k++) { bl_run(u[k], STEP); take(u[k], &mixed[k]); }
    for (k = 0; k < 2; k++) {
        int w = 0, j;
        for (j = 0; j < solo[k].n; j++) w += solo[k].e[j].type == 'W';
        printf("unit %d: %d events (%d SSI writes) alone, %d interleaved: %s\n", k + 1, solo[k].n, w, mixed[k].n,
               same(&solo[k], &mixed[k]) ? "identical" : "DIFFER");
        ok &= same(&solo[k], &mixed[k]) && w > 0;
        bl_destroy(u[k]);
    }
    printf("%s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
