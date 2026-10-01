/* fat_img.c -- see fat_img.h.  Written from the FAT on-disk format as Microsoft documents it (FAT: General Overview
 * of On-Disk Format, 1.03): the boot sector's fields, the FAT12/16/32 cluster-count rule, 8.3 entries, long-name
 * entries and their checksum. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fat_img.h"

#define SECTOR 512UL
#define ROOT_ENTRIES 512UL
#define ROOT_SECTORS (ROOT_ENTRIES * 32 / SECTOR)
#define MAX_DEPTH 16
#define SERIAL 0x0B1A2E00UL             /* fixed: the same files make the same image */

int fat_break;

static int fail(char *err, int errlen, const char *msg)
{
    if (err && errlen > 0)
        snprintf(err, (size_t)errlen, "%s", msg);
    return 0;
}

static void wr16(unsigned char *p, unsigned long v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, unsigned long v) { wr16(p, v); wr16(p + 2, v >> 16); }
static unsigned long rd16(const unsigned char *p) { return (unsigned long)p[0] | ((unsigned long)p[1] << 8); }
static unsigned long rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }

/* ---- UTF-8 <-> UTF-16 ------------------------------------------------------------------------------------------ */
/* the next code point of s (advanced); a byte that is not UTF-8 counts as U+FFFD */
static unsigned long utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned long c = p[0];
    int k, n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n < 0) { (*s)++; return 0xFFFD; }
    c &= n ? (0x3F >> n) : 0x7F;
    for (k = 1; k <= n; k++) {
        if ((p[k] & 0xC0) != 0x80) { (*s)++; return 0xFFFD; }
        c = (c << 6) | (p[k] & 0x3F);
    }
    *s += n + 1;
    return c;
}

/* s as UTF-16 into out (at most cap units); the count */
static int to_utf16(const char *s, unsigned short *out, int cap)
{
    int n = 0;
    while (*s && n < cap) {
        unsigned long c = utf8_next(&s);
        if (c >= 0x10000 && n + 2 <= cap) {
            c -= 0x10000;
            out[n++] = (unsigned short)(0xD800 | (c >> 10));
            out[n++] = (unsigned short)(0xDC00 | (c & 0x3FF));
        } else if (c < 0x10000)
            out[n++] = (unsigned short)c;
        else
            break;
    }
    return n;
}

static void put_utf8(char **o, char *end, unsigned long c)
{
    char *p = *o;
    int n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4, k;
    if (p + n >= end) return;
    if (n == 1) { *p = (char)c; *o = p + 1; return; }
    for (k = n - 1; k > 0; k--) { p[k] = (char)(0x80 | (c & 0x3F)); c >>= 6; }
    p[0] = (char)((n == 2 ? 0xC0 : n == 3 ? 0xE0 : 0xF0) | c);
    *o = p + n;
}

/* ---- writing ------------------------------------------------------------------------------------------------- */
typedef struct {
    char *name;
    int dir, parent, read_only;
    unsigned char *data;
    unsigned long n;
    unsigned t, d;
    unsigned char sfn[11];
    unsigned long first, clusters, entries;
} node;

struct fat_builder {
    char label[12];
    node *nodes;                    /* nodes[0]: the root */
    int n, cap;
};

static unsigned char upper_ascii(unsigned long c)
{
    return (unsigned char)(c >= 'a' && c <= 'z' ? c - 32 : c);
}

static int sfn_char_ok(unsigned long c)    /* a character an 8.3 name may hold (upper-cased) */
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && c < 0x80 && strchr("!#$%&'()-@^_`{}~", (int)c));
}

