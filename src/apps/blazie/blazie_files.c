/* blazie_files.c -- a saved Braille Lite or Type 'n Speak's files, from the command line (Windows, Linux, the
 * BTSpeak): the same as the emulator's Firmware menu, on a state file the emulator saved (%APPDATA%\ssi263-speech\
 * blazie-emu\english.state, ...).  Close the emulator first, or switch it to another unit: it saves its unit over the
 * file when it closes.
 *
 *   blazie_files list STATE                    the files, their folders and sizes, the free space
 *   blazie_files export STATE IMAGE.img         every file into a FAT disk image (7-Zip opens it; Linux mounts it)
 *   blazie_files import STATE IMAGE.img [--dry-run]   the unit's files made what the image holds (bl_files_xfer.h)
 *   blazie_files extract STATE DIR [--crlf]     every file into DIR/<folder>/<name>; --crlf: text files with CR LF
 *                                               line ends, for a PC editor (the image keeps the unit's CR)
 *   blazie_files check STATE                    the file system's rules, checked
 *   --codepage=852 (anywhere)                   the unit's names in code page 852: the Slovak Braille 'n Speak
 *                                               2000's (its "fles subory" folder); 850 otherwise (bl_files_xfer.h)
 *
 * And, since 7-Zip only reads disk images and Windows does not open one by itself, a way to make and take apart
 * images with a plain folder in between (Linux can also mount the image: mount -o loop):
 *   blazie_files unpack IMAGE.img DIR           the image's folders and files into DIR
 *   blazie_files pack DIR IMAGE.img             DIR's folders and files into a new image (what import reads)
 *
 * Exit status: 0 done, 1 failed, 2 usage.  Build on Linux: cc -O2 -o blazie_files blazie_files.c
 * ../../csrc/blazie/{bl_files,bl_files_state,bl_files_xfer,fat_img}.c
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif
#include "../../csrc/blazie/bl_files.h"
#include "../../csrc/blazie/bl_files_state.h"
#include "../../csrc/blazie/bl_files_xfer.h"
#include "../../csrc/blazie/fat_img.h"

/* ---- the host's folders, names in UTF-8 -------------------------------------------------------------------------- */
typedef struct { char name[768]; int dir; unsigned dos_time, dos_date; } host_entry;

#ifdef _WIN32
static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n > 0 ? n : 1);
    return w;
}

static FILE *open_file(const char *path, const char *mode)
{
    wchar_t *w = wide(path), wm[4] = {0};
    FILE *f;
    int i;
    for (i = 0; i < 3 && mode[i]; i++) wm[i] = (wchar_t)mode[i];
    f = w ? _wfopen(w, wm) : NULL;
    free(w);
    return f;
}

static int make_dir(const char *path)
{
    wchar_t *w = wide(path);
    int ok = w && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    free(w);
    return ok;
}

static int list_dir(const char *dir, host_entry **out)
{
    char pat[1200];
    wchar_t *w;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0, cap = 0;
    *out = NULL;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    w = wide(pat);
    h = w ? FindFirstFileW(w, &fd) : INVALID_HANDLE_VALUE;
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do {
        host_entry *e;
        FILETIME local;
        WORD dd = 0, dt = 0;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (n == cap) {
            host_entry *ne = (host_entry *)realloc(*out, sizeof(host_entry) * (size_t)(cap = cap ? cap * 2 : 32));
            if (!ne) break;
            *out = ne;
        }
        e = &(*out)[n++];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, e->name, sizeof e->name, NULL, NULL);
        e->dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        FileTimeToLocalFileTime(&fd.ftLastWriteTime, &local);
        FileTimeToDosDateTime(&local, &dd, &dt);
        e->dos_date = dd;
        e->dos_time = dt;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
#else
static void dos_of(const struct tm *t, unsigned *dos_time, unsigned *dos_date)
{
    int y = t->tm_year + 1900;
    if (y < 1980) y = 1980;
    if (y > 2107) y = 2107;
    *dos_time = (unsigned)((t->tm_hour << 11) | (t->tm_min << 5) | (t->tm_sec / 2));
    *dos_date = (unsigned)(((y - 1980) << 9) | ((t->tm_mon + 1) << 5) | t->tm_mday);
}

static FILE *open_file(const char *path, const char *mode) { return fopen(path, mode); }
static int make_dir(const char *path) { return mkdir(path, 0777) == 0 || errno == EEXIST; }

static int list_dir(const char *dir, host_entry **out)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0, cap = 0;
    *out = NULL;
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char full[1600];
        struct stat st;
        host_entry *e;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        if (stat(full, &st) != 0)
            continue;
        if (n == cap) {
            host_entry *ne = (host_entry *)realloc(*out, sizeof(host_entry) * (size_t)(cap = cap ? cap * 2 : 32));
            if (!ne) break;
            *out = ne;
        }
        e = &(*out)[n++];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        e->dir = S_ISDIR(st.st_mode);
        dos_of(localtime(&st.st_mtime), &e->dos_time, &e->dos_date);
    }
    closedir(d);
    return n;
}
#endif

