/* tns_term.c -- see tns_term.h. */
#include <string.h>
#include "tns_term.h"
#include "tns_keys.h"

/* a US keyboard's shifted characters and the key under each */
static const char SHIFTED[] = "!@#$%^&*()_+{}|:\"<>?~";
static const char UNSHIFTED[] = "1234567890-=[]\\;',./`";

int tns_code_of(int key)
{
    char name[16];
    if (key == K_LCTRL || key == K_RCTRL)
        return tns_code_named("ctrl");
    if (key >= 'A' && key <= 'Z')
        key = key - 'A' + 'a';
    return tns_code_named(key_name(key, name));
}

int tns_press_codes(const key_event *e, unsigned char *codes, int cap)
{
    int key = e->key, mods = e->mods, code, n = 0, i;
    unsigned char mod_codes[3];
    int n_mod = 0;
    const char *s;
    if (key >= 'A' && key <= 'Z') {
        key = key - 'A' + 'a';
        mods |= KM_SHIFT;
    } else if (key > ' ' && key < 0x7F && (s = strchr(SHIFTED, key)) != NULL) {
        key = UNSHIFTED[s - SHIFTED];
        mods |= KM_SHIFT;
    }
    code = tns_code_of(key);
    if (!code || cap < 8)
        return 0;
    if (mods & KM_CTRL) mod_codes[n_mod++] = (unsigned char)tns_code_named("ctrl");
    if (mods & KM_ALT) mod_codes[n_mod++] = (unsigned char)tns_code_named("lalt");
    if (mods & KM_SHIFT) mod_codes[n_mod++] = (unsigned char)tns_code_named("lshift");
    for (i = 0; i < n_mod; i++)
        codes[n++] = mod_codes[i];
    codes[n++] = (unsigned char)code;
    codes[n++] = (unsigned char)(code & 0x7F);
    for (i = n_mod - 1; i >= 0; i--)
        codes[n++] = (unsigned char)(mod_codes[i] & 0x7F);
    return n;
}