static int name_eq(const char *a, const char *b)   /* case-insensitive for ASCII, exact otherwise */
{
    for (; *a && *b; a++, b++)
        if (upper_ascii((unsigned char)*a) != upper_ascii((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int name_ok(const char *name)
{
    size_t n = strlen(name), i;
    unsigned short u[300];
    if (!n || !strcmp(name, ".") || !strcmp(name, "..") || name[n - 1] == ' ' || name[n - 1] == '.')
        return 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)name[i] < 0x20 || strchr("\\/:*?\"<>|", name[i]))
            return 0;
    return to_utf16(name, u, 300) <= 255;
}

static int add_node(fat_builder *b, int parent, const char *name, int dir)
{
    int i;
    node *x;
    if (parent < 0 || parent >= b->n || !b->nodes[parent].dir || !name_ok(name))
        return -1;
    for (i = 1; i < b->n; i++)
        if (b->nodes[i].parent == parent && name_eq(b->nodes[i].name, name))
            return -1;
    if (b->n == b->cap) {
        int cap = b->cap * 2;
        node *nn = (node *)realloc(b->nodes, sizeof(node) * (size_t)cap);
        if (!nn) return -1;
        b->nodes = nn;
        b->cap = cap;
    }
    x = &b->nodes[b->n];
    memset(x, 0, sizeof *x);
    x->name = (char *)malloc(strlen(name) + 1);
    if (!x->name) return -1;
    strcpy(x->name, name);
    x->dir = dir;
    x->parent = parent;
    return b->n++;
}

fat_builder *fat_new(const char *label)
{
    fat_builder *b = (fat_builder *)calloc(1, sizeof *b);
    int i;
    if (!b) return NULL;
    b->cap = 64;
    b->nodes = (node *)calloc((size_t)b->cap, sizeof(node));
    if (!b->nodes) { free(b); return NULL; }
    b->nodes[0].dir = 1;
    b->n = 1;
    if (label)
        for (i = 0; i < 11 && label[i]; i++)
            b->label[i] = (char)upper_ascii((unsigned char)label[i]);
    return b;
}

void fat_free(fat_builder *b)
{
    int i;
    if (!b) return;
    for (i = 0; i < b->n; i++) {
        free(b->nodes[i].name);
        free(b->nodes[i].data);
    }
    free(b->nodes);
    free(b);
}

int fat_add_dir(fat_builder *b, int parent, const char *name, unsigned dos_time, unsigned dos_date)
{
    int i = add_node(b, parent, name, 1);
    if (i < 0) return -1;
    b->nodes[i].t = dos_time;
    b->nodes[i].d = dos_date;
    return i;
}

int fat_add_file(fat_builder *b, int parent, const char *name, const unsigned char *data, unsigned long n,
                 unsigned dos_time, unsigned dos_date, int read_only)
{
    int i = add_node(b, parent, name, 0);
    node *x;
    if (i < 0) return 0;
    x = &b->nodes[i];
    x->data = (unsigned char *)malloc(n ? n : 1);
    if (!x->data) { free(x->name); b->n--; return 0; }
    memcpy(x->data, data, n);
    x->n = n;
    x->t = dos_time;
    x->d = dos_date;
    x->read_only = read_only;
    return 1;
}

static int sfn_taken(const fat_builder *b, int self, const unsigned char *sfn)
{
    int i;
    for (i = 1; i < self; i++)
        if (b->nodes[i].parent == b->nodes[self].parent && !memcmp(b->nodes[i].sfn, sfn, 11))
            return 1;
    return 0;
}

/* the 8.3 alias: the name itself when it is a valid 8.3 name (any case), else BASE~N.EXT as Windows makes them */
static void make_sfn(fat_builder *b, int self)
{
    const char *name = b->nodes[self].name, *s, *dot = strrchr(name, '.');
    unsigned char base[8], ext[3], sfn[11];
    int nb = 0, ne = 0, plain = 1, k;
    long seq;
    char tail[12];
    if (dot == name)
        dot = NULL;                 /* ".abc": no extension */
    for (s = name; *s; ) {
        const char *at = s;
        unsigned long c = utf8_next(&s);
        int in_ext = dot && at > dot;
        unsigned char u = c < 0x80 ? upper_ascii(c) : '_';
        if (at == dot) continue;
        if (c == ' ' || c == '.' ) { plain = 0; continue; }
        if (c >= 0x80 || !sfn_char_ok(u)) { plain = 0; u = '_'; }
        if (in_ext) { if (ne < 3) ext[ne++] = u; else plain = 0; }
        else { if (nb < 8) base[nb++] = u; else plain = 0; }
    }
    if (!nb) { plain = 0; base[nb++] = '_'; }
    memset(sfn, ' ', 11);
    memcpy(sfn, base, (size_t)nb);
    memcpy(sfn + 8, ext, (size_t)ne);
    if (plain && !sfn_taken(b, self, sfn)) {
        memcpy(b->nodes[self].sfn, sfn, 11);
        return;
    }
    for (seq = 1; seq < 1000000; seq++) {
        int nt = snprintf(tail, sizeof tail, "~%ld", seq), keep = nb < 8 - nt ? nb : 8 - nt;
        memset(sfn, ' ', 8);
        memcpy(sfn, base, (size_t)keep);
        for (k = 0; k < nt; k++)
            sfn[keep + k] = (unsigned char)tail[k];
        if (!sfn_taken(b, self, sfn))
            break;
    }
    memcpy(b->nodes[self].sfn, sfn, 11);
}

static unsigned char sfn_sum(const unsigned char *sfn)
{
    unsigned char sum = 0;
    int i;
    for (i = 0; i < 11; i++)
        sum = (unsigned char)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
    return sum;
}

static int lfn_entries(const char *name)
{
    unsigned short u[300];
    return (to_utf16(name, u, 300) + 12) / 13;
}

static void put_sfn(unsigned char *e, const unsigned char *sfn, int attr, unsigned t, unsigned d, unsigned long first,
                    unsigned long size)
{
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = (unsigned char)attr;
    wr16(e + 14, t);
    wr16(e + 16, d);
    wr16(e + 18, d);
    wr16(e + 22, t);
    wr16(e + 24, d);
    wr16(e + 26, first);
    wr32(e + 28, size);
}

/* a node's entries (its long name, then its 8.3 entry) at p; the next free entry */
static unsigned char *put_node(unsigned char *p, const node *x)
{
    unsigned short u[300];
    int n = to_utf16(x->name, u, 300), parts = (n + 12) / 13, k, j;
    unsigned char sum = (unsigned char)(sfn_sum(x->sfn) ^ (fat_break == 1 ? 0x5A : 0));
    static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (k = parts; k >= 1; k--) {
        memset(p, 0, 32);
        p[0] = (unsigned char)(k | (k == parts ? 0x40 : 0));
        p[11] = 0x0F;
        p[13] = sum;
        for (j = 0; j < 13; j++) {
            int at = (k - 1) * 13 + j;
            unsigned long c = at < n ? u[at] : at == n ? 0 : 0xFFFF;
            wr16(p + pos[j], c);
        }
        p += 32;
    }
    put_sfn(p, x->sfn, x->dir ? 0x10 : 0x20 | (x->read_only ? 0x01 : 0), x->t, x->d, x->first, x->dir ? 0 : x->n);
    return p + 32;
}

unsigned char *fat_build(fat_builder *b, unsigned long *size, char *err, int errlen)
{
    unsigned long total = 16384, spc = 2, fat = 1, clusters = 0, need = 0, cb, next, i, data_at;
    unsigned char *img, *p;
    int k;
    for (k = 1; k < b->n; k++)
        make_sfn(b, k);
    for (k = 0; k < b->n; k++)
        b->nodes[k].entries = k ? 2 : (b->label[0] ? 1 : 0);
    for (k = 1; k < b->n; k++)
        b->nodes[b->nodes[k].parent].entries += (unsigned long)lfn_entries(b->nodes[k].name) + 1;
    if (b->nodes[0].entries > ROOT_ENTRIES) {
        fail(err, errlen, "too many folders and files at the top of the image");
        return NULL;
    }
    for (;;) {                      /* the geometry: FAT16, clusters for everything with room to spare */
        for (spc = 2; total / spc > 65000; spc *= 2) ;
        cb = spc * SECTOR;
        for (fat = 1;;) {
            unsigned long c = (total - 1 - 2 * fat - ROOT_SECTORS) / spc, f = ((c + 2) * 2 + SECTOR - 1) / SECTOR;
            if (f <= fat) { clusters = c; break; }
            fat = f;
        }
        need = 0;
        for (k = 1; k < b->n; k++) {
            node *x = &b->nodes[k];
            x->clusters = x->dir ? (x->entries * 32 + cb - 1) / cb : (x->n + cb - 1) / cb;
            need += x->clusters;
        }
        if (need + need / 2 + 64 <= clusters && clusters >= 4085)
            break;
        if (total >= 4194304UL) {
            fail(err, errlen, "the files are too large for one image");
            return NULL;
        }
        total *= 2;
    }
    img = (unsigned char *)calloc(total, SECTOR);
    if (!img) {
        fail(err, errlen, "out of memory");
        return NULL;
    }
    /* the boot sector */
    p = img;
    p[0] = 0xEB; p[1] = 0x3C; p[2] = 0x90;
    memcpy(p + 3, "MSWIN4.1", 8);
    wr16(p + 11, SECTOR);
    p[13] = (unsigned char)spc;
    wr16(p + 14, 1);
    p[16] = 2;
    wr16(p + 17, ROOT_ENTRIES);
    if (total < 65536) wr16(p + 19, total); else wr32(p + 32, total);
    p[21] = 0xF8;
    wr16(p + 22, fat);
    wr16(p + 24, 32);
    wr16(p + 26, 64);
    p[36] = 0x80;
    p[38] = 0x29;
    wr32(p + 39, SERIAL);
    memset(p + 43, ' ', 11);
    if (b->label[0]) memcpy(p + 43, b->label, strlen(b->label)); else memcpy(p + 43, "NO NAME", 7);
    memcpy(p + 54, "FAT16   ", 8);
    p[62] = 0xFA; p[63] = 0xF4; p[64] = 0xEB; p[65] = 0xFD;   /* not bootable: cli, hlt, loop */
    p[510] = 0x55; p[511] = 0xAA;
    /* clusters, in order */
    next = 2;
    for (k = 1; k < b->n; k++) {
        node *x = &b->nodes[k];
        x->first = x->clusters ? next : 0;
        next += x->clusters;
    }
    /* the two FATs */
    for (i = 0; i < 2; i++) {
        unsigned char *f = img + (1 + i * fat) * SECTOR;
        wr16(f, 0xFFF8);
        wr16(f + 2, 0xFFFF);
        for (k = 1; k < b->n; k++) {
            const node *x = &b->nodes[k];
            unsigned long c;
            for (c = 0; c < x->clusters; c++)
                wr16(f + (x->first + c) * 2, c + 1 < x->clusters ? x->first + c + 1 : 0xFFFF);
        }
    }
    /* the folders' entries, then the files' bytes */
    data_at = (1 + 2 * fat + ROOT_SECTORS) * SECTOR;
    for (k = 0; k < b->n; k++) {
        const node *x = &b->nodes[k];
        int c;
        if (!x->dir) {
            if (x->n)
                memcpy(img + data_at + (x->first - 2) * cb, x->data, x->n);
            continue;
        }
        if (k == 0) {
            p = img + (1 + 2 * fat) * SECTOR;
            if (b->label[0]) {
                unsigned char lab[11];
                memset(lab, ' ', 11);
                memcpy(lab, b->label, strlen(b->label));
                put_sfn(p, lab, 0x08, 0, 0, 0, 0);
                p += 32;
            }
        } else {
            unsigned char dots[11];
            p = img + data_at + (x->first - 2) * cb;
            memset(dots, ' ', 11);
            dots[0] = '.';
            put_sfn(p, dots, 0x10, x->t, x->d, x->first, 0);
            dots[1] = '.';
            put_sfn(p + 32, dots, 0x10, x->t, x->d, x->parent ? b->nodes[x->parent].first : 0, 0);
            p += 64;
        }
        for (c = 1; c < b->n; c++)
            if (b->nodes[c].parent == k)
                p = put_node(p, &b->nodes[c]);
    }
    *size = total * SECTOR;
    return img;
}

/* ---- reading ------------------------------------------------------------------------------------------------- */
struct fat_volume {
    const unsigned char *img;
    unsigned long size, base, bps, spc, fat_at, root_at, root_entries, data_at, clusters, root_cluster;
    int bits;                       /* 12, 16, 32 */
    fat_entry *e;
    unsigned long *first;
    int n, cap;
};

static unsigned long next_cluster(const fat_volume *v, unsigned long c)
{
    unsigned long off;
    if (v->bits == 12) {
        unsigned long x;
        off = v->fat_at + c + c / 2;
        if (off + 2 > v->size) return 0x0FFFFFFF;
        x = rd16(v->img + off);
        x = c & 1 ? x >> 4 : x & 0xFFF;
        return x >= 0xFF7 ? 0x0FFFFFFF : x;
    }
    if (v->bits == 16) {
        unsigned long x;
        off = v->fat_at + c * 2;
        if (off + 2 > v->size) return 0x0FFFFFFF;
        x = rd16(v->img + off);
        return x >= 0xFFF7 ? 0x0FFFFFFF : x;
    }
    off = v->fat_at + c * 4;
    if (off + 4 > v->size) return 0x0FFFFFFF;
    {
        unsigned long x = rd32(v->img + off) & 0x0FFFFFFF;
        return x >= 0x0FFFFFF7 ? 0x0FFFFFFF : x;
    }
}

static const unsigned char *cluster_at(const fat_volume *v, unsigned long c)
{
    unsigned long off;
    if (c < 2 || c >= v->clusters + 2) return NULL;
    off = v->data_at + (c - 2) * v->spc * v->bps;
    return off + v->spc * v->bps <= v->size ? v->img + off : NULL;
}

/* a chain's bytes, up to `limit` (0: the whole chain); NULL if it breaks before limit */
static unsigned char *read_chain(const fat_volume *v, unsigned long c, unsigned long limit, unsigned long *got)
{
    unsigned long cb = v->spc * v->bps, cap = limit ? limit : cb * 16, n = 0, steps = 0;
    unsigned char *buf = (unsigned char *)malloc(cap ? cap : 1);
    if (!buf) return NULL;
    while ((!limit || n < limit) && steps++ <= v->clusters) {
        const unsigned char *src = cluster_at(v, c);
        unsigned long take = cb;
        if (!src) break;
        if (limit && take > limit - n) take = limit - n;
        if (n + take > cap) {
            unsigned char *nb;
            cap = (n + take) * 2;
            nb = (unsigned char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        memcpy(buf + n, src, take);
        n += take;
        c = next_cluster(v, c);
        if (c == 0x0FFFFFFF) break;
    }
    if (limit && n < limit) { free(buf); return NULL; }
    *got = n;
    return buf;
}

static int push_entry(fat_volume *v, const fat_entry *e, unsigned long first)
{
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 64;
        fat_entry *ne = (fat_entry *)realloc(v->e, sizeof(fat_entry) * (size_t)cap);
        unsigned long *nf;
        if (!ne) return 0;
        v->e = ne;
        nf = (unsigned long *)realloc(v->first, sizeof(unsigned long) * (size_t)cap);
        if (!nf) return 0;
        v->first = nf;
        v->cap = cap;
    }
    v->e[v->n] = *e;
    v->first[v->n] = first;
    v->n++;
    return 1;
}

static void sfn_name(const unsigned char *e, char *out, size_t cap)
{
    char *o = out, *end = out + cap;
    int i, last;
    for (last = 7; last >= 0 && e[last] == ' '; last--) ;
    for (i = 0; i <= last; i++) {
        unsigned char c = i == 0 && e[0] == 0x05 ? 0xE5 : e[i];
        if ((e[12] & 0x08) && c >= 'A' && c <= 'Z') c += 32;
        put_utf8(&o, end, c < 0x80 ? c : '_');
    }
    for (last = 10; last >= 8 && e[last] == ' '; last--) ;
    if (last >= 8) put_utf8(&o, end, '.');
    for (i = 8; i <= last; i++) {
        unsigned char c = e[i];
        if ((e[12] & 0x10) && c >= 'A' && c <= 'Z') c += 32;
        put_utf8(&o, end, c < 0x80 ? c : '_');
    }
    *o = 0;
}

static int walk(fat_volume *v, const unsigned char *d, unsigned long n_entries, int parent, int depth)
{
    unsigned short lfn[20 * 13];
    int lfn_parts = 0, lfn_sum = -1;
    unsigned seen = 0;
    unsigned long i;
    for (i = 0; i < n_entries; i++) {
        const unsigned char *e = d + i * 32;
        int attr = e[11];
        fat_entry fe;
        unsigned long first;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) { lfn_parts = 0; continue; }
        if ((attr & 0x3F) == 0x0F) {
            int ord = e[0] & 0x1F, j;
            static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            if (e[0] & 0x40) { lfn_parts = ord; lfn_sum = e[13]; seen = 0; }
            if (ord < 1 || ord > 20 || ord > lfn_parts || e[13] != lfn_sum) { lfn_parts = 0; continue; }
            for (j = 0; j < 13; j++)
                lfn[(ord - 1) * 13 + j] = (unsigned short)rd16(e + pos[j]);
            seen |= 1u << (ord - 1);
            continue;
        }
        if (attr & 0x08) { lfn_parts = 0; continue; }   /* the volume label */
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) { lfn_parts = 0; continue; }
        memset(&fe, 0, sizeof fe);
        if (lfn_parts && seen == (1u << lfn_parts) - 1 && sfn_sum(e) == lfn_sum) {
            char *o = fe.name, *end = fe.name + sizeof fe.name;
            int j;
            for (j = 0; j < lfn_parts * 13 && lfn[j] && lfn[j] != 0xFFFF; j++) {
                unsigned long c = lfn[j];
                if (c >= 0xD800 && c < 0xDC00 && j + 1 < lfn_parts * 13 && lfn[j + 1] >= 0xDC00 && lfn[j + 1] < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lfn[j + 1] - 0xDC00);
                    j++;
                }
                put_utf8(&o, end, c);
            }
            *o = 0;
        } else
            sfn_name(e, fe.name, sizeof fe.name);
        lfn_parts = 0;
        fe.dir = (attr & 0x10) != 0;
        fe.parent = parent;
        fe.attr = attr;
        fe.size = fe.dir ? 0 : rd32(e + 28);
        fe.dos_time = (unsigned)rd16(e + 22);
        fe.dos_date = (unsigned)rd16(e + 24);
        first = rd16(e + 26) | (v->bits == 32 ? rd16(e + 20) << 16 : 0);
        if (!fe.name[0]) continue;
        if (!push_entry(v, &fe, first)) return 0;
        if (fe.dir && first && depth < MAX_DEPTH && v->n < 65536) {
            unsigned long got;
            unsigned char *sub = read_chain(v, first, 0, &got);
            int me = v->n - 1, ok;
            if (!sub) continue;
            ok = walk(v, sub, got / 32, me, depth + 1);
            free(sub);
            if (!ok) return 0;
        }
    }
    return 1;
}

