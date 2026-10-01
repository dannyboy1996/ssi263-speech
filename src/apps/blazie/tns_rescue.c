/* tns_rescue.c -- see tns_rescue.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tns_rescue.h"
#include "tns_setup.h"
#include "../../csrc/blazie/bl_files.h"
#include "../../csrc/blazie/bl_files_state.h"
#include "../../csrc/blazie/tns_board.h"

static void logf_(tns_rescue_report *r, const char *line)
{
    size_t n = strlen(r->log);
    if (n + strlen(line) + 2 < sizeof r->log)
        snprintf(r->log + n, sizeof r->log - n, "%s\n", line);
}

static int open_state(const char *path, bls_unit *u, blf_fs **fs, char *err, int errlen)
{
    if (!bls_load(path, u, err, errlen))
        return 0;
    *fs = blf_open(u->model, u->ram, u->flash, u->flash_size, err, errlen);
    if (!*fs) {
        bls_free(u);
        return 0;
    }
    return 1;
}

static void count(const blf_fs *fs, tns_rescue_report *r)
{
    blf_file f;
    unsigned long n;
    int i;
    memset(r, 0, sizeof *r);
    for (i = 0; i < blf_count(fs); i++)
        if (!blf_get(fs, i)->system && !blf_get(fs, i)->in_flash)
            r->ram_files++;
    while (blf_lost_get(fs, r->lost_flash, &f, &n))
        r->lost_flash++;
}

int tns_needs_setup(const char *path, tns_rescue_report *r)
{
    bls_unit u;
    blf_fs *fs;
    char err[200];
    int broken;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fclose(f);
    if (!open_state(path, &u, &fs, err, sizeof err))
        return -1;
    broken = u.model == BLF_TYPE_N_SPEAK && (!blf_ram_ok(fs) || !blf_folders_ok(fs));
    if (r)
        count(fs, r);
    blf_close(fs);
    bls_free(&u);
    return broken;
}

/* a name the new unit does not have yet: the name itself, else a digit before its extension */
static void free_name(const blf_fs *fs, const char *name, char *out, size_t cap)
{
    const char *dot = strchr(name, '.');
    size_t stem = dot ? (size_t)(dot - name) : strlen(name);
    int k;
    snprintf(out, cap, "%s", name);
    for (k = 2; k <= 9 && blf_find(fs, out) >= 0; k++) {
        size_t s = stem > BLF_NAME_MAX - 5 ? BLF_NAME_MAX - 5 : stem;
        snprintf(out, cap, "%.*s%d%s", (int)s, name, k, dot ? dot : "");
    }
}

static int carry(blf_fs *to, int folder, const blf_file *f, const unsigned char *d, unsigned long n, int dropped,
                 tns_rescue_report *r)
{
    char name[BLF_NAME_MAX + 8], line[200], e[200];
    free_name(to, f->name, name, sizeof name);
    if (!blf_add(to, folder, name, d, n, f->dos_time, f->dos_date, (f->prot & 2) != 0, e, sizeof e)) {
        snprintf(line, sizeof line, "NOT carried: %s (%lu bytes): %s", f->name, n, e);
        logf_(r, line);
        return 0;
    }
    r->carried++;
    r->first_lost += dropped > 0;
    {
        const char *missing = dropped > 0 ? "; its first character was never stored by the old unit and is missing"
                                          : "";
        if (strcmp(name, f->name))
            snprintf(line, sizeof line, "%s %s (was %s), %lu bytes%s", f->in_flash ? "flash" : "RAM", name, f->name,
                     n, missing);
        else
            snprintf(line, sizeof line, "%s %s, %lu bytes%s", f->in_flash ? "flash" : "RAM", name, n, missing);
    }
    logf_(r, line);
    return 1;
}

static int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b;
    char buf[65536];
    size_t n;
    int ok = 1;
    if (!a)
        return 0;
    b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        ok = ok && fwrite(buf, 1, n, b) == n;
    fclose(a);
    return (fclose(b) == 0) && ok;
}

int tns_rescue(const char *firmware, const char *path, int keep_files, tns_rescue_report *r, char *err, int errlen)
{
    char tmp[1100], before[1100];
    bls_unit old, fresh;
    blf_fs *ofs = NULL, *nfs = NULL;
    long prog = tns_program_end(firmware);
    int i, ok = 0;
    if (prog < 0) {
        snprintf(err, (size_t)errlen, "no Type 'n Speak firmware in %s", firmware);
        return 0;
    }
    if (!open_state(path, &old, &ofs, err, errlen))
        return 0;
    count(ofs, r);
    snprintf(tmp, sizeof tmp, "%s.setup", path);
    snprintf(before, sizeof before, "%s.before-setup", path);
    if (!tns_factory_setup(firmware, tmp, err, errlen) || !open_state(tmp, &fresh, &nfs, err, errlen))
        goto done_old;
    if (!blf_ram_ok(nfs) || !blf_flash_ok(nfs) || !blf_folders_ok(nfs)) {
        snprintf(err, (size_t)errlen, "the unit's own setup did not complete");
        goto done;
    }
    if (keep_files) {
        blf_file f;
        unsigned long n;
        const unsigned char *d;
        for (i = 0; i < blf_count(ofs); i++) {
            const blf_file *g = blf_get(ofs, i);
            long start, drop = 0;
            if (g->system)
                continue;
            d = blf_data(ofs, i, &n);
            if (!g->in_flash) {             /* a byte below the program's end was never stored: it is the program's */
                start = (long)(d - old.ram);
                if (start < prog) {
                    drop = prog - start < (long)n ? prog - start : (long)n;
                    d += drop;
                    n -= (unsigned long)drop;
                }
            }
            carry(nfs, g->in_flash ? 1 : 0, g, d, n, (int)drop, r);
        }
        for (i = 0; (d = blf_lost_get(ofs, i, &f, &n)) != NULL; i++) {
            unsigned long k;
            for (k = 0; k < n && d[k] == 0xFF; k++) {}
            if (k < n || !n)
                carry(nfs, 1, &f, d, n, 0, r);
            else {                          /* the old unit never wrote its text: only the name is left */
                char line[200];
                snprintf(line, sizeof line, "LOST: %s (%lu bytes), moved to flash by the old unit, which never wrote "
                         "its text there", f.name, n);
                logf_(r, line);
                r->unrecoverable++;
            }
        }
    }
    if (!blf_check(nfs, err, errlen))
        goto done;
    if (!copy_file(path, before)) {
        snprintf(err, (size_t)errlen, "could not keep the old state as %s", before);
        goto done;
    }
    ok = bls_save(path, &fresh, err, errlen);
done:
    blf_close(nfs);
    bls_free(&fresh);
done_old:
    remove(tmp);
    blf_close(ofs);
    bls_free(&old);
    return ok;
}
