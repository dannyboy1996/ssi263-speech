/* term_keys.h -- a terminal's bytes as keys (keys.h): what a key sends in xterm, the Linux console, VTE (GNOME,
 * MATE), tmux and screen.  Portable C: the shell reads the bytes, this only decodes them.
 *
 *   a printable character          that character, as typed ('f', 'F', '!')
 *   Ctrl+letter (01h-1Ah)          the letter with KM_CTRL; but 09h is Tab, 0Dh and 0Ah Enter, 08h and 7Fh Backspace
 *   ESC then a character           the character with KM_ALT (Alt+Shift+F: 'F' with KM_ALT)
 *   ESC [ ... / ESC O ...          arrows, Home/End, Insert/Delete, Page Up/Down, F1-F12, with their modifiers
 *   ESC alone                      Esc -- once TERM_ESC_S has passed with nothing after it (term_dec_flush)
 *
 * UTF-8 beyond ASCII and sequences not listed are dropped.
 */
#ifndef BLAZIE_TERM_KEYS_H
#define BLAZIE_TERM_KEYS_H

#include "keys.h"

#define TERM_ESC_S 0.03                 /* a lone ESC waits this long for the rest of a sequence */

typedef struct {
    unsigned char buf[32];              /* the start of a sequence, not yet complete */
    int n;
    double since;                       /* when it began */
    int utf8;                           /* continuation bytes still to skip */
} term_dec;

void term_dec_init(term_dec *d);
/* the keys in `bytes` (n of them, read at time `now` in seconds), into out; how many (at most cap) */
int term_dec_feed(term_dec *d, const unsigned char *bytes, int n, double now, key_event *out, int cap);
/* an incomplete sequence older than TERM_ESC_S, as keys (a lone ESC is Esc; ESC x is Alt+x) */
int term_dec_flush(term_dec *d, double now, key_event *out, int cap);
/* when term_dec_flush has something to do; -1: nothing waits */
double term_dec_deadline(const term_dec *d);

#endif
