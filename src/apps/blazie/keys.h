/* keys.h -- a key, as the shells see it before it reaches a unit: the terminal's bytes (term_keys.c) and the Linux
 * input devices (evdev_linux.c) both become key_events, which the Braille Lite's chords (bl_keys.c) and the Type 'n
 * Speak's key codes (tns_term.c) are made from.  Portable C, no platform calls.
 *
 * A key is a character (1-255: as typed, 'f', 'F', ';', ' ') or a named key (K_*, from 0x100).  From a terminal a
 * key is only ever PRESSED: the terminal says when a key goes down (and auto-repeats it), never when it comes up.
 * From an input device (evdev) keys go DOWN and come UP, each physical key on its own (shift is a key there, and a
 * letter is its lowercase character).
 *
 * Names (the settings file's): a character stands for itself (letters in lowercase: "f", ";", "/"), or "space",
 * "enter", "tab", "backspace", "esc", "up", "down", "left", "right", "home", "end", "pageup", "pagedown",
 * "insert", "delete", "f1".."f12", "lshift", "rshift", "lctrl", "rctrl", "lalt", "ralt", "capslock", "numlock",
 * "scrolllock", "print", "pause", "brl_dot1".."brl_dot8" (a braille keyboard's own keys, as Linux names them), or
 * "code:N" (an input device's key code N).  Modifiers before a name: "ctrl-o", "alt-shift-f".
 */
#ifndef BLAZIE_KEYS_H
#define BLAZIE_KEYS_H

enum {
    K_ENTER = 0x100, K_TAB, K_BACKSPACE, K_ESC, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN,
    K_INSERT, K_DELETE,
    K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,
    K_LSHIFT, K_RSHIFT, K_LCTRL, K_RCTRL, K_LALT, K_RALT, K_CAPS, K_NUMLOCK, K_SCROLL, K_PRINT, K_PAUSE,
    K_BRL1, K_BRL2, K_BRL3, K_BRL4, K_BRL5, K_BRL6, K_BRL7, K_BRL8,
    K_NAMED_END,
    K_CODE = 0x1000             /* K_CODE + n: an input device's key code n with no name here */
};

/* modifiers, as xterm numbers them (its parameter minus 1) */
#define KM_SHIFT 1
#define KM_ALT 2
#define KM_CTRL 4

enum { KE_PRESS, KE_DOWN, KE_UP };

typedef struct {
    int key;                    /* a character or K_* */
    int mods;                   /* KM_*: held with the key (from an input device the modifier keys also come as
                                   events of their own) */
    int type;                   /* KE_PRESS (a terminal), KE_DOWN or KE_UP (an input device) */
} key_event;

/* a key and its modifiers from a name ("f", "space", "ctrl-o", "f11", "code:57"); 0 for an unknown name */
int key_parse(const char *name, int *mods);
/* the key's name, as key_parse reads it (without modifiers); out holds at least 16 */
const char *key_name(int key, char *out);
/* the same key whatever the case: letters in lowercase */
int key_fold(int key);

/* a list of keys with their modifiers, from a setting ("f brl_dot1", "f12 ctrl-k"); how many (at most max) */
typedef struct { int key, mods; } key_combo;
int key_list(const char *setting, key_combo *out, int max);
/* the event is one of the list (a character compared without case; the modifiers exactly) */
int key_in_list(const key_event *e, const key_combo *list, int n);

#endif