static int by_name(const void *a, const void *b)
{
    return strcmp(((const host_entry *)a)->name, ((const host_entry *)b)->name);
}

static const char *const MODEL[] = {"Braille Lite", "Type 'n Speak"};

static int g_cp = BLX_CP850;                /* --codepage=852: the unit's names' code page */

static int usage(void)
{
    printf("usage: blazie_files list|check STATE\n"
           "       blazie_files export STATE IMAGE.img\n"
           "       blazie_files import STATE IMAGE.img [--dry-run]\n"
           "       blazie_files extract STATE DIR [--crlf]\n"
           "       blazie_files unpack IMAGE.img DIR\n"
           "       blazie_files pack DIR IMAGE.img\n"
           "       --codepage=852: the unit's names in code page 852 (the Slovak Braille 'n Speak 2000)\n");
    return 2;
}

static unsigned char *read_file(const char *path, unsigned long *n)
{
    FILE *f = open_file(path, "rb");
    long size;
    unsigned char *d;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (unsigned char *)malloc(size > 0 ? (size_t)size : 1);
    if (d && fread(d, 1, (size_t)size, f) != (size_t)size) { free(d); d = NULL; }
    fclose(f);
    *n = (unsigned long)size;
    return d;
}

static int write_file(const char *path, const unsigned char *d, unsigned long n)
{
    FILE *f = open_file(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static void print_log(const blx_report *r)
{
    if (r->log)
        fputs(r->log, stdout);
}

static int list(const blf_fs *fs, int model)
{
    unsigned long ram_free, flash_free;
    int i, f;
    printf("%s, %s; RAM file system %s, flash %s, folders %s\n", MODEL[model], blf_language(fs) ? "Spanish" : "English",
           blf_ram_ok(fs) ? "set up" : "NOT set up", blf_flash_ok(fs) ? "set up" : "NOT set up",
           blf_folders_ok(fs) ? "set up" : "NOT set up");
    for (f = 0; f < BLF_FOLDERS; f++) {
        const blf_folder *fo = blf_folder_get(fs, f);
        if (fo->name[0])
            printf("folder %d: %s (%s)\n", f, fo->name, fo->type == 'f' ? "flash" : fo->type == 'r' ? "RAM" : "?");
    }
    for (i = 0; i < blf_count(fs); i++) {
        const blf_file *x = blf_get(fs, i);
        printf("%-20s %8lu bytes  %-5s folder %2d  type %c%s%s%s%s\n", x->name, x->size, x->in_flash ? "flash" : "RAM",
               x->folder, x->type ? x->type : '-', x->prot & 2 ? ", protected" : "",
               x->has_password ? ", password" : "", x->system ? ", the unit's own" : "", x->open ? ", open" : "");
    }
    for (i = 0; ; i++) {                     /* flash files a unit without folders lost (bl_files.h) */
        blf_file x;
        unsigned long n;
        if (!blf_lost_get(fs, i, &x, &n))
            break;
        printf("%-20s %8lu bytes  flash LOST: moved to flash by a unit whose folders were never set up\n", x.name, n);
    }
    blf_free_space(fs, &ram_free, &flash_free);
    printf("%d files; free: %lu RAM pages, %lu k flash\n", blf_count(fs), ram_free / 4096, flash_free / 1024);
    if (model == BLF_TYPE_N_SPEAK && (!blf_ram_ok(fs) || !blf_folders_ok(fs)))
        printf("This Type 'n Speak was never set up: the emulator offers to set it up again, keeping its RAM files, "
               "when it next starts it.\n");
    return 0;
}

static int extract(const blf_fs *fs, const char *dir, int crlf)
{
    int i, n = 0;
    char path[1200], u8[200], f8[200];
    if (!make_dir(dir)) {
        printf("cannot make %s\n", dir);
        return 1;
    }
    for (i = 0; i < blf_count(fs); i++) {
        const blf_file *x = blf_get(fs, i);
        const blf_folder *fo = blf_folder_get(fs, x->folder);
        unsigned long len, k, o = 0;
        const unsigned char *d = blf_data(fs, i, &len);
        unsigned char *out = (unsigned char *)malloc(len * 2 + 1);
        if (!out) return 1;
        for (k = 0; k < len; k++) {
            if (crlf && (x->type == 'A' || x->type == 'B') && d[k] == '\r') {
                out[o++] = '\r';
                out[o++] = '\n';
            } else
                out[o++] = d[k];
        }
        blx_unit_to_utf8_cp(g_cp, fo && fo->name[0] ? fo->name : "no folder", f8, sizeof f8);
        snprintf(path, sizeof path, "%s/%s", dir, f8);
        make_dir(path);
        blx_unit_to_utf8_cp(g_cp, x->name, u8, sizeof u8);
        snprintf(path, sizeof path, "%s/%s/%s", dir, f8, u8);
        if (!write_file(path, out, o))
            printf("could not write %s\n", path);
        else
            n++;
        free(out);
    }
    printf("extracted %d of %d files into %s\n", n, blf_count(fs), dir);
    return n == blf_count(fs) ? 0 : 1;
}

/* ---- pack and unpack: an image and a plain folder ------------------------------------------------------------- */
static int pack_dir(fat_builder *b, int parent, const char *dir, int depth, int *files)
{
    host_entry *e;
    int n = list_dir(dir, &e), i, ok = 1;
    if (n < 0) {
        printf("cannot read the folder %s\n", dir);
        return 0;
    }
    qsort(e, (size_t)n, sizeof *e, by_name);   /* the same folder makes the same image */
    for (i = 0; i < n && ok; i++) {
        char full[1600];
        snprintf(full, sizeof full, "%s/%s", dir, e[i].name);
        if (e[i].dir) {
            int id = fat_add_dir(b, parent, e[i].name, e[i].dos_time, e[i].dos_date);
            if (id < 0)
                printf("skipped the folder %s (a name an image cannot hold)\n", full);
            else if (depth < 8)
                ok = pack_dir(b, id, full, depth + 1, files);
        } else {
            unsigned long size;
            unsigned char *d = read_file(full, &size);
            if (!d) {
                printf("cannot read %s\n", full);
                ok = 0;
            } else if (!fat_add_file(b, parent, e[i].name, d, size, e[i].dos_time, e[i].dos_date, 0))
                printf("skipped %s (a name an image cannot hold, or out of memory)\n", full);
            else
                ++*files;
            free(d);
        }
    }
    free(e);
    return ok;
}

static int pack(const char *dir, const char *image)
{
    fat_builder *b = fat_new("BLAZIE");
    char err[300];
    unsigned long size;
    unsigned char *img = NULL;
    int files = 0, ok = b && pack_dir(b, 0, dir, 0, &files);
    if (ok)
        img = fat_build(b, &size, err, sizeof err);
    fat_free(b);
    if (!img || !write_file(image, img, size)) {
        printf("pack failed: %s\n", !ok ? "see above" : img ? "cannot write the image" : err);
        free(img);
        return 1;
    }
    free(img);
    printf("packed %d files into %s\n", files, image);
    return 0;
}

static int unpack(const char *image, const char *dir)
{
    unsigned long size;
    unsigned char *img = read_file(image, &size);
    char err[300], **path;
    fat_volume *v;
    int i, files = 0, rc = 0;
    if (!img) { printf("cannot read %s\n", image); return 1; }
    v = fat_read(img, size, err, sizeof err);
    if (!v) { printf("%s: %s\n", image, err); free(img); return 1; }
    path = (char **)calloc((size_t)fat_count(v) + 1, sizeof *path);
    if (!path || !make_dir(dir)) { printf("cannot make %s\n", dir); rc = 1; }
    for (i = 0; !rc && i < fat_count(v); i++) {
        const fat_entry *e = fat_get(v, i);
        const char *up = e->parent >= 0 ? path[e->parent] : dir;
        size_t len = strlen(up) + strlen(e->name) + 2;
        if (!up || !(path[i] = (char *)malloc(len))) continue;
        snprintf(path[i], len, "%s/%s", up, e->name);
        if (e->dir) {
            if (!make_dir(path[i])) { printf("cannot make %s\n", path[i]); rc = 1; }
        } else {
            unsigned long n;
            unsigned char *d = fat_data(v, i, &n);
            if (!d || !write_file(path[i], d, n)) { printf("cannot write %s\n", path[i]); rc = 1; }
            else files++;
            free(d);
        }
    }
    for (i = 0; path && i < fat_count(v); i++)
        free(path[i]);
    free(path);
    fat_close(v);
    free(img);
    if (!rc)
        printf("unpacked %d files into %s\n", files, dir);
    return rc;
}

int main(int argc, char **argv)
{
    bls_unit u;
    blf_fs *fs;
    char err[300];
    int rc = 0;
    if (getenv("TEST_FILES_FAT_BREAK"))      /* run_tests' must-fail control for the 7-Zip check */
        fat_break = atoi(getenv("TEST_FILES_FAT_BREAK"));
    if (getenv("TEST_FILES_BREAK"))          /* the Linux round trip's control (bl_files.h blf_break) */
        blf_break = atoi(getenv("TEST_FILES_BREAK"));
    {                                        /* --codepage=852, anywhere: taken out of the arguments */
        int i, k = 1;
        for (i = 1; i < argc; i++) {
            if (!strncmp(argv[i], "--codepage=", 11)) {
                int cp = atoi(argv[i] + 11);
                if (cp != BLX_CP850 && cp != BLX_CP852)
                    return usage();
                g_cp = cp;
            } else
                argv[k++] = argv[i];
        }
        argc = k;
    }
    if (argc < 3)
        return usage();
    if (!strcmp(argv[1], "pack") && argc >= 4)
        return pack(argv[2], argv[3]);
    if (!strcmp(argv[1], "unpack") && argc >= 4)
        return unpack(argv[2], argv[3]);
    if (!bls_load(argv[2], &u, err, sizeof err)) {
        printf("%s: %s\n", argv[2], err);
        return 1;
    }
    fs = blf_open(u.model, u.ram, u.flash, u.flash_size, err, sizeof err);
    if (!fs) {
        printf("%s: %s\n", argv[2], err);
        bls_free(&u);
        return 1;
    }
    if (!strcmp(argv[1], "list"))
        rc = list(fs, u.model);
    else if (!strcmp(argv[1], "check")) {
        if (blf_check(fs, err, sizeof err))
            printf("ok: %d files, the file system's rules hold\n", blf_count(fs));
        else {
            printf("BROKEN: %s\n", err);
            rc = 1;
        }
    } else if (!strcmp(argv[1], "export") && argc >= 4) {
        blx_report r;
        unsigned long size;
        unsigned char *img;
        blx_report_init(&r);
        img = blx_export_cp(fs, u.model, g_cp, &size, &r, err, sizeof err);
        if (!img || !write_file(argv[3], img, size)) {
            printf("export failed: %s\n", img ? "cannot write the image" : err);
            rc = 1;
        } else {
            print_log(&r);
            printf("Exported %d files to %s\n", r.exported, argv[3]);
        }
        free(img);
        blx_report_free(&r);
    } else if (!strcmp(argv[1], "import") && argc >= 4) {
        blx_report r;
        unsigned long size;
        unsigned char *img = read_file(argv[3], &size);
        int dry = argc >= 5 && !strcmp(argv[4], "--dry-run");
        blx_report_init(&r);
        if (!img) {
            printf("cannot read %s\n", argv[3]);
            rc = 1;
        } else if (!blx_import_cp(fs, g_cp, img, size, &r, err, sizeof err)) {
            printf("import failed: %s\n", err);
            rc = 1;
        } else {
            print_log(&r);
            if (!blf_check(fs, err, sizeof err)) {
                printf("NOT saved: the result breaks the file system's rules (%s)\n", err);
                rc = 1;
            } else if (dry)
                printf("dry run, nothing saved: ");
            else {
                char before[1200];                /* the unit as it was, kept beside it */
                unsigned long bn;
                unsigned char *old = read_file(argv[2], &bn);
                snprintf(before, sizeof before, "%s.before-import", argv[2]);
                if (!old || !write_file(before, old, bn)) {
                    printf("NOT saved: could not keep the old state as %s\n", before);
                    rc = 1;
                } else if (!bls_save(argv[2], &u, err, sizeof err)) {
                    printf("NOT saved: %s\n", err);
                    rc = 1;
                } else
                    printf("(the unit as it was before is kept in %s)\n", before);
                free(old);
            }
            if (!rc)
                printf("%d added, %d rewritten, %d moved, %d deleted, %d unchanged, %d kept, %d skipped, %d new folders\n",
                       r.added, r.replaced, r.moved, r.deleted, r.unchanged, r.kept, r.skipped, r.folders_added);
        }
        free(img);
        blx_report_free(&r);
    } else if (!strcmp(argv[1], "extract") && argc >= 4)
        rc = extract(fs, argv[3], argc >= 5 && !strcmp(argv[4], "--crlf"));
    else
        rc = usage();
    blf_close(fs);
    bls_free(&u);
    return rc;
}
