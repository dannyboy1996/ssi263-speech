/* bl_live.c -- bns_live.exe's --live pipe protocol on the library board (bl_board.c): the gate for the board.
 *
 * Today's Python host (src/hosts/blazie.py) drives it unchanged, so nvda/tools/bns_equiv.py --against the golden
 * vectors shows whether the per-instance board behaves exactly as bns.c's live mode did.
 *
 *   bl_live FIRMWARE --live --state-in STATE --phon-ms MS [--key INSTR=CHORD]...
 * Commands (one per line; replies "W reg val" / "T byte" / "DROPPED n" lines, then "OK cycles"):
 *   B instr | LIVE | R cycles | A 0|1 | S hex | U hex | D | Q
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bl_board.h"

#define MAX_KEYS 64

static void flush_events(bl_unit *u)
{
    const bl_event *ev;
    int n = bl_events(u, &ev), i;
    for (i = 0; i < n; i++) {
        if (ev[i].type == 'W')
            printf("W %d %02X\n", ev[i].a, ev[i].b);
        else
            printf("T %02X\n", ev[i].a);
    }
    bl_clear_events(u);
}

int main(int argc, char **argv)
{
    static char line[70000];
    unsigned long long key_at[MAX_KEYS];
    unsigned char key_val[MAX_KEYS];
    int n_keys = 0, i;
    const char *state = NULL;
    double phon_ms = 70.0;
    char err[256];
    bl_unit *u;
    if (argc < 2) {
        fprintf(stderr, "usage: bl_live FIRMWARE --live --state-in STATE --phon-ms MS [--key INSTR=CHORD]...\n");
        return 2;
    }
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--live")) continue;
        else if (!strcmp(argv[i], "--state-in") && i + 1 < argc) state = argv[++i];
        else if (!strcmp(argv[i], "--phon-ms") && i + 1 < argc) phon_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc && n_keys < MAX_KEYS) {
            unsigned long long at;
            unsigned vv;
            if (sscanf(argv[++i], "%llu=%x", &at, &vv) == 2) { key_at[n_keys] = at; key_val[n_keys++] = (unsigned char)vv; }
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
            return 2;
        }
    }
    u = bl_create(argv[1], state, phon_ms, key_at, key_val, n_keys, err, sizeof err);
    if (!u) {
        fprintf(stderr, "bl_live: %s\n", err);
        return 2;
    }
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'B') {
            bl_boot(u, strtoull(line + 2, 0, 0));
        } else if (!strncmp(line, "LIVE", 4)) {
            bl_live(u);
        } else if (line[0] == 'R') {
            bl_run(u, strtoull(line + 2, 0, 0));
        } else if (line[0] == 'A') {
            bl_set_ar(u, atoi(line + 2));
        } else if (line[0] == 'S') {
            unsigned char buf[35000];
            const char *h = line + 2;
            unsigned v;
            int n = 0;
            while (n < (int)sizeof buf && sscanf(h, "%2x", &v) == 1) {
                buf[n++] = (unsigned char)v;
                h += 2;
            }
            bl_queue(u, buf, n);
        } else if (line[0] == 'U') {
            unsigned v;
            if (sscanf(line + 2, "%2x", &v) == 1)
                bl_urgent(u, (int)v);
        } else if (line[0] == 'D') {
            flush_events(u);
            printf("DROPPED %d\n", bl_drop(u));
        } else if (line[0] == 'Q')
            break;
        flush_events(u);
        printf("OK %llu\n", bl_cycles(u));
        fflush(stdout);
    }
    bl_destroy(u);
    return 0;
}