fat_volume *fat_read(const unsigned char *img, unsigned long size, char *err, int errlen)
{
    fat_volume *v;
    unsigned long base = 0, bps, spc, reserved, nfats, root_entries, total, fat_size, root_sectors, data_sectors;
    const unsigned char *b;
    int ok = 1;
    if (size < SECTOR) { fail(err, errlen, "not a disk image (too short)"); return NULL; }
    b = img;
    bps = rd16(b + 11);
    if (!(bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) || !b[13] || !rd16(b + 14) || !b[16]) {
        int k;                      /* not a FAT boot sector: a partition table's first FAT partition? */
        for (k = 0; k < 4 && img[510] == 0x55 && img[511] == 0xAA; k++) {
            const unsigned char *pe = img + 446 + k * 16;
            if (pe[4] == 0x01 || pe[4] == 0x04 || pe[4] == 0x06 || pe[4] == 0x0B || pe[4] == 0x0C || pe[4] == 0x0E) {
                base = rd32(pe + 8) * SECTOR;
                break;
            }
        }
        if (!base || base + SECTOR > size) { fail(err, errlen, "not a FAT disk image"); return NULL; }
        b = img + base;
        bps = rd16(b + 11);
        if (!(bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) || !b[13] || !rd16(b + 14) || !b[16]) {
            fail(err, errlen, "not a FAT disk image");
            return NULL;
        }
    }
    spc = b[13];
    reserved = rd16(b + 14);
    nfats = b[16];
    root_entries = rd16(b + 17);
    total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    fat_size = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
    root_sectors = (root_entries * 32 + bps - 1) / bps;
    if (!fat_size || total <= reserved + nfats * fat_size + root_sectors) { fail(err, errlen, "not a FAT disk image"); return NULL; }
    data_sectors = total - reserved - nfats * fat_size - root_sectors;
    v = (fat_volume *)calloc(1, sizeof *v);
    if (!v) { fail(err, errlen, "out of memory"); return NULL; }
    v->img = img;
    v->size = size;
    v->base = base;
    v->bps = bps;
    v->spc = spc;
    v->clusters = data_sectors / spc;
    v->bits = v->clusters < 4085 ? 12 : v->clusters < 65525 ? 16 : 32;
    v->fat_at = base + reserved * bps;
    v->root_at = v->fat_at + nfats * fat_size * bps;
    v->root_entries = root_entries;
    v->data_at = v->root_at + root_sectors * bps;
    if (v->bits == 32) {
        unsigned long got;
        unsigned char *root;
        v->root_cluster = rd32(b + 44);
        root = read_chain(v, v->root_cluster, 0, &got);
        if (!root) { fat_close(v); fail(err, errlen, "the image's root folder is damaged"); return NULL; }
        ok = walk(v, root, got / 32, -1, 0);
        free(root);
    } else {
        if (v->root_at + root_entries * 32 > size) { fat_close(v); fail(err, errlen, "the image is cut short"); return NULL; }
        ok = walk(v, img + v->root_at, root_entries, -1, 0);
    }
    if (!ok) { fat_close(v); fail(err, errlen, "out of memory"); return NULL; }
    return v;
}

int fat_count(const fat_volume *v) { return v->n; }
const fat_entry *fat_get(const fat_volume *v, int i) { return i >= 0 && i < v->n ? &v->e[i] : NULL; }

unsigned char *fat_data(const fat_volume *v, int i, unsigned long *n)
{
    unsigned long got;
    unsigned char *d;
    if (i < 0 || i >= v->n || v->e[i].dir) return NULL;
    *n = v->e[i].size;
    if (!*n) return (unsigned char *)calloc(1, 1);
    d = read_chain(v, v->first[i], *n, &got);
    return d;
}

void fat_close(fat_volume *v)
{
    if (!v) return;
    free(v->e);
    free(v->first);
    free(v);
}
