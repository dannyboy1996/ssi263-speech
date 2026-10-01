/* ini.c -- see ini.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ini.h"

struct ini {
    char **lines;
    int n, cap;
};

static char *dup_text(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static int add_line(ini *f, int at, const char *text)
{
    char *t;
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 32;
        char **l = (char **)realloc(f->lines, sizeof(char *) * (size_t)cap);
        if (!l)
            return 0;
        f->lines = l;
        f->cap = cap;
    }
    if (!(t = dup_text(text)))
        return 0;
    memmove(f->lines + at + 1, f->lines + at, sizeof(char *) * (size_t)(f->n - at));
    f->lines[at] = t;
    f->n++;
    return 1;
}

ini *ini_load(const char *path)
{
    ini *f = (ini *)calloc(1, sizeof(ini));
    FILE *fp;
    char line[1024];
    if (!f)
        return NULL;
    if (path && (fp = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof line, fp)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = 0;
            add_line(f, f->n, line);
        }
        fclose(fp);
    }
    return f;
}

void ini_free(ini *f)
{
    int i;
    if (!f)
        return;
    for (i = 0; i < f->n; i++)
        free(f->lines[i]);
    free(f->lines);
    free(f);
}

static const char *skip_space(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* a [section] line: 1 and its name in name */
static int section_of(const char *line, char *name, int cap)
{
    const char *s = skip_space(line), *e;
    int n;
    if (*s != '[' || !(e = strchr(s, ']')))
        return 0;
    n = (int)(e - s - 1);
    if (n >= cap)
        n = cap - 1;
    memcpy(name, s + 1, (size_t)n);
    name[n] = 0;
    return 1;
}

/* a key = value line: 1, the key and the value (both trimmed) */
static int setting_of(const char *line, char *key, int key_cap, char *value, int value_cap)
{
    const char *s = skip_space(line), *eq = strchr(s, '=');
    int n;
    if (!*s || *s == ';' || *s == '#' || *s == '[' || !eq)
        return 0;
    n = (int)(eq - s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    if (n <= 0)
        return 0;
    if (n >= key_cap)
        n = key_cap - 1;
    memcpy(key, s, (size_t)n);
    key[n] = 0;
    s = skip_space(eq + 1);
    n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    if (n >= value_cap)
        n = value_cap - 1;
    memcpy(value, s, (size_t)n);
    value[n] = 0;
    return 1;
}

/* the line of section/key (-1: none); *end: where the section ends (or -1 when there is no such section) */
static int find(const ini *f, const char *section, const char *key, int *end)
{
    char sec[128], k[128], v[8];
    int i, in = 0, found = -1;
    *end = -1;
    for (i = 0; i < f->n; i++) {
        if (section_of(f->lines[i], sec, sizeof sec)) {
            in = !strcmp(sec, section);
            if (in && *end < 0)
                *end = i + 1;
            continue;
        }
        if (in) {
            if (*skip_space(f->lines[i]))
                *end = i + 1;                   /* after the section's last line that is not blank */
            if (key && setting_of(f->lines[i], k, sizeof k, v, sizeof v) && !strcmp(k, key))
                found = i;
        }
    }
    return found;
}

const char *ini_get(const ini *f, const char *section, const char *key, const char *dflt)
{
    static char value[1024];
    char k[128];
    int end, i = find(f, section, key, &end);
    if (i < 0 || !setting_of(f->lines[i], k, sizeof k, value, sizeof value))
        return dflt;
    return value;
}

int ini_get_int(const ini *f, const char *section, const char *key, int dflt)
{
    const char *v = ini_get(f, section, key, NULL);
    return v && *v ? atoi(v) : dflt;
}

void ini_set(ini *f, const char *section, const char *key, const char *value)
{
    char line[1200];
    int end, i = find(f, section, key, &end);
    snprintf(line, sizeof line, "%s = %s", key, value);
    if (i >= 0) {
        char *t = dup_text(line);
        if (t) {
            free(f->lines[i]);
            f->lines[i] = t;
        }
        return;
    }
    if (end < 0) {                              /* a new section at the end */
        char head[160];
        if (f->n && f->lines[f->n - 1][0])
            add_line(f, f->n, "");
        snprintf(head, sizeof head, "[%s]", section);
        add_line(f, f->n, head);
        end = f->n;
    }
    add_line(f, end, line);
}

int ini_entry(const ini *f, const char *section, int index, char *key, int key_cap, char *value, int value_cap)
{
    char sec[128] = "";
    int i, in = section[0] == 0;
    for (i = 0; i < f->n; i++) {
        if (section_of(f->lines[i], sec, sizeof sec)) {
            in = !strcmp(sec, section);
            continue;
        }
        if (in && setting_of(f->lines[i], key, key_cap, value, value_cap) && index-- == 0)
            return 1;
    }
    return 0;
}

int ini_save(const ini *f, const char *path)
{
    FILE *fp = fopen(path, "w");
    int i, ok;
    if (!fp)
        return 0;
    for (i = 0; i < f->n; i++)
        fprintf(fp, "%s\n", f->lines[i]);
    ok = !ferror(fp);
    return fclose(fp) == 0 && ok;
}
