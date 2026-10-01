/* tns_keymap_win.c -- see tns_keymap_win.h.  The codes are tns_keys.h's (shared with the Linux shell); here only
   which Windows virtual key is which of its keys. */
#include "tns_keymap_win.h"
#include "tns_keys.h"

typedef struct { int vk; const char *name; } vk_name;

static const vk_name VKS[] = {
    {VK_SPACE, "space"}, {VK_OEM_PERIOD, "."}, {VK_OEM_COMMA, ","}, {VK_OEM_2, "/"}, {VK_OEM_1, ";"},
    {VK_OEM_7, "'"}, {VK_OEM_MINUS, "-"}, {VK_OEM_PLUS, "="}, {VK_OEM_4, "["}, {VK_OEM_6, "]"},
    {VK_OEM_5, "\\"},
    {VK_F1, "f1"}, {VK_F2, "f2"}, {VK_F3, "f3"}, {VK_F4, "f4"}, {VK_F5, "f5"}, {VK_F6, "f6"}, {VK_F7, "f7"},
    {VK_F8, "f8"}, {VK_F9, "f9"}, {VK_F10, "f10"},
    {VK_CONTROL, "ctrl"}, {VK_CAPITAL, "capslock"},
    {VK_ESCAPE, "esc"}, {VK_DELETE, "delete"}, {VK_INSERT, "insert"}, {VK_RETURN, "enter"}, {VK_TAB, "tab"},
    {VK_BACK, "backspace"},
    {VK_NUMLOCK, "numlock"}, {VK_SCROLL, "scrolllock"}, {VK_SNAPSHOT, "print"}, {VK_PAUSE, "pause"},
    {VK_CANCEL, "pause"},                   /* Ctrl+Pause arrives as VK_CANCEL */
    {VK_LEFT, "left"}, {VK_RIGHT, "right"}, {VK_UP, "up"}, {VK_DOWN, "down"}, {VK_PRIOR, "pageup"},
    {VK_NEXT, "pagedown"}, {VK_HOME, "home"}, {VK_END, "end"},
};

int tns_code_for_key(WPARAM vk, LPARAM lp)
{
    int scan = (int)((lp >> 16) & 0xFF), extended = (int)((lp >> 24) & 1);
    char name[2];
    unsigned i;
    if (vk == VK_SHIFT)
        return tns_code_named(scan == 0x36 ? "rshift" : "lshift");
    if (vk == VK_MENU)
        return tns_code_named(extended ? "ralt" : "lalt");
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        name[0] = (char)(vk >= 'A' ? vk - 'A' + 'a' : vk);
        name[1] = 0;
        return tns_code_named(name);
    }
    for (i = 0; i < sizeof VKS / sizeof VKS[0]; i++)
        if (VKS[i].vk == (int)vk)
            return tns_code_named(VKS[i].name);
    return 0;
}
