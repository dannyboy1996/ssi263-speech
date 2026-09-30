/* tns_keymap_win.c -- see tns_keymap_win.h. */
#include "tns_keymap_win.h"

typedef struct { int vk; unsigned char code; } key_code;

static const key_code KEYS[] = {
    {'A', 0x94}, {'B', 0xB3}, {'C', 0xB2}, {'D', 0xB4}, {'E', 0xB0}, {'F', 0xBC}, {'G', 0xC0}, {'H', 0xC8},
    {'I', 0xCD}, {'J', 0xC4}, {'K', 0xCC}, {'L', 0xD4}, {'M', 0xC3}, {'N', 0xBB}, {'O', 0xD5}, {'P', 0xDD},
    {'Q', 0x90}, {'R', 0xB8}, {'S', 0xAC}, {'T', 0xB5}, {'U', 0xC5}, {'V', 0xAB}, {'W', 0x98}, {'X', 0xAA},
    {'Y', 0xBD}, {'Z', 0x92},
    {'1', 0x8B}, {'2', 0x8D}, {'3', 0x95}, {'4', 0xAD}, {'5', 0x97}, {'6', 0xAF}, {'7', 0xB7}, {'8', 0xBF},
    {'9', 0xC7}, {'0', 0xCF},
    {VK_SPACE, 0xA9}, {VK_OEM_PERIOD, 0xC2}, {VK_OEM_COMMA, 0xCB}, {VK_OEM_2, 0xCA}, {VK_OEM_1, 0xDC},
    {VK_OEM_7, 0xD3}, {VK_OEM_MINUS, 0xD7}, {VK_OEM_PLUS, 0xDF}, {VK_OEM_4, 0xD0}, {VK_OEM_6, 0xD8},
    {VK_OEM_5, 0xE0},
    {VK_F1, 0x8A}, {VK_F2, 0x8C}, {VK_F3, 0x8F}, {VK_F4, 0x8E}, {VK_F5, 0x96}, {VK_F6, 0xAE}, {VK_F7, 0xB6},
    {VK_F8, 0xBE}, {VK_F9, 0xC6}, {VK_F10, 0xCE},
    {VK_CONTROL, 0x81}, {VK_CAPITAL, 0x93},
    {VK_ESCAPE, 0x89}, {VK_DELETE, 0xC9}, {VK_INSERT, 0xC1}, {VK_RETURN, 0xDB}, {VK_TAB, 0x91}, {VK_BACK, 0xED},
    {VK_NUMLOCK, 0xD6}, {VK_SCROLL, 0xDE}, {VK_SNAPSHOT, 0xEE}, {VK_PAUSE, 0xEF}, {VK_CANCEL, 0xEF},
    {VK_LEFT, 0xD1}, {VK_RIGHT, 0xE9}, {VK_UP, 0xDA}, {VK_DOWN, 0xD9}, {VK_PRIOR, 0xEC}, {VK_NEXT, 0xEB},
    {VK_HOME, 0xF0}, {VK_END, 0xEA},
};

int tns_code_for_key(WPARAM vk, LPARAM lp)
{
    int scan = (int)((lp >> 16) & 0xFF), extended = (int)((lp >> 24) & 1);
    unsigned i;
    if (vk == VK_SHIFT)
        return scan == 0x36 ? 0xE2 : 0xE1;       /* right shift, left shift */
    if (vk == VK_MENU)
        return extended ? 0xB9 : 0xA1;           /* right alt, left alt */
    for (i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (KEYS[i].vk == (int)vk)
            return KEYS[i].code;
    return 0;
}
