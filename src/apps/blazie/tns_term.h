/* tns_term.h -- keys (keys.h) as the Type 'n Speak's key events (tns_board.h: bit 7 = down), with tns_keys.h's
 * codes.  Portable C.
 *
 * From a terminal a key is only pressed, so it becomes its whole stroke: the modifiers it needs down, the key down
 * and up, the modifiers up -- 'A' is left Shift down, a down, a up, Shift up; Ctrl+O is Ctrl, o; Alt+x is left Alt,
 * x; '!' is Shift and 1 (a US keyboard's shifted characters).  From an input device each key goes down and up as
 * the hand moves it.
 */
#ifndef BLAZIE_TNS_TERM_H
#define BLAZIE_TNS_TERM_H

#include "keys.h"

/* the unit's down code for a key (an input device's: letters lowercase, shift and the others keys of their own); 0
   for a key the unit does not have */
int tns_code_of(int key);
/* a terminal's key press as the unit's key events, into codes (cap at least 8); how many (0: not the unit's key) */
int tns_press_codes(const key_event *e, unsigned char *codes, int cap);

#endif
