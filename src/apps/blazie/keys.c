/* keys.c -- see keys.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keys.h"

static const char *const NAMES[K_NAMED_END - K_ENTER] = {
    "enter", "tab", "backspace", "esc", "up", "down", "left", "right", "home", "end", "pageup", "pagedown",
    "insert", "delete",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
    "lshift", "rshift", "lctrl", "rctrl", "lalt", "ralt", "capslock", "numlock", "scrolllock", "print", "pause",
    "brl_dot1", "brl_dot2", "brl_dot3", "brl_dot4", "brl_dot5", "brl_dot6", "brl_dot7", "brl_dot8",
};

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int same_text(const char *a, const char *b, size_t n)   /* the first n of a equal b, without case */
{
    size_t i;
    if (strlen(b) != n)
        return 0;
    for (i = 0; i < n; i++)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i]))
            return 0;
    return 1;
}

int key_parse(const char *name, int *mods)
{
    int m = 0, k;
    size_t n;
    for (;;) {                                  /* the modifiers, each followed by - */
        if (!strncmp(name, "ctrl-", 5)) { m |= KM_CTRL; name += 5; }
        else if (!strncmp(name, "alt-", 4)) { m |= KM_ALT; name += 4; }
        else if (!strncmp(name, "shift-", 6)) { m |= KM_SHIFT; name += 6; }
        else break;
    }
    if (mods)
        *mods = m;
    n = strlen(name);
    if (n == 1 && (unsigned char)name[0] > ' ' && (unsigned char)name[0] < 0x7F)
        return lower((unsigned char)name[0]);
    if (same_text(name, "space", n))
        return ' ';
    if (n > 5 && !strncmp(name, "code:", 5)) {
        k = atoi(name + 5);
        return k > 0 && k < 0x300 ? K_CODE + k : 0;
    }
    for (k = 0; k < K_NAMED_END - K_ENTER; k++)
        if (same_text(name, NAMES[k], n))
            return K_ENTER + k;
    if (same_text(name, "escape", n))
        return K_ESC;
    if (same_text(name, "return", n))
        return K_ENTER;
    return 0;
}

const char *key_name(int key, char *out)
{
    if (key == ' ')
        return strcpy(out, "space");
    if (key > ' ' && key < 0x7F) {
        out[0] = (char)key;
        out[1] = 0;
        return out;
    }
    if (key >= K_ENTER && key < K_NAMED_END)
        return strcpy(out, NAMES[key - K_ENTER]);
    if (key >= K_CODE) {
        snprintf(out, 16, "code:%d", key - K_CODE);
        return out;
    }
    snprintf(out, 16, "char %d", key);
    return out;
}

int key_fold(int key)
{
    return key < 0x100 ? lower(key) : key;
}

int key_list(const char *setting, key_combo *out, int max)
{
    char tok[64];
    int n = 0;
    const char *p = setting;
    while (*p && n < max) {
        size_t len = 0;
        const char *start;
        int k, m;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        /* names are separated by spaces; "a, ;" is read too (a comma after a name is a separator, a lone one the
           comma key) */
        start = p;
        while (p[len] && p[len] != ' ' && p[len] != '\t')
            len++;
        p += len;
        if (len > 1 && start[len - 1] == ',')
            len--;
        if (len >= sizeof tok)
            len = sizeof tok - 1;
        memcpy(tok, start, len);
        tok[len] = 0;
        k = key_parse(tok, &m);
        if (k) {
            out[n].key = k;
            out[n].mods = m;
            n++;
        }
    }
    return n;
}

int key_in_list(const key_event *e, const key_combo *list, int n)
{
    int key = e->key, mods = e->mods, i;
    if (key >= 'A' && key <= 'Z') {             /* a terminal's capital: the letter with shift */
        key = lower(key);
        mods |= KM_SHIFT;
    }
    for (i = 0; i < n; i++)
        if (list[i].key == key && list[i].mods == mods)
            return 1;
    return 0;
}
