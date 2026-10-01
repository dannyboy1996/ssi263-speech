/* tns_keys.h -- the Type 'n Speak's keyboard: each key's code (tns_board.h: the down code, bit 7 set; the up code is
 * the same without bit 7), by the key's name (keys.h's names; "ctrl" for either Ctrl key).  The codes were measured
 * by pressing each key on the running firmware (investigation notes); a key the unit does not have is not here.
 *
 * One table for every shell: tns_keymap_win.c (a Windows virtual key) and tns_term.c (a terminal's or an input
 * device's key) both look the codes up here.  Header only, so the Windows build needs no new source file.
 */
#ifndef BLAZIE_TNS_KEYS_H
#define BLAZIE_TNS_KEYS_H

#include <string.h>

typedef struct { const char *name; unsigned char code; } tns_key_code;

static const tns_key_code TNS_KEYS[] = {
    {"a", 0x94}, {"b", 0xB3}, {"c", 0xB2}, {"d", 0xB4}, {"e", 0xB0}, {"f", 0xBC}, {"g", 0xC0}, {"h", 0xC8},
    {"i", 0xCD}, {"j", 0xC4}, {"k", 0xCC}, {"l", 0xD4}, {"m", 0xC3}, {"n", 0xBB}, {"o", 0xD5}, {"p", 0xDD},
    {"q", 0x90}, {"r", 0xB8}, {"s", 0xAC}, {"t", 0xB5}, {"u", 0xC5}, {"v", 0xAB}, {"w", 0x98}, {"x", 0xAA},
    {"y", 0xBD}, {"z", 0x92},
    {"1", 0x8B}, {"2", 0x8D}, {"3", 0x95}, {"4", 0xAD}, {"5", 0x97}, {"6", 0xAF}, {"7", 0xB7}, {"8", 0xBF},
    {"9", 0xC7}, {"0", 0xCF},
    {"space", 0xA9}, {".", 0xC2}, {",", 0xCB}, {"/", 0xCA}, {";", 0xDC},
    {"'", 0xD3}, {"-", 0xD7}, {"=", 0xDF}, {"[", 0xD0}, {"]", 0xD8},
    {"\\", 0xE0},
    {"f1", 0x8A}, {"f2", 0x8C}, {"f3", 0x8F}, {"f4", 0x8E}, {"f5", 0x96}, {"f6", 0xAE}, {"f7", 0xB6},
    {"f8", 0xBE}, {"f9", 0xC6}, {"f10", 0xCE},
    {"ctrl", 0x81}, {"capslock", 0x93},
    {"esc", 0x89}, {"delete", 0xC9}, {"insert", 0xC1}, {"enter", 0xDB}, {"tab", 0x91}, {"backspace", 0xED},
    {"numlock", 0xD6}, {"scrolllock", 0xDE}, {"print", 0xEE}, {"pause", 0xEF},
    {"left", 0xD1}, {"right", 0xE9}, {"up", 0xDA}, {"down", 0xD9}, {"pageup", 0xEC}, {"pagedown", 0xEB},
    {"home", 0xF0}, {"end", 0xEA},
    {"lshift", 0xE1}, {"rshift", 0xE2}, {"lalt", 0xA1}, {"ralt", 0xB9},
};

/* the key's down code, or 0 for a key the unit does not have */
static int tns_code_named(const char *name)
{
    unsigned i;
    for (i = 0; i < sizeof TNS_KEYS / sizeof TNS_KEYS[0]; i++)
        if (!strcmp(TNS_KEYS[i].name, name))
            return TNS_KEYS[i].code;
    return 0;
}

#endif
